/*
 *  Charm++ support for array section multicast and reduction
 *
 *  written by Gengbin Zheng,   gzheng@uiuc.edu
 *  on 12/2001
 *
 *  features:
 *     using a spanning tree (factor defined in CkSectionID)
 *     support pipelining via fragmentation  (SPLIT_MULTICAST)
 *     support *any-time* migration, spanning tree will be rebuilt automatically
 * */

#include "charm++.h"
#include "envelope.h"
#include "register.h"

#include "ckmulticast.h"
#include "spanningTree.h"
#include "XArraySectionReducer.h"

#include <map>
#include <vector>
#include <unordered_map>

#define DEBUGF(x)  // CkPrintf x;

// turn on or off fragmentation in multicast
#define SPLIT_MULTICAST  1

// maximum number of fragments into which a message can be broken
// NOTE: CkReductionMsg::{nFrags,fragNo} and sectionRedInfo::npProcessed are int8_t,
//       which has a maximum value of 127.
#define MAXFRAGS 100

typedef CkQ<multicastGrpMsg *> multicastGrpMsgBuf;
typedef std::vector<CkArrayIndex> arrayIndexList;
typedef std::vector<int> groupPeList;
typedef std::vector<CkSectionInfo> sectionIdList;
typedef std::vector<CkReductionMsg *> reductionMsgs;
typedef CkQ<int> PieceSize;
typedef std::vector<CmiUInt8> ObjKeyList;
typedef unsigned char byte;

/** Information about the status of reductions proceeding along a given section
 *
 * An instance of this class is stored in every mCastEntry object making it possible
 * to track redn operations on a per section basis all along the spanning tree.
 */
class sectionRedInfo {
    public:
        /// Number of local array elements which have contributed a given fragment
        int lcount [MAXFRAGS];
        /// Number of child vertices (NOT array elements) that have contributed a given fragment
        int ccount [MAXFRAGS];
        /// The total number of array elements that have contributed so far to a given fragment
        int gcount [MAXFRAGS];
        /// The number of fragments processed so far
        int8_t npProcessed;
        /// User callback
        CkCallback *storedCallback;
        /// User reduction client function
        redClientFn storedClient;
        /// User provided data for the redn client function
        void *storedClientParam;
        /// Reduction sequence number
        int redNo;
        /// Root only: a fragment of this reduction was completed by
        /// contributions that did not all come through this tree
        int8_t mixed;
        /// Messages for this reduction
        reductionMsgs  msgs [MAXFRAGS];
        /// Messages of future reductions
        reductionMsgs futureMsgs;

    public:
        sectionRedInfo(): npProcessed(0),
                         storedCallback(NULL),
                         storedClientParam(NULL),
                         redNo(0), mixed(0) {
            for (int8_t i=0; i<MAXFRAGS; i++)
                lcount [i] = ccount [i] = gcount [i] = 0;
        }
};

/// cookie status
#define COOKIE_NOTREADY 0
#define COOKIE_READY    1
#define COOKIE_OLD      2




class mCastPacket {
public:
  CkSectionInfo cookie;
  int offset;
  int n;
  std::vector<char> data;
  int seqno;
  int count;
  int totalsize;

  mCastPacket(CkSectionInfo &_cookie, int _offset, int _n, char *_d, int _s, int _c, int _t):
              cookie(_cookie), offset(_offset), n(_n), data(_d, _d+_n), seqno(_s), count(_c), totalsize(_t) {}
};

typedef CkQ<mCastPacket *> multicastGrpPacketBuf;

class SectionLocation {
public:
  mCastEntry *entry;
  int         pe;
public:
  SectionLocation(): entry(NULL), pe(-1) {}
  SectionLocation( mCastEntry *e, int p) { set(e, p); }
  inline void set(mCastEntry *e, int p) { entry = e; pe = p; }
  inline void clear() { entry = NULL; pe = -1; }
};




/// Cookie for an array section 
class mCastEntry 
{
    public:
        /// Array ID 
        CkArrayID     aid;
        /// Spanning tree parent
        CkSectionInfo parentGrp;
        /// List of direct children
        sectionIdList children;
        /// branching factor for spanning tree
        int bfactor;
        /// Number of direct children
        int numChild;
        /// List of all tree member array indices (Only useful on the tree root)
        arrayIndexList allElem;
        /// List of all tree member PE's (Only useful on the tree root (for group sections))
        groupPeList allGrpElem;
        /// Only useful on root for LB
        ObjKeyList     allObjKeys;
        /// List of array elements on local PE
        arrayIndexList localElem;
        /// List of group elements on local PE(either 0 or 1 elems)
        bool localGrpElem;
        /// Should always be myPE
        int pe;
        /// Section ID of the root
        CkSectionInfo rootSid;
        multicastGrpMsgBuf msgBuf;
        /// Buffer storing the pending packets
        multicastGrpPacketBuf packetBuf;
        /// For multicast packetization
        char *asm_msg;
        int   asm_fill;
        /// Linked list of entries on the same processor
        mCastEntry *oldc, *newc;
        /// Old spanning tree
        SectionLocation   oldtree;
        // for reduction
        sectionRedInfo red;
        //
        char needRebuild;
    private:
        char flag;
	char grpSec;
    public:
        mCastEntry(CkArrayID _aid): aid(_aid), numChild(0), localGrpElem(0), asm_msg(NULL),
                   asm_fill(0), oldc(NULL), newc(NULL), needRebuild(0),
                   flag(COOKIE_NOTREADY), grpSec(0) {}
        mCastEntry(CkGroupID _gid): aid(_gid), numChild(0), localGrpElem(0), asm_msg(NULL),
                   asm_fill(0), oldc(NULL), newc(NULL), needRebuild(0),
                   flag(COOKIE_NOTREADY), grpSec(1) {}
        mCastEntry(mCastEntry *);
        /// Check if this tree is only a branch and has a parent
        inline int hasParent() { return parentGrp.get_val()?1:0; }
        /// Is this tree obsolete?
        inline int isObsolete() { return (flag == COOKIE_OLD); }
        /// Make the current tree obsolete
        inline void setObsolete() { flag=COOKIE_OLD; }
        /// Check if this (branch of the) tree is ready for use
        inline int notReady() { return (flag == COOKIE_NOTREADY); }
        /// Mark this (branch of the) tree as ready for use
        inline void setReady() { flag=COOKIE_READY; }
        /// Is this a group section
        inline int isGrpSec() {  return grpSec; }
        inline int getNumLocalElems(){
            return (isGrpSec()? localGrpElem : localElem.size());
        }
	inline void setLocalGrpElem() { localGrpElem = 1; }
        inline int getNumAllElems(){
            return (isGrpSec()? allGrpElem.size() : allElem.size());
        }
        /// Increment the reduction number across the whole linked list of cookies
        inline void incReduceNo() {
            red.redNo ++;
            for (mCastEntry *next = newc; next; next=next->newc) 
                next->red.redNo++;
        }
        /// Get a handle on the array ID this tree is a member of
        inline CkArrayID getAid() { return aid; }
        inline int hasOldtree() { return oldtree.entry != NULL; }
        inline void print() {
            CmiPrintf("[%d] mCastEntry: %p, numChild: %d pe: %d flag: %d asm_msg:%p asm_fill:%d\n", CkMyPe(), (void *)this, numChild, pe, flag, (void *)asm_msg, asm_fill);
        }
};




class cookieMsg: public CMessage_cookieMsg {
public:
  CkSectionInfo cookie;
public:
  cookieMsg() {};
  cookieMsg(CkSectionInfo m): cookie(m) {};
};




/**
 * Multicast tree setup message.
 * Message is directed to a set of PEs with the purpose of building a spanning
 * tree of them.
 */
class multicastSetupMsg: public CMessage_multicastSetupMsg {
public:
  /**
   * number of PEs in this subtree
   */
  int  nIdx;
  /**
   * for array sections: list of array section elements in this subtree
   * for group sections: NULL
   */
  CkArrayIndex *arrIdx;
  /**
   * for array sections:
   * peElems contains list of PEs that are in this subtree, and
   * for each PE, the index to its array section elements, i.e:
   * peElems[2*i] is the i-th PE
   * peElems[2*i+1] is the index in arrIdx of the first element in i-th PE
   *
   * for group sections: list of PEs in this subtree
   */
  int *peElems;
  CkSectionInfo parent;
  CkSectionInfo rootSid;
  int redNo;
  int forGrpSec(){
    CkAssert(nIdx);
    return ((void *)arrIdx == (void *)peElems);
  }
  int bfactor;
};




/// message send in spanning tree
class multicastGrpMsg: public CkMcastBaseMsg, public CMessage_multicastGrpMsg {
};


extern void CkPackMessage(envelope **pEnv);
extern void CkUnpackMessage(envelope **pEnv);



void _ckMulticastInit(void)
{
/*
  CkDisableTracing(CkIndex_CkMulticastMgr::recvMsg(0));
  CkDisableTracing(CkIndex_CkMulticastMgr::recvRedMsg(0));
*/
}


mCastEntry::mCastEntry (mCastEntry *old): 
  numChild(0), oldc(NULL), newc(NULL), flag(COOKIE_NOTREADY), grpSec(old->isGrpSec())
{
  aid = old->aid;
  parentGrp = old->parentGrp;
  allElem = old->allElem;
  allGrpElem = old->allGrpElem;
#if CMK_LBDB_ON
  allObjKeys = old->allObjKeys;
#endif
  pe = old->pe;
  red.storedCallback = old->red.storedCallback;
  red.storedClient = old->red.storedClient;
  red.storedClientParam = old->red.storedClientParam;
  red.redNo = old->red.redNo;
  needRebuild = 0;
  asm_msg = NULL;
  asm_fill = 0;
}



void CkMulticastMgr::setSection(CkSectionInfo &_id, CkArrayID aid, CkArrayIndex *al, int n)
{
    setSection(_id, aid, al, n, dfactor);
}

void CkMulticastMgr::setSection(CkSectionInfo &_id, CkArrayID aid, CkArrayIndex *al, int n, int factor)
{
    // Create a multicast entry
    mCastEntry *entry = new mCastEntry(aid);
    // Push all the section member indices into the entry
    entry->allElem.resize(n);
    entry->allObjKeys.reserve(n);
    for (int i=0; i<n; i++) {
        entry->allElem[i] = al[i];
#if CMK_LBDB_ON
        CmiUInt8 _key;
        if(CProxy_ArrayBase(aid).ckLocMgr()->lookupID(al[i], _key))
            entry->allObjKeys.push_back(_key);
#endif
    }
    entry->allObjKeys.shrink_to_fit();
    entry->bfactor = factor;
    //  entry->aid = aid;
    _id.get_aid() = aid;
    _id.get_val() = entry;		// allocate table for this section
    // 
    initCookie(_id);
}




void CkMulticastMgr::setSection(CkSectionInfo &id)
{
  initCookie(id);
}




/// @warning: This is deprecated
void CkMulticastMgr::setSection(CProxySection_ArrayElement &proxy)
{
  CkArrayID aid = proxy.ckGetArrayID();
  CkSectionInfo &_id = proxy.ckGetSectionInfo();

  mCastEntry *entry = new mCastEntry(aid);

  const CkArrayIndex *al = proxy.ckGetArrayElements();
  entry->allElem.resize(proxy.ckGetNumElements());
  entry->allObjKeys.reserve(proxy.ckGetNumElements());
  for (int i=0; i<proxy.ckGetNumElements(); i++) {
    entry->allElem[i] = al[i];
#if CMK_LBDB_ON
    CmiUInt8 _key;
    if(CProxy_ArrayBase(aid).ckLocMgr()->lookupID(al[i], _key))
      entry->allObjKeys.push_back(_key);
#endif
  }
  entry->allObjKeys.shrink_to_fit();
  if(proxy.ckGetBfactor() == USE_DEFAULT_BRANCH_FACTOR)
    entry->bfactor = dfactor;
  else
    entry->bfactor = proxy.ckGetBfactor();
  _id.get_aid() = aid;
  _id.get_val() = entry;		// allocate table for this section
  initCookie(_id);
}




void CkMulticastMgr::resetSection(CProxySection_ArrayBase &proxy)
{
  CkSectionInfo &info = proxy.ckGetSectionInfo();

  int oldpe = info.get_pe();
  if (oldpe == CkMyPe()) return;	// we don't have to recreate one

  CkArrayID aid = proxy.ckGetArrayID();
  CkSectionID *sid = proxy.ckGetSectionIDs();
  mCastEntry *entry = new mCastEntry(aid);

  mCastEntry *oldentry = (mCastEntry *)info.get_val();
  DEBUGF(("[%d] resetSection: old entry:%p new entry:%p\n", CkMyPe(), oldentry, entry));

  const std::vector<CkArrayIndex> &al = sid->_elems;
  CmiAssert(info.get_aid() == (CkGroupID)aid);
  prepareCookie(entry, *sid, al.data(), sid->_elems.size(), aid);

  CProxy_CkMulticastMgr  mCastGrp(thisgroup);

    // store old tree info
  entry->oldtree.set(oldentry, oldpe);

  // retire the old tree and fetch its reduction number, in one message
  mCastGrp[oldpe].retrieveCookie(CkSectionInfo(oldpe, oldentry, 0, aid), info);
}


/// Build a mCastEntry object with relevant section info and set the section cookie to point to this object
void CkMulticastMgr::prepareCookie(mCastEntry *entry, CkSectionID &sid, const CkArrayIndex *al, int count, CkArrayID aid)
{
  entry->allElem.resize(count);
  entry->allObjKeys.reserve(count);
  for (int i=0; i<count; i++) {
    entry->allElem[i] = al[i];
#if CMK_LBDB_ON
    CmiUInt8 _key;
    if(CProxy_ArrayBase(aid).ckLocMgr()->lookupID(al[i], _key))
      entry->allObjKeys.push_back(_key);
#endif
  }
  entry->allObjKeys.shrink_to_fit();
  if(sid.bfactor == USE_DEFAULT_BRANCH_FACTOR)
    entry->bfactor = dfactor;
  else
    entry->bfactor = sid.bfactor;

  sid._cookie.get_aid() = aid;
  sid._cookie.get_val() = entry;	// allocate table for this section
  sid._cookie.get_pe() = CkMyPe();
}


/// similar to prepareCookie, but for group sections
void CkMulticastMgr::prepareGrpCookie(mCastEntry *entry, CkSectionID &sid, const int *pelist, int count, CkGroupID gid)
{
  entry->allGrpElem.resize(count);
  for (int i=0; i<count; i++) {
    entry->allGrpElem[i] = pelist[i];
  }

  if(sid.bfactor == USE_DEFAULT_BRANCH_FACTOR)
    entry->bfactor = dfactor;
  else
    entry->bfactor = sid.bfactor;

  sid._cookie.get_aid() = gid;
  sid._cookie.get_val() = entry;  // allocate table for this section
  sid._cookie.get_pe() = CkMyPe();
  DEBUGF(("[%d]In prepareGrpCookie: entry: %p, entry->isGrpSec(): %d\n", CkMyPe(), entry, entry->isGrpSec()));
  CkAssert(entry->isGrpSec());
}


// this is used
void CkMulticastMgr::initDelegateMgr(CProxy *cproxy, int opts)
{
  if(opts == GROUP_SECTION_PROXY){
      initGrpDelegateMgr((CProxySection_Group *) cproxy, opts);
      return;
  }

  CProxySection_ArrayBase *proxy = (CProxySection_ArrayBase *)cproxy;
  int numSubSections = proxy->ckGetNumSubSections();
  CkCallback *sectionCB;
  // If its a cross-array section,
  if (numSubSections > 1)
  {
      /** @warning: Each instantiation is a mem leak! :o
       * The class is trivially small, but there's one instantiation for each
       * section delegated to CkMulticast. The objects need to live as long as
       * their section exists and is used. The idea of 'destroying' an array
       * section is still academic, and hence no effort has been made to charge
       * some 'owner' entity with the task of deleting this object.
       *
       * Reimplementing delegated x-array reductions will make this consideration moot
       */
      // Configure the final cross-section reducer
      ck::impl::XArraySectionReducer *red =
          new ck::impl::XArraySectionReducer(numSubSections, nullptr);
      // Configure the subsection callback to deposit with the final reducer
      sectionCB = new CkCallback(ck::impl::processSectionContribution, red);
  }
  for (int i=0; i<numSubSections; i++)
  {
      CkArrayID aid = proxy->ckGetArrayIDn(i);
      mCastEntry *entry = new mCastEntry(aid);
      CkSectionID *sid = &( proxy->ckGetSectionID(i) );
      const CkArrayIndex *al = proxy->ckGetArrayElements(i);
      if (numSubSections > 1)
          entry->red.storedCallback = sectionCB;
      prepareCookie(entry, *sid, al, proxy->ckGetNumElements(i), aid);
      initCookie(sid->_cookie);
  }
}


//similar to initDelegateMgr, but for groupsections
void CkMulticastMgr::initGrpDelegateMgr(CProxySection_Group *proxy, int opts)
{
  int numSubSections = proxy->ckGetNumSections();
  for (int i=0; i<numSubSections; i++)
  {
      CkGroupID gid = proxy->ckGetGroupIDn(i);
      mCastEntry *entry = new mCastEntry(gid);
      CkSectionID *sid = &( proxy->ckGetSectionID(i) );
      const int *pelist = proxy->ckGetElements(i);
      prepareGrpCookie(entry, *sid, pelist, proxy->ckGetNumElements(i), gid);
      initGrpCookie(sid->_cookie);
  }
}


void CkMulticastMgr::retrieveCookie(CkSectionInfo s, CkSectionInfo srcInfo)
{
  mCastEntry *entry = (mCastEntry *)s.get_val();
  // Retire first: an obsolete root forwards contributions to the new root,
  // so its redNo no longer advances and the value sent below is final.
  retire(s, srcInfo);
  CProxy_CkMulticastMgr  mCastGrp(thisgroup);
  mCastGrp[srcInfo.get_pe()].recvCookieInfo(srcInfo, entry->red.redNo);
}

// now that we get reduction number from the old cookie,
// we continue to build the spanning tree
void CkMulticastMgr::recvCookieInfo(CkSectionInfo s, int red)
{
  mCastEntry *entry = (mCastEntry *)s.get_val();
  entry->red.redNo = red;

  initCookie(s);

  // TODO delete old tree
}




void CkMulticastMgr::initCookie(CkSectionInfo s)
{
  mCastEntry *entry = (mCastEntry *)s.get_val();
  const int n = entry->allElem.size();
  DEBUGF(("init: %d elems %p at %f\n", n, s.get_val(), CkWallTimer()));
  // might want to consider using unordered_map, but some spanning tree
  // algorithms could rely on initial list of PEs being ordered
  std::map<int, std::vector<int>> elemBins;
  CkArray *array = CProxy_ArrayBase(s.get_aid()).ckLocalBranch();
  for (int i=0; i < n; i++) {
    int ape = array->lastKnown(entry->allElem[i]);
    CmiAssert(ape >=0 && ape < CkNumPes());
    elemBins[ape].push_back(i);
  }
  // Create and initialize a setup message
  multicastSetupMsg *msg = new (n, (elemBins.size()+1)*2, 0) multicastSetupMsg;
  msg->nIdx = elemBins.size();
  msg->parent = CkSectionInfo(entry->getAid());
  msg->rootSid = s;
  msg->redNo = entry->red.redNo;
  msg->bfactor = entry->bfactor;
  int cntElems=0, idx=0;
  for (std::map<int, std::vector<int> >::iterator itr = elemBins.begin();
       itr != elemBins.end(); ++itr) {
    msg->peElems[idx++] = itr->first;
    msg->peElems[idx++] = cntElems;
    std::vector<int> &elems = itr->second;
    for (int j=0; j < elems.size(); j++) {
      msg->arrIdx[cntElems++] = entry->allElem[elems[j]];
    }
  }
  msg->peElems[idx++] = -1;
  msg->peElems[idx] = cntElems;
  // Trigger the spanning tree build
  CProxy_CkMulticastMgr  mCastGrp(thisgroup);
  mCastGrp[CkMyPe()].setup(msg);
}


//similar to initCookie, but for group section
void CkMulticastMgr::initGrpCookie(CkSectionInfo s)
{
   mCastEntry *entry = (mCastEntry *)s.get_val();
   int n = entry->allGrpElem.size();
   DEBUGF(("init: %d elems %p\n", n, s.get_val()));
   // Create and initialize a setup message
   multicastSetupMsg *msg = new (0, n, 0) multicastSetupMsg;
   DEBUGF(("[%d] initGrpCookie: msg->arrIdx: %p\n", CkMyPe(), msg->arrIdx));
   msg->nIdx = n;
   msg->parent = CkSectionInfo(entry->getAid());
   msg->rootSid = s;
   msg->redNo = entry->red.redNo;
   msg->bfactor = entry->bfactor;
   // Fill the message with the section member indices and their last known locations
   for (int i=0; i<n; i++) {
     msg->peElems[i] = entry->allGrpElem[i];
   }
   // Trigger the spanning tree build
   CProxy_CkMulticastMgr  mCastGrp(thisgroup);
   mCastGrp[CkMyPe()].setup(msg);
}




void CkMulticastMgr::teardown(CkSectionInfo cookie)
{
    int i;
    mCastEntry *sect = (mCastEntry *)cookie.get_val();
    // Mark this section as obsolete
    sect->setObsolete();
    // Release the buffered messages 
    releaseBufferedReduceMsgs(sect);
    // Propagate the teardown to each of your children
    CProxy_CkMulticastMgr mp(thisgroup);
    for (i=0; i<sect->children.size(); i++)
        mp[sect->children[i].get_pe()].teardown(sect->children[i]);
}




void CkMulticastMgr::retire(CkSectionInfo cookie, CkSectionInfo newroot)
{
    int i;
    mCastEntry *sect = (mCastEntry *)cookie.get_val();
    // Reset the root section info
    sect->rootSid = newroot;
    // Mark this section as obsolete
    sect->setObsolete();
    // Release the buffered messages 
    releaseBufferedReduceMsgs(sect);
    // Propagate the teardown to each of your children
    CProxy_CkMulticastMgr mp(thisgroup);
    for (i=0; i<sect->children.size(); i++)
        mp[sect->children[i].get_pe()].teardown(sect->children[i]);
}




void CkMulticastMgr::freeup(CkSectionInfo cookie)
{
  mCastEntry *sect = (mCastEntry *)cookie.get_val();
  CProxy_CkMulticastMgr mp(thisgroup);
  // Parse through all the section members on this PE and...
  while (sect) 
  {
      // Free their children
      for (int i=0; i<sect->children.size(); i++)
          mp[ sect->children[i].get_pe() ].freeup(sect->children[i]);
      // Free the cookie itself
      DEBUGF(("[%d] Free up on %p\n", CkMyPe(), sect));
      mCastEntry *oldc= sect->oldc;
      delete sect;
      sect = oldc;
  }
}


typedef std::vector<int>::iterator TreeIterator;

void CkMulticastMgr::setup(multicastSetupMsg *msg)
{
    mCastEntry *entry;
    CkArrayID aid = msg->rootSid.get_aid();
    if (msg->parent.get_pe() == CkMyPe()) 
      entry = (mCastEntry *)msg->rootSid.get_val(); //sid.val;
    else{
      if(msg->forGrpSec())
        entry = new mCastEntry((CkGroupID) aid);
      else
        entry = new mCastEntry(aid);
    } 
    entry->aid = aid;
    entry->pe = CkMyPe();
    /// A root retired before its own setup arrives keeps the new root as rootSid
    if (!entry->isObsolete()) entry->rootSid = msg->rootSid;
    entry->parentGrp = msg->parent;
    int factor = entry->bfactor = msg->bfactor;

    DEBUGF(("[%d] setup: %p redNo: %d => %d with %d elems, grpSec: %d, factor: %d\n", CkMyPe(), entry, entry->red.redNo, msg->redNo, msg->nIdx, entry->isGrpSec(), factor));
    entry->red.redNo = msg->redNo;

    const int numpes = msg->nIdx;
    std::unordered_map<int, int> peIdx;    // pe -> idx in msg->peElems
    if (!entry->isGrpSec()) peIdx.reserve(numpes);
    std::vector<int> mySubTreePEs;
    mySubTreePEs.reserve(numpes);
    // The first PE in my subtree should be me, the tree root (as required by the spanning tree builder)
    mySubTreePEs.push_back(CkMyPe());

    for (int i1=0; i1 < numpes; i1++) { // nIdx is now number of PEs
      if (entry->isGrpSec()) {
        // group sections
        if (msg->peElems[i1] != CkMyPe())
          mySubTreePEs.push_back(msg->peElems[i1]);
        else
          entry->setLocalGrpElem();
      } else {
        // array sections
        int i2 = i1*2;
        int pe = msg->peElems[i2];
        peIdx[pe] = i2;
        if (pe != CkMyPe()) {
          mySubTreePEs.push_back(pe);
        } else {
          int begin = msg->peElems[i2+1];
          int end = msg->peElems[i2+3];
          entry->localElem.reserve(entry->localElem.size() + (end - begin));
          for (int j=begin; j < end; j++)
            entry->localElem.push_back(msg->arrIdx[j]);
        }
      }
    }

    // The number of multicast children can be limited by the spanning tree factor 
    int num = mySubTreePEs.size() - 1, numchild = 0;
    if (factor <= 0) numchild = num;
    else numchild = num<factor?num:factor;
  
    entry->numChild = numchild;

    // If there are any children, go about building a spanning tree
    if (numchild) 
    {
        // Build the next generation of the spanning tree rooted at my PE
        bool isRoot = (msg->parent.get_pe() == CkMyPe());
        ST_RecursivePartition<TreeIterator> treeBuilder(false,!isRoot);
        numchild = treeBuilder.buildSpanningTree(mySubTreePEs.begin(), mySubTreePEs.end(), numchild);
        entry->numChild = numchild;

        CProxy_CkMulticastMgr  mCastGrp(thisgroup);

        // Ask each direct child to setup its subtree
        for (int i=0; i < numchild; i++)
        {
            TreeIterator subtreeStart = treeBuilder.begin(i), subtreeEnd = treeBuilder.end(i);

            // Find the total number of section member elements on this subtree
            int numSubTreeElems = 0;
            int numSubTreePes = treeBuilder.subtreeSize(i);
            multicastSetupMsg *m;

            if (entry->isGrpSec()) {
              m = new (0, numSubTreePes, 0) multicastSetupMsg;
            } else {
              for (TreeIterator j=subtreeStart; j != subtreeEnd; j++) {
                  int idx = peIdx[*j];
                  numSubTreeElems += (msg->peElems[idx+3] - msg->peElems[idx+1]);
              }
              m = new (numSubTreeElems, (numSubTreePes+1)*2, 0) multicastSetupMsg;
            }

            // Prepare the setup msg intended for the child
            m->parent = CkSectionInfo(aid, entry);
            m->nIdx = numSubTreePes;
            m->rootSid = msg->rootSid;
            m->redNo = msg->redNo;
            m->bfactor = msg->bfactor;

            // Give each child the number, indices and location of its children
            int cntElems = 0, i2 = 0;
            for (TreeIterator j=subtreeStart; j != subtreeEnd; j++)
            {
                int childPE = *j;
                m->peElems[i2++] = childPE;
                if (!entry->isGrpSec()) {
                  m->peElems[i2++] = cntElems;
                  int i1 = peIdx[childPE];  // get index to pe in parent message
                  for (int k=msg->peElems[i1+1]; k < msg->peElems[i1+3]; k++) {
                    m->arrIdx[cntElems++] = msg->arrIdx[k];
                  }
                }
            }
            if (!entry->isGrpSec()) {
              m->peElems[i2++] = -1;
              m->peElems[i2] = cntElems;
            }

            int childroot = *subtreeStart;
            DEBUGF(("[%d] call set up %d numelem:%d, bfactor: %d\n", CkMyPe(), childroot, numSubTreeElems, m->bfactor));
            // Send the message to the child
            mCastGrp[childroot].setup(m);
        }
    }
    // else, tell yourself that your children are ready
    else 
        childrenReady(entry);
    delete msg;
}




void CkMulticastMgr::childrenReady(mCastEntry *entry)
{
    CProxy_CkMulticastMgr  mCastGrp(thisgroup);
    if (entry->isObsolete()) {
        /// Retired while the tree was still being built: retire found no
        /// children to tear down, so tear them down now. The entry stays
        /// obsolete; the messages released below are forwarded to the new root.
        for (size_t i=0; i<entry->children.size(); i++)
            mCastGrp[entry->children[i].get_pe()].teardown(entry->children[i]);
    }
    else
        entry->setReady();

    DEBUGF(("[%d] childrenReady entry %p groupsection?: %d,  Arrayelems: %zu, GroupElems: %zu, redNo: %d\n", CkMyPe(), entry, entry->isGrpSec(), entry->allElem.size(), entry->allGrpElem.size(), entry->red.redNo));

    if (entry->hasParent()) 
        mCastGrp[entry->parentGrp.get_pe()].recvCookie(entry->parentGrp, CkSectionInfo(entry->getAid(), entry));
#if SPLIT_MULTICAST
    // clear packet buffer
    while (!entry->packetBuf.isEmpty()) 
    {
        mCastPacket *packet = entry->packetBuf.deq();
        packet->cookie.get_val() = entry;
        mCastGrp[CkMyPe()].recvPacket(packet->cookie, packet->offset, packet->n, packet->data.data(), packet->seqno, packet->count, packet->totalsize, 1);
        delete packet;
    }
#endif
    // clear msg buffer
    while (!entry->msgBuf.isEmpty()) 
    {
        multicastGrpMsg *newmsg = entry->msgBuf.deq();
        DEBUGF(("[%d] release buffer %p ep:%d, msg-used?: %d\n", CkMyPe(), newmsg, newmsg->ep, UsrToEnv(newmsg)->isUsed()));
        newmsg->_cookie.get_val() = entry;
        mCastGrp[CkMyPe()].recvMsg(newmsg);
    }
    // release reduction msgs
    releaseFutureReduceMsgs(entry);
}




void CkMulticastMgr::recvCookie(CkSectionInfo sid, CkSectionInfo child)
{
  mCastEntry *entry = (mCastEntry *)sid.get_val();
  entry->children.push_back(child);
  if (entry->children.size() == entry->numChild) {
    childrenReady(entry);
  }
}




// rebuild is called when root not migrated
// when rebuilding, all multicast msgs will be buffered.
void CkMulticastMgr::rebuild(CkSectionInfo &sectId)
{
  // tear down old tree
  mCastEntry *curCookie = (mCastEntry*)sectId.get_val();
  CkAssert(curCookie->pe == CkMyPe());
  // make sure I am the newest one
  while (curCookie->newc) curCookie = curCookie->newc;
  if (curCookie->isObsolete()) return;

  //CmiPrintf("tree rebuild\n");
  mCastEntry *newCookie = new mCastEntry(curCookie);  // allocate table for this section

  // build a chain
  newCookie->oldc = curCookie;
  curCookie->newc = newCookie;

  sectId.get_val() = newCookie;

  DEBUGF(("rebuild: redNo:%d oldc:%p newc;%p\n", newCookie->red.redNo, curCookie, newCookie));

  curCookie->setObsolete();

  resetCookie(sectId);
}

void CkMulticastMgr::resetCookie(CkSectionInfo s)
{
  mCastEntry *newCookie = (mCastEntry*)s.get_val();
  mCastEntry *oldCookie = newCookie->oldc;

  // get rid of old one
  DEBUGF(("reset: oldc: %p\n", oldCookie));
  CProxy_CkMulticastMgr  mCastGrp(thisgroup);
  int mype = CkMyPe();
  mCastGrp[mype].teardown(CkSectionInfo(mype, oldCookie, 0, oldCookie->getAid()));

  // build a new one
  initCookie(s);
}

void CkMulticastMgr::SimpleSend(int ep,void *m, CkArrayID a, CkSectionID &sid, int opts)
{
  DEBUGF(("[%d] SimpleSend: nElems: %zu\n", CkMyPe(), sid._elems.size()));
    // set an invalid cookie since we don't have it
  ((multicastGrpMsg *)m)->_cookie = CkSectionInfo(-1, NULL, 0, a);
  for (int i=0; i< sid._elems.size()-1; i++) {
     CProxyElement_ArrayBase ap(a, sid._elems[i]);
     void *newMsg=CkCopyMsg((void **)&m);
     ap.ckSend((CkArrayMessage *)newMsg,ep,opts|CK_MSG_LB_NOTRACE);
  }
  if (!sid._elems.empty()) {
     CProxyElement_ArrayBase ap(a, sid._elems[sid._elems.size()-1]);
     ap.ckSend((CkArrayMessage *)m,ep,opts|CK_MSG_LB_NOTRACE);
  }
}

void CkMulticastMgr::ArraySectionSend(CkDelegateData *pd,int ep,void *m, int nsid, CkSectionID *sid, int opts)
{
        DEBUGF(("ArraySectionSend\n"));

    for (int snum = 0; snum < nsid; snum++) {
        void *msgCopy = m;
        if (nsid - snum > 1)
            msgCopy = CkCopyMsg(&m);
        sendToSection(pd, ep, msgCopy, &(sid[snum]), opts);
    }
}


void CkMulticastMgr::GroupSectionSend(CkDelegateData *pd,int ep,void *m, int nsid, CkSectionID *sid)
{

  DEBUGF(("[%d] GroupSectionSend, nsid: %d \n", CkMyPe(), nsid));
  for (int snum = 0; snum < nsid; snum++) {
    void *msgCopy = m;
    if (nsid - snum > 1)
      msgCopy = CkCopyMsg(&m);
    DEBUGF(("GroupSectionSend, msg-used(m):%d, msg-used(msgCopy):%d\n", UsrToEnv(m)->isUsed(), UsrToEnv(msgCopy)->isUsed()));
    sendToSection(pd, ep, msgCopy, &(sid[snum]), 0);
  }
}

void CkMulticastMgr::sendToSection(CkDelegateData *pd,int ep,void *m, CkSectionID *sid, int opts)
{
  multicastGrpMsg *msg = (multicastGrpMsg *)m;
  msg->ep = ep;
  CkSectionInfo &s = sid->_cookie;
  mCastEntry *entry;

  // If this section is rooted at this PE
  if (s.get_pe() == CkMyPe()) {
    entry = (mCastEntry *)s.get_val();   
    if (NULL == entry)
      CmiAbort("Unknown array section, Did you forget to register the array section to CkMulticastMgr using setSection()?");

    // update entry pointer in case there is a newer one.
    if (entry->newc) {
      do { entry=entry->newc; } while (entry->newc);
      s.get_val() = entry;
    }

#if CMK_LBDB_ON
    if(!entry->isGrpSec()){
      // fixme: running obj?
      envelope *env = UsrToEnv(msg);
      const LDOMHandle &om = CProxy_ArrayBase(s.get_aid()).ckLocMgr()->getOMHandle();
      LBManagerObj()->MulticastSend(om,entry->allObjKeys.data(),entry->allObjKeys.size(),env->getTotalsize());
    }
#endif

    // The first time we need to rebuild the spanning tree, we do p2p sends to refresh lastKnown
    if (entry->needRebuild == 1) {
      msg->_cookie = s;
      SimpleSend(ep, msg, s.get_aid(), *sid, opts);
      entry->needRebuild = 2;
      return;
    }
    // else the second time, we just rebuild cos now we'll have all the lastKnown PEs
    else if (entry->needRebuild == 2) rebuild(s);
  }
  // else, if the root has migrated, we have a sub-optimal mcast
  else {
    // fixme - in this case, not recorded in LB
    CmiPrintf("Warning: Multicast not optimized after multicast root migrated. \n");
  }

  // update cookie
  msg->_cookie = s;

#if SPLIT_MULTICAST
  // split multicast msg into SPLIT_NUM copies
  envelope *env = UsrToEnv(m);
  CkPackMessage(&env);
  int totalsize = env->getTotalsize();
  int packetSize = 0;
  int totalcount = 0;
  if(totalsize < split_threshold){
    packetSize = totalsize;
    totalcount = 1;
  }else{
    packetSize = split_size;
    totalcount = totalsize/split_size;
    if(totalsize%split_size) totalcount++; 
    //packetSize = totalsize/SPLIT_NUM;
    //if (totalsize%SPLIT_NUM) packetSize ++;
    //totalcount = SPLIT_NUM;
  }
  CProxy_CkMulticastMgr  mCastGrp(thisgroup);
  int sizesofar = 0;
  char *data = (char*) env;
  if (totalcount == 1) {
    // If the root of this section's tree is on this PE, then just propagate msg
    if (s.get_pe() == CkMyPe()) {
      CkUnpackMessage(&env);
      msg = (multicastGrpMsg *)EnvToUsr(env);
      recvMsg(msg);
    }
    // else send msg to root of section's spanning tree
    else {
      CProxy_CkMulticastMgr  mCastGrp(thisgroup);
      msg = (multicastGrpMsg *)EnvToUsr(env);
      mCastGrp[s.get_pe()].recvMsg(msg);
    }
    return;
  }
  for (int i=0; i<totalcount; i++) {
    int mysize = packetSize;
    if (mysize + sizesofar > totalsize) {
      mysize = totalsize-sizesofar;
    }
    //CmiPrintf("[%d] send to %d : mysize: %d total: %d \n", CkMyPe(), s.get_pe(), mysize, totalsize);
    mCastGrp[s.get_pe()].recvPacket(s, sizesofar, mysize, data, i, totalcount, totalsize, 0);
    sizesofar += mysize;
    data += mysize;
  }
  CmiFree(env);
#else
  if (s.get_pe() == CkMyPe()) {
    DEBUGF((CkPrintf("sendToSection, msg-used :%d\n", UsrToEnv(msg)->isUsed()));
    recvMsg(msg);
  }
  else {
    CProxy_CkMulticastMgr  mCastGrp(thisgroup);
    mCastGrp[s.get_pe()].recvMsg(msg);
  }
#endif
}

void CkMulticastMgr::recvPacket(CkSectionInfo &&_cookie, int offset, int n, char *data, int seqno, int count, int totalsize, bool fromBuffer)
{
  int i;
  mCastEntry *entry = (mCastEntry *)_cookie.get_val();


  if (!fromBuffer && (entry->notReady() || !entry->packetBuf.isEmpty())) {
    entry->packetBuf.enq(new mCastPacket(_cookie, offset, n, data, seqno, count, totalsize));
//CmiPrintf("[%d] Buffered recvPacket: seqno: %d %d frombuf:%d empty:%d entry:%p\n", CkMyPe(), seqno, count, fromBuffer, entry->packetBuf.isEmpty(),entry);
    return;
  }

//CmiPrintf("[%d] recvPacket ready: seqno: %d %d buffer: %d entry:%p\n", CkMyPe(), seqno, count, fromBuffer, entry);

  // send to spanning tree children
  // can not optimize using list send because the difference in cookie
  CProxy_CkMulticastMgr  mCastGrp(thisgroup);
  for (i=0; i<entry->children.size(); i++) {
    mCastGrp[entry->children[i].get_pe()].recvPacket(entry->children[i], offset, n, data, seqno, count, totalsize, 0);
  }

  if (entry->asm_msg == NULL) {
    CmiAssert(entry->asm_fill == 0);
    entry->asm_msg = (char *)CmiAlloc(totalsize);
  }
  memcpy(entry->asm_msg+offset, data, n);
  entry->asm_fill += n;
  if (entry->asm_fill == totalsize) {
    CkUnpackMessage((envelope **)&entry->asm_msg);
    multicastGrpMsg *msg = (multicastGrpMsg *)EnvToUsr((envelope*)entry->asm_msg);
    msg->_cookie = _cookie;
//    mCastGrp[CkMyPe()].recvMsg(msg);
    sendToLocal(msg);
    entry->asm_msg = NULL;
    entry->asm_fill = 0;
  }
//  if (fromBuffer) delete [] data;
}

void CkMulticastMgr::recvMsg(multicastGrpMsg *msg)
{
  int i;
  CkSectionInfo &sectionInfo = msg->_cookie;
  mCastEntry *entry = (mCastEntry *)msg->_cookie.get_val();
  CmiAssert((CkGroupID)entry->getAid() == sectionInfo.get_aid());

  if (entry->notReady()) {
    DEBUGF(("entry not ready, enq buffer %p, msg-used?: %d\n", msg, UsrToEnv(msg)->isUsed()));
    entry->msgBuf.enq(msg);
    return;
  }

  // send to spanning tree children
  // can not optimize using list send because the difference in cookie
  CProxy_CkMulticastMgr  mCastGrp(thisgroup);
  for (i=0; i<entry->children.size(); i++) {
    multicastGrpMsg *newmsg = (multicastGrpMsg *)CkCopyMsg((void **)&msg);
    newmsg->_cookie = entry->children[i];
    mCastGrp[entry->children[i].get_pe()].recvMsg(newmsg);
  }

  sendToLocal(msg);
}

void CkMulticastMgr::sendToLocal(multicastGrpMsg *msg)
{
  int i;
  CkSectionInfo &sectionInfo = msg->_cookie;
  mCastEntry *entry = (mCastEntry *)msg->_cookie.get_val();
  CmiAssert((CkGroupID)entry->getAid() == sectionInfo.get_aid());
  CkGroupID aid = sectionInfo.get_aid();
  
  // send to local
  int nLocal;

  // if group section
  if(entry->isGrpSec()){
    nLocal = entry->localGrpElem;
    if(nLocal){
      DEBUGF(("[%d] send to local branch, GroupSection\n", CkMyPe()));
      CkAssert(nLocal == 1);
      CProxyElement_Group ap(aid, CkMyPe());
      if (ap.ckIsDelegated()) {
        CkGroupMsgPrep(msg->ep, msg, aid);
        (ap.ckDelegatedTo())->GroupSend(ap.ckDelegatedPtr(), msg->ep, msg, CkMyPe(), aid);
      }
      else{
        if (_entryTable[msg->ep]->noKeep)
          CkSendMsgBranchInline(msg->ep, msg, CkMyPe(), aid, 0);
        else
          CkSendMsgBranch(msg->ep, msg, CkMyPe(), aid,0);
      }
    }
    return;
  }

  // else if array section
  nLocal = entry->localElem.size();
  DEBUGF(("[%d] send to local %d elems, ArraySection\n", CkMyPe(), nLocal));
  for (i=0; i<nLocal-1; i++) {
    CProxyElement_ArrayBase ap(aid, entry->localElem[i]);
    multicastGrpMsg *newm = (multicastGrpMsg *)CkCopyMsg((void **)&msg);
    ap.ckSend((CkArrayMessage *)newm, msg->ep, CK_MSG_LB_NOTRACE);
  }
  if (nLocal) {
    CProxyElement_ArrayBase ap(aid, entry->localElem[nLocal-1]);
    ap.ckSend((CkArrayMessage *)msg, msg->ep, CK_MSG_LB_NOTRACE);
  }
  else {
    CkAssert (entry->rootSid.get_pe() == CkMyPe());
    delete msg;
  }
}



void CkGetSectionInfo(CkSectionInfo &id, void *msg)
{
  CkMcastBaseMsg *m = (CkMcastBaseMsg *)msg;
  if (!CkMcastBaseMsg::checkMagic(m)) {
    CmiPrintf("ERROR: This is not a CkMulticast message!\n");
    CmiAbort("Did you remember to do CkMulticast delegation, and inherit multicast message from CkMcastBaseMsg in correct order?");
  }
  // ignore invalid cookie sent by SimpleSend
  if (m->gpe() != -1) {
    id.get_pe() = m->gpe();
    id.get_val() = m->entry();
    id.get_aid() = m->_cookie.get_aid();
  }
  // note: retain old redNo
}

// Reduction

void CkMulticastMgr::setReductionClient(CProxySection_ArrayBase &proxy, CkCallback *cb)
{
  CkCallback *sectionCB;
  int numSubSections = proxy.ckGetNumSubSections();
  // If its a cross-array section,
  if (numSubSections > 1)
  {
    /** @warning: setReductionClient in cross section reduction should be phased out
     * Issue: https://github.com/charmplusplus/charm/issues/1249
     * Since finalCB is already set in initDelegateMgr it needs to be deleted here
     * There will still be a memory leak for the instantiation of XGroupSectionReducer object
     * Since this function will eventually be phased out, it is only a quick fix
     */
    // Configure the final cross-section reducer
    CkSectionInfo &sInfo = proxy.ckGetSectionID(0)._cookie;
    mCastEntry *entry = (mCastEntry *)sInfo.get_val();
    delete entry->red.storedCallback;
    ck::impl::XGroupSectionReducer *red =
      new ck::impl::XGroupSectionReducer(numSubSections, cb);
    // Configure the subsection callback to deposit with the final reducer
    sectionCB = new CkCallback(ck::impl::processSectionContribution, red);
  }
  // else, just direct the reduction to the actual client cb
  else
      sectionCB = cb;
  // Wire the sections together by storing the subsection cb in each sectionID
  for (int i=0; i<numSubSections; i++)
  {
      CkSectionInfo &sInfo = proxy.ckGetSectionID(i)._cookie;
      mCastEntry *entry = (mCastEntry *)sInfo.get_val();
      entry->red.storedCallback = sectionCB;
  }
}


//for group sections
void CkMulticastMgr::setReductionClient(CProxySection_Group &proxy, CkCallback *cb)
{
  CkCallback *sectionCB;
  int numSubSections = proxy.ckGetNumSections();
  DEBUGF(("[%d]setReductionClient for grpSec, numSubSections: %d \n", CkMyPe(), numSubSections));
  // If its a cross-array section,
  if (numSubSections > 1)
  {
    /** @warning: setReductionClient in cross section reduction should be phased out
     * Issue: https://github.com/charmplusplus/charm/issues/1249
     * Since finalCB is already set in initDelegateMgr it needs to be deleted here
     * There will still be a memory leak for the instantiation of XGroupSectionReducer object
     * Since this function will eventually be phased out, it is only a quick fix
     */
    // Configure the final cross-section reducer
    CkSectionInfo &sInfo = proxy.ckGetSectionID(0)._cookie;
    mCastEntry *entry = (mCastEntry *)sInfo.get_val();
    delete entry->red.storedCallback;
    ck::impl::XGroupSectionReducer *red =
      new ck::impl::XGroupSectionReducer(numSubSections, cb);
    // Configure the subsection callback to deposit with the final reducer
    sectionCB = new CkCallback(ck::impl::processSectionContribution, red);
  }
  // else, just direct the reduction to the actual client cb
  else
    sectionCB = cb;
  // Wire the sections together by storing the subsection cb in each sectionID
  for (int i=0; i<numSubSections; i++)
  {
    CkSectionInfo &sInfo = proxy.ckGetSectionID(i)._cookie;
    mCastEntry *entry = (mCastEntry *)sInfo.get_val();
    entry->red.storedCallback = sectionCB;
  }
}


void CkMulticastMgr::setReductionClient(CProxySection_ArrayElement &proxy, redClientFn fn,void *param)
{
  CkSectionInfo &id = proxy.ckGetSectionInfo();
  mCastEntry *entry = (mCastEntry *)id.get_val();
  entry->red.storedClient = fn;
  entry->red.storedClientParam = param;
}

inline CkReductionMsg *CkMulticastMgr::buildContributeMsg(int dataSize,void *data,CkReduction::reducerType type, CkSectionInfo &id, CkCallback &cb, int userFlag)
{
  CkReductionMsg *msg = CkReductionMsg::buildNew(dataSize, data);
  msg->reducer = type;
  msg->sid = id;
  msg->sourceFlag = -1;   // from array element
  msg->redNo = id.get_redNo();
  msg->gcount = 1;
  msg->rebuilt = (id.get_pe() == CkMyPe())?0:1;
  msg->callback = cb;
  msg->userFlag=userFlag;
  return msg;
}


void CkMulticastMgr::contribute(CkSectionInfo &id, int userFlag, int fragSize)
{
  CkCallback cb;
  contribute(0, NULL, CkReduction::nop, id, cb, userFlag, fragSize);
}

void CkMulticastMgr::contribute(CkSectionInfo &id, const CkCallback &cb, int userFlag, int fragSize)
{
  contribute(0, NULL, CkReduction::nop, id, cb, userFlag, fragSize);
}

void CkMulticastMgr::contribute(int dataSize,void *data,CkReduction::reducerType type, CkSectionInfo &id, int userFlag, int fragSize)
{
  CkCallback cb;
  contribute(dataSize, data, type, id, cb, userFlag, fragSize);
}


void CkMulticastMgr::contribute(int dataSize,void *data,CkReduction::reducerType type, CkSectionInfo &id, const CkCallback &cb, int userFlag, int fragSize)
{
  if (id.get_val() == NULL || id.get_redNo() == -1) 
    CmiAbort("contribute: SectionID is not initialized\n");

  int8_t nFrags;
  if (-1 == fragSize) {		// no frag
    nFrags = 1;
    fragSize = dataSize;
  }
  else {
    CmiAssert (dataSize >= fragSize);
    nFrags = dataSize/fragSize;
    if (dataSize%fragSize) nFrags++;
  }

  if (MAXFRAGS < nFrags) {
    CmiPrintf ("Recompile CkMulticast library for fragmenting msgs into more than %d fragments\n", MAXFRAGS);
    CmiAbort ("frag size too small\n");
  }

  int mpe = id.get_pe();
  CProxy_CkMulticastMgr  mCastGrp(thisgroup);

  // break the message into k-piece fragments
  int fSize = fragSize;
  for (int8_t i=0; i<nFrags; i++) {
    if ((0 != i) && ((nFrags-1) == i) && (0 != dataSize%fragSize)) {
      fSize = dataSize%fragSize;
    }

    CkReductionMsg *msg = CkReductionMsg::buildNew(fSize, data);

    // initialize the new msg
    msg->reducer            = type;
    msg->sid                = id;
    msg->nFrags             = nFrags;
    msg->fragNo             = i;
    msg->sourceFlag         = -1;
    msg->redNo              = id.get_redNo();
    msg->gcount             = 1;
    msg->rebuilt            = (mpe == CkMyPe())?0:1;
    msg->callback           = cb;
    msg->userFlag           = userFlag;


    mCastGrp[mpe].recvRedMsg(msg);

    data = (void*)(((char*)data) + fSize);
  }

  id.get_redNo()++;
  DEBUGF(("[%d] val: %d %p\n", CkMyPe(), id.get_pe(), id.get_val()));
}

CkReductionMsg* CkMulticastMgr::combineFrags (CkSectionInfo& id, 
                                              mCastEntry* entry,
                                              sectionRedInfo& redInfo) {
  int8_t i;
  int dataSize = 0;
  int8_t nFrags   = redInfo.msgs[0][0]->nFrags;

  // to avoid memcpy and allocation cost for non-pipelined reductions
  if (1 == nFrags) {
    CkReductionMsg* msg = redInfo.msgs[0][0];

    // free up the msg slot
    redInfo.msgs[0].clear();

    return msg;
  }

  for (i=0; i<nFrags; i++) {
    dataSize += redInfo.msgs[i][0]->dataSize;
  }

  CkReductionMsg *msg = CkReductionMsg::buildNew(dataSize, NULL);

  // initialize msg header
  msg->redNo      = redInfo.msgs[0][0]->redNo;
  msg->reducer    = redInfo.msgs[0][0]->reducer;
  msg->sid        = id;
  msg->nFrags     = nFrags;

  // I guess following fields need not be initialized
  msg->sourceFlag = 2;
  msg->rebuilt    = redInfo.msgs[0][0]->rebuilt;
  msg->callback   = redInfo.msgs[0][0]->callback;
  msg->userFlag   = redInfo.msgs[0][0]->userFlag;

  byte* data = (byte*)msg->getData ();
  for (i=0; i<nFrags; i++) {
    // copy data from fragments to msg
    memcpy(data, redInfo.msgs[i][0]->getData(), redInfo.msgs[i][0]->dataSize);
    data += redInfo.msgs[i][0]->dataSize;

    // free fragments
    delete redInfo.msgs[i][0];
    redInfo.msgs[i].clear();
  }

  return msg;
}



void CkMulticastMgr::reduceFragment (int index, CkSectionInfo& id,
                                     mCastEntry* entry, sectionRedInfo& redInfo,
                                     int currentTreeUp) {

    CProxy_CkMulticastMgr  mCastGrp(thisgroup);
    reductionMsgs& rmsgs = redInfo.msgs[index];
    int dataSize         = rmsgs[0]->dataSize;
    int i;
    int oldRedNo = redInfo.redNo;
    int nFrags   = rmsgs[0]->nFrags;
    int fragNo   = rmsgs[0]->fragNo;
    int userFlag = rmsgs[0]->userFlag;

    // Figure out (from one of the msg fragments) which reducer function to use
    CkReduction::reducerType reducer = rmsgs[0]->reducer;
    CkReduction::reducerFn f= CkReduction::reducerTable()[reducer].fn;
    CkAssert(NULL != f);

    // Check if migration occurred in any of the subtrees, and pick one valid callback
    CkCallback msg_cb;
    int8_t rebuilt = 0;
    for (i=0; i<rmsgs.size(); i++) {
        if (rmsgs[i]->rebuilt) rebuilt = 1;
        if (!rmsgs[i]->callback.isInvalid()) msg_cb = rmsgs[i]->callback;
    }

    // Perform the actual reduction
    CkReductionMsg *newmsg = (*f)(rmsgs.size(), rmsgs.data());
    newmsg->redNo  = redInfo.redNo;
    newmsg->nFrags = nFrags;
    newmsg->fragNo = fragNo;
    newmsg->userFlag = userFlag;
    newmsg->reducer = reducer;

    // Increment the number of fragments processed
    redInfo.npProcessed ++;

    // Delete all the fragments which are no longer needed
    for (i=0; i<rmsgs.size(); i++)
        if (rmsgs[i]!=newmsg) delete rmsgs[i];
    rmsgs.clear();

    // If I am not the tree root
    if (entry->hasParent()) {
        // send up to parent
        newmsg->sid        = entry->parentGrp;
        newmsg->sourceFlag = 2;
        newmsg->redNo      = oldRedNo; ///< @todo: redundant, duplicate assignment?
        newmsg->gcount     = redInfo.gcount [index];
        newmsg->rebuilt    = rebuilt;
        newmsg->callback   = msg_cb;
        DEBUGF(("[%d] ckmulticast: send %p to parent %d\n", CkMyPe(), entry->parentGrp.get_val(), entry->parentGrp.get_pe()));
        mCastGrp[entry->parentGrp.get_pe()].recvRedMsg(newmsg);
    } else {
        newmsg->sid = id;
        // Buffer the reduced fragment
        rmsgs.push_back (newmsg);
        // If all the fragments have been reduced
        if (redInfo.npProcessed == nFrags) {
            // Combine the fragments
            newmsg = combineFrags (id, entry, redInfo);
            // Set the reference number based on the user flag at the contribute call
            CkSetRefNum(newmsg, userFlag);
            // Trigger the appropriate reduction client
            if (redInfo.storedCallback != NULL)
                redInfo.storedCallback->send(newmsg);
            else if ( !msg_cb.isInvalid() )
                msg_cb.send(newmsg);
            else if (redInfo.storedClient != NULL) {
                redInfo.storedClient(id, redInfo.storedClientParam, dataSize, newmsg->data);
                delete newmsg;
            }
            else
                CmiAbort("Did you forget to register a reduction client?");

            DEBUGF(("ckmulticast: redn client called - currentTreeUp: %d entry:%p oldc: %p\n", currentTreeUp, entry, entry->oldc));
            //
            if (currentTreeUp) {
                if (entry->oldc) {
                    // free old tree on same processor;
                    mCastGrp[CkMyPe()].freeup(CkSectionInfo(id.get_pe(), entry->oldc, 0, entry->getAid()));
                    entry->oldc = NULL;
                }
                if (entry->hasOldtree()) {
                    // free old tree on old processor
                    int oldpe = entry->oldtree.pe;
                    mCastGrp[oldpe].freeup(CkSectionInfo(oldpe, entry->oldtree.entry, 0, entry->getAid()));
                    entry->oldtree.clear();
                }
            }
            // Indicate if a tree rebuild is required
            if (rebuilt && !entry->needRebuild) entry->needRebuild = 1;
        }
    }
}



/**
 * Called from:
 *   - contribute(): calls PE specified in the cookie
 *   - reduceFragment(): calls parent PE
 *   - recvRedMsg(): calls root PE (if tree is obsolete)
 *   - releaseFutureRedMsgs: calls this PE
 *   - releaseBufferedRedMsgs: calls root PE
 */
void CkMulticastMgr::recvRedMsg(CkReductionMsg *msg)
{
    int8_t i;
    /// Grab the section info embedded in the redn msg
    CkSectionInfo id = msg->sid;
    /// ... and get at the ptr which shows me which cookie to use
    mCastEntry *entry = (mCastEntry *)id.get_val();
    CmiAssert(entry!=NULL);

    CProxy_CkMulticastMgr  mCastGrp(thisgroup);

    //-------------------------------------------------------------------------
    /// If this cookie is obsolete
    if (entry->isObsolete()) {
        // Send up to root
        DEBUGF(("[%d] ckmulticast: section cookie obsolete. Will send to root %d\n", CkMyPe(), entry->rootSid.get_pe()));

        /// If I am the root, traverse the linked list of cookies to get the latest
        if (!entry->hasParent()) {
            mCastEntry *newentry = entry->newc;
            while (newentry && newentry->newc) newentry=newentry->newc;
            if (newentry) entry = newentry;
            CmiAssert(entry!=NULL);
        }

        ///
        if (!entry->hasParent() && !entry->isObsolete()) {
            /// Indicate it is not on old spanning tree
            msg->sourceFlag = 0;
        }
        /// If I am not the root or this latest cookie is also obsolete
        else {
            // Ensure that you're here with reason
            CmiAssert(entry->rootSid.get_pe() != CkMyPe() || entry->rootSid.get_val() != entry);
            // Edit the msg so that the recipient knows where to find its cookie
            msg->sid = entry->rootSid;
            msg->sourceFlag = 0;
            // Send the msg directly to the root of the redn tree
            mCastGrp[entry->rootSid.get_pe()].recvRedMsg(msg);
            return;
        }
    }

    /// Grab the locally stored redn info
    sectionRedInfo &redInfo = entry->red;


    DEBUGF(("[%d] RecvRedMsg, entry: %p, lcount: %d, cccount: %d, #localelems: %d, #children: %zu \n", CkMyPe(), (void *)entry, redInfo.lcount[msg->fragNo], redInfo.ccount[msg->fragNo], entry->getNumLocalElems(), entry->children.size()));

    //-------------------------------------------------------------------------
    /// If you've received a msg from a previous redn, something has gone horribly wrong somewhere!
    if (msg->redNo < redInfo.redNo) {
        CmiPrintf("[%d] msg redNo:%d, msg:%p, entry:%p redno:%d\n", CkMyPe(), msg->redNo, (void *)msg, (void *)entry, redInfo.redNo);
        CmiAbort("CkMulticast received a reduction msg with redNo less than the current redn number. Should never happen! \n");
    }

    //-------------------------------------------------------------------------
    /// If the current tree is not yet ready or if you've received a msg for a future redn, buffer the msg
    if (entry->notReady() || msg->redNo > redInfo.redNo) {
        DEBUGF(("[%d] Future redmsgs, buffered! msg:%p entry:%p ready:%d msg red:%d sys redno:%d\n", CkMyPe(), (void *)msg, (void *)entry, entry->notReady(), msg->redNo, redInfo.redNo));
        redInfo.futureMsgs.push_back(msg);
        return;
    }

    //-------------------------------------------------------------------------
    const int index = msg->fragNo;
    // New contribution from an ArrayElement
    if (msg->isFromUser()) {
        redInfo.lcount [index] ++;
    }
    // Redn from a child
    if (msg->sourceFlag == 2) {
        redInfo.ccount [index] ++;
    }
    // Total elems that have contributed the indexth fragment
    redInfo.gcount [index] += msg->gcount;

    //-------------------------------------------------------------------------
    // Buffer the msg
    redInfo.msgs [index].push_back(msg);

    //-------------------------------------------------------------------------
    /// Flag if this fragment can be reduced (if all local elements and children have contributed this fragment)
    int currentTreeUp = 0;
    if (redInfo.lcount [index] == entry->getNumLocalElems() && redInfo.ccount [index] == entry->children.size())
        currentTreeUp = 1;

    /// Flag (only at the redn root) if all array elements contributed all their fragments
    int mixTreeUp = 0;
    if (!entry->hasParent()) {
        mixTreeUp = 1;
        for (int8_t i=0; i<msg->nFrags; i++)
            if (entry->getNumAllElems() != redInfo.gcount [i])
                mixTreeUp = 0;
    }

    // If reduceFragment is not being called now, check if partialReduction is possible (streamable)
    if (!currentTreeUp && !mixTreeUp && redInfo.msgs[index].size() > 1 && CkReduction::reducerTable()[msg->reducer].streamable) {
      reductionMsgs& rmsgs = redInfo.msgs[index];
      CkReduction::reducerType reducer = rmsgs[0]->reducer;
      CkReduction::reducerFn f= CkReduction::reducerTable()[msg->reducer].fn;
      CkAssert(f != NULL);

      int oldRedNo = redInfo.redNo;
      int nFrags   = rmsgs[0]->nFrags;
      int fragNo   = rmsgs[0]->fragNo;
      int userFlag = rmsgs[0]->userFlag;
      CkSectionInfo oldId = rmsgs[0]->sid;
      CkCallback msg_cb;
      int8_t rebuilt = 0;
      if (msg->rebuilt) rebuilt = 1;
      if (!msg->callback.isInvalid()) msg_cb = msg->callback;
      // Perform the actual reduction (streaming)
      CkReductionMsg *newmsg = (*f)(rmsgs.size(), rmsgs.data());
      newmsg->redNo  = oldRedNo;
      newmsg->nFrags = nFrags;
      newmsg->fragNo = fragNo;
      newmsg->userFlag = userFlag;
      newmsg->reducer = reducer;
      if (rebuilt) newmsg->rebuilt = 1;
      if (!msg_cb.isInvalid()) newmsg->callback = msg_cb;
      newmsg->gcount = redInfo.gcount[index];
      newmsg->sid = oldId;
      // Remove the current message that was pushed
      rmsgs.pop_back();
      delete msg;
      // Only the partially reduced message should be remaining in the msgs vector after partialReduction
      CkAssert(rmsgs.size() == 1);
    }

    //-------------------------------------------------------------------------
    /// If this fragment can be reduced, or if I am the root and have received all fragments from all elements
    if (currentTreeUp || mixTreeUp)
    {
        const int8_t nFrags = msg->nFrags;
        /// Reduce this fragment
        reduceFragment (index, id, entry, redInfo, currentTreeUp);
        if (!currentTreeUp) redInfo.mixed = 1;

        /// If all the fragments for the current reduction have been processed
        if (redInfo.npProcessed == nFrags) {

            /// Increment the reduction number in all of this section's cookies
            entry->incReduceNo();

            /// Reset bookkeeping counters
            for (i=0; i<nFrags; i++) {
                redInfo.lcount [i] = 0;
                redInfo.ccount [i] = 0;
                redInfo.gcount [i] = 0;
            }
            redInfo.npProcessed = 0;
            /// A child that did not carry every fragment still has the number
            /// this tree was built with (contributions were in flight in the
            /// old tree) and would hold the next reduction as a future one.
            if (redInfo.mixed) {
                redInfo.mixed = 0;
                for (size_t c=0; c<entry->children.size(); c++)
                    mCastGrp[entry->children[c].get_pe()].updateRedNo((mCastEntry *)entry->children[c].get_val(), redInfo.redNo);
            }
            /// Now that, the current redn is done, release any pending msgs from future redns
            releaseFutureReduceMsgs(entry);
        }
    }
}



void CkMulticastMgr::releaseFutureReduceMsgs(mCastEntryPtr entry)
{
  CProxy_CkMulticastMgr  mCastGrp(thisgroup);

  for (int i=0; i<entry->red.futureMsgs.size(); i++) {
    DEBUGF(("releaseFutureReduceMsgs: %p\n", entry->red.futureMsgs[i]));
    mCastGrp[CkMyPe()].recvRedMsg(entry->red.futureMsgs[i]);
  }
  entry->red.futureMsgs.clear();
}



// these messages have to be sent to root
void CkMulticastMgr::releaseBufferedReduceMsgs(mCastEntryPtr entry)
{
  int i;
  CProxy_CkMulticastMgr  mCastGrp(thisgroup);

  for (int j=0; j<MAXFRAGS; j++) {
    for (i=0; i<entry->red.msgs[j].size(); i++) {
      CkReductionMsg *msg = entry->red.msgs[j][i];
      DEBUGF(("releaseBufferedReduceMsgs:%p red:%d in entry:%p\n", msg, msg->redNo, entry));
      msg->sid = entry->rootSid;
      msg->sourceFlag = 0;
      mCastGrp[entry->rootSid.get_pe()].recvRedMsg(msg);
    }
    entry->red.msgs[j].clear();
  }


  for (i=0; i<entry->red.futureMsgs.size(); i++) {
    CkReductionMsg *msg = entry->red.futureMsgs[i];
    DEBUGF(("releaseBufferedFutureReduceMsgs: %p red:%d in entry: %p\n", msg,msg->redNo, entry));
    msg->sid = entry->rootSid;
    msg->sourceFlag = 0;
    mCastGrp[entry->rootSid.get_pe()].recvRedMsg(msg);
  }
  entry->red.futureMsgs.clear();
}



void CkMulticastMgr::updateRedNo(mCastEntryPtr entry, int red)
{
  DEBUGF(("[%d] updateRedNo entry:%p to %d\n", CkMyPe(), entry, red));
  if (entry->red.redNo < red)
    entry->red.redNo = red;

  CProxy_CkMulticastMgr mp(thisgroup);
  for (int i=0; i<entry->children.size(); i++) {
    mp[entry->children[i].get_pe()].updateRedNo((mCastEntry *)entry->children[i].get_val(), red);
  }

  releaseFutureReduceMsgs(entry);
}

#include "CkMulticast.def.h"

