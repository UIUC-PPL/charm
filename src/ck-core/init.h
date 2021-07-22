#ifndef _INIT_H
#define _INIT_H

#include "charm.h" // For CkNumPes
#include "charm-api.h"
#include <new>   // for in-place new operator
#include "ckhashtable.h"
#include <vector>
#include "ckrdma.h"

typedef CkQ<void *> PtrQ;
class envelope;
typedef std::vector<CkZeroPtr<envelope> > PtrVec;

// Map to store object index and number of pending rdma ops
typedef std::unordered_map<CmiUInt8, CmiUInt1> ObjNumRdmaOpsMap;
typedef std::unordered_map<CmiUInt4, CkNcpyBufferPost> ReqTagPostMap;
typedef std::unordered_map<CmiUInt4, CkPostedBuffer> ReqTagBufferMap;

class IrrGroup;
class TableEntry {
    IrrGroup *obj;
    PtrQ *pending; //Buffers msgs recv'd before group is created
    int cIdx;

  public:
    TableEntry(int ignored=0) { (void)ignored; obj=0; pending=0; cIdx=-1; }
    inline IrrGroup* getObj(void) { return obj; }
    inline void setObj(void *_obj) { obj=(IrrGroup *)_obj; }
    PtrQ* getPending(void) { return pending; }
    inline void clearPending(void) { delete pending; pending = NULL; }
    void enqMsg(void *msg) {
      if (pending==0)
        pending=new PtrQ();
      pending->enq(msg);
    }
    void setcIdx(int cIdx_){
      cIdx = cIdx_;
    }
    inline int getcIdx(void) const { return cIdx; }
};

template <class dtype>
class GroupIdxArray {
  // The initial size of the table for groups created on PE 0:
  enum {INIT_BINS_PE0=32};
  
  dtype *tab;         // direct table for groups created on processor 0
  CkHashtable_c hashTab; // hashtable for groups created on processors >0
  int max;   // Size of "tab"
   
  //This non-inline version of "find", below, allows the (much simpler)
  // common case to be inlined.
  dtype& nonInlineFind(CkGroupID n) {
#if CMK_ERROR_CHECKING
      if (n.idx==0) {
        CkAbort("Group ID is zero-- invalid!\n");
        return *(new dtype);
      } else
#endif
      if (n.idx>=max) { /* Extend processor 0's group table */
        dtype *oldtab=tab;
	int i, oldmax=max;
	max=2*n.idx+1;
	tab=new dtype[max];
	for (i=0;i<oldmax;i++) tab[i]=oldtab[i];
	for (i=oldmax;i<max;i++) tab[i]=dtype(0);
	delete [] oldtab;
	return tab[n.idx];
      }
      else /*n.idx < 0*/
      { /*Groups created on processors >0 go into a hashtable:*/
        if(hashTab == NULL)
          hashTab = CkCreateHashtable_int(sizeof(dtype),17);

        dtype *ret = (dtype *)CkHashtableGet(hashTab,&(n.idx));

        if(ret == NULL)  // insert new entry into the table
        {
          ret = (dtype *)CkHashtablePut(hashTab,&(n.idx));
          new (ret) dtype(0); //Call dtype's constructor (ICK!)
        }
	return *ret;
      }
   }

  public:
    GroupIdxArray() {tab=NULL;max=0;hashTab=NULL;}
    ~GroupIdxArray() {delete[] tab; if (hashTab!=NULL) CkDeleteHashtable(hashTab);}
    void init(void) {
      max = INIT_BINS_PE0;
      tab = new dtype[max];
      for(int i=0;i<max;i++)
       tab[i]=dtype(0);
      hashTab=NULL;
    }

    inline dtype& find(CkGroupID n) {
      if(n.idx>0 && n.idx<max)
        return tab[n.idx];
      else
        return nonInlineFind(n);
    }
};

typedef GroupIdxArray<TableEntry> GroupTable;
typedef std::vector<CkGroupID> GroupIDTable;

typedef void (*CkInitCallFn)(void);
class InitCallTable 
{
public:
  CkQ<CkInitCallFn>  initNodeCalls;
  CkQ<CkInitCallFn>  initProcCalls;
public:
  void enumerateInitCalls();
};
void _registerInitCall(CkInitCallFn fn, int isNodeCall);

/*********************************************************/
/**
\addtogroup CkInit
These are implemented in init.C.
*/
/*@{*/
extern unsigned int    _printCS;
extern unsigned int    _printSS;

extern int     _infoIdx;
extern int     _charmHandlerIdx;
extern int     _roRestartHandlerIdx;     /* for checkpoint/restart */
#if CMK_SHRINK_EXPAND
extern int     _ROGroupRestartHandlerIdx;     /* for checkpoint/restart */
#endif
extern int     _bocHandlerIdx;
extern int     _qdHandlerIdx;
extern unsigned int   _numInitMsgs;

CksvExtern(unsigned int,  _numInitNodeMsgs);
CksvExtern(CmiNodeLock, _nodeLock);
CksvExtern(GroupTable*,  _nodeGroupTable);
CksvExtern(GroupIDTable, _nodeGroupIDTable);
CksvExtern(CmiImmediateLockType, _nodeGroupTableImmLock);
CksvExtern(unsigned int, _numNodeGroups);

CkpvExtern(int, _charmEpoch);

CkpvExtern(CkGroupID,_currentGroup);
CkpvExtern(void*,  _currentNodeGroupObj);
CkpvExtern(CkGroupID, _currentGroupRednMgr);

CkpvExtern(GroupTable*, _groupTable);
CkpvExtern(GroupIDTable*, _groupIDTable);
CkpvExtern(CmiImmediateLockType, _groupTableImmLock);
CkpvExtern(unsigned int, _numGroups);

CkpvExtern(bool, _destroyingNodeGroup);

CkpvExtern(char **,Ck_argv);

static inline IrrGroup *_localBranch(CkGroupID gID)
{
  return CkpvAccess(_groupTable)->find(gID).getObj();
}

// Similar to _localBranch, but should be used from non-PE-local, but node-local PE
// Ensure thread safety while using this function as it is accessing a non-PE-local group
static inline IrrGroup *_localBranchOther(CkGroupID gID, int rank)
{
  return CkpvAccessOther(_groupTable, rank)->find(gID).getObj();
}

extern void _registerCommandLineOpt(const char* opt);
extern void _initCharm(int argc, char **argv);
extern void _sendReadonlies();

CLINKAGE int charm_main(int argc, char **argv);
FLINKAGE void FTN_NAME(CHARM_MAIN_FORTRAN_WRAPPER, charm_main_fortran_wrapper)(int *argc, char **argv);

/** This routine registers the user's main module.  It is normally
    generated by the translator, but for FEM and AMPI may actually be 
    the "fallback" version in compat_regmm.c. */
CLINKAGE void CkRegisterMainModule(void);

typedef  void  (*CkExitFn) (void);

extern CkQ<CkExitFn> _CkExitFnVec;
extern void registerExitFn(CkExitFn);
// Each registered exit function must eventually lead to a single call
// being made to CkContinueExit()
extern void CkContinueExit();

void EmergencyExit(void);

/*@}*/

#endif


