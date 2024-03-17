/** \file cklocation.C
 *  \addtogroup CkArrayImpl
 *
 *  The location manager keeps track of an indexed set of migratable objects.
 *  It is used by the array manager to locate array elements, interact with the
 *  load balancer, and perform migrations.
 *
 *  Orion Sky Lawlor, olawlor@acm.org 9/29/2001
 */

#include "TopoManager.h"
#include "charm++.h"
#include "ck.h"
#include "cksyncbarrier.h"
#include "hilbert.h"
#include "partitioning_strategies.h"
#include "pup_stl.h"
#include "register.h"
#include "trace.h"
#include <algorithm>
#include <limits>
#include <sstream>
#include <stdarg.h>
#include <vector>

#if CMK_LBDB_ON
#  include "LBManager.h"
#  include "MetaBalancer.h"
#  if CMK_GLOBAL_LOCATION_UPDATE
#    include "BaseLB.h"
#    include "init.h"
#  endif
CkpvExtern(int, _lb_obj_index);  // for lbdb user data for obj index
#endif                           // CMK_LBDB_ON

CpvExtern(std::vector<NcpyOperationInfo*>, newZCPupGets);  // used for ZC Pup
#ifndef CMK_CHARE_USE_PTR
CkpvExtern(int, currentChareIdx);
#endif

#if CMK_GRID_QUEUE_AVAILABLE
CpvExtern(void*, CkGridObject);
#endif

#define ARRAY_DEBUG_OUTPUT 0

#if ARRAY_DEBUG_OUTPUT
#  define DEB(x) CkPrintf x   // General debug messages
#  define DEBI(x) CkPrintf x  // Index debug messages
#  define DEBC(x) CkPrintf x  // Construction debug messages
#  define DEBS(x) CkPrintf x  // Send/recv/broadcast debug messages
#  define DEBM(x) CkPrintf x  // Migration debug messages
#  define DEBL(x) CkPrintf x  // Load balancing debug messages
#  define DEBN(x) CkPrintf x  // Location debug messages
#  define DEBB(x) CkPrintf x  // Broadcast debug messages
#  define AA "LocMgr on %d: "
#  define AB , CkMyPe()
#  define DEBUG(x) CkPrintf x
#  define DEBAD(x) CkPrintf x
#else
#  define DEB(X)   /*CkPrintf x*/
#  define DEBI(X)  /*CkPrintf x*/
#  define DEBC(X)  /*CkPrintf x*/
#  define DEBS(x)  /*CkPrintf x*/
#  define DEBM(X)  /*CkPrintf x*/
#  define DEBL(X)  /*CkPrintf x*/
#  define DEBN(x)  /*CkPrintf x*/
#  define DEBB(x)  /*CkPrintf x*/
#  define str(x)   /**/
#  define DEBUG(x) /**/
#  define DEBAD(x) /*CkPrintf x*/
#endif

// whether to use block mapping in the SMP node level
bool useNodeBlkMapping;

/// Message size above which the runtime will buffer messages directed at
/// unlocated array elements
int _messageBufferingThreshold;

#if CMK_LBDB_ON

#  if CMK_GLOBAL_LOCATION_UPDATE
void UpdateLocation(MigrateInfo& migData)
{
  CkGroupID locMgrGid = ck::ObjID(migData.obj.id).getCollectionID();
  if (locMgrGid.idx == 0)
  {
    return;
  }

  CkLocMgr* localLocMgr = (CkLocMgr*)CkLocalBranch(locMgrGid);
  // CkLocMgr only uses element IDs, so extract just that part from the ObjID
  localLocMgr->updateLocation(ck::ObjID(migData.obj.id).getElementID(), migData.to_pe);
}
#  endif

#endif

/*********************** Array Messages ************************/
CmiUInt8 CkArrayMessage::array_element_id(void)
{
  return ck::ObjID(UsrToEnv((void*)this)->getRecipientID()).getElementID();
}
unsigned short& CkArrayMessage::array_ep(void)
{
  return UsrToEnv((void*)this)->getsetArrayEp();
}
unsigned short& CkArrayMessage::array_ep_bcast(void)
{
  return UsrToEnv((void*)this)->getsetArrayBcastEp();
}
unsigned char& CkArrayMessage::array_hops(void)
{
  return UsrToEnv((void*)this)->getsetArrayHops();
}
unsigned int CkArrayMessage::array_getSrcPe(void)
{
  return UsrToEnv((void*)this)->getsetArraySrcPe();
}
unsigned int CkArrayMessage::array_ifNotThere(void)
{
  return UsrToEnv((void*)this)->getArrayIfNotThere();
}
void CkArrayMessage::array_setIfNotThere(unsigned int i)
{
  UsrToEnv((void*)this)->setArrayIfNotThere(i);
}

/*********************** Array Map ******************
Given an array element index, an array map tells us
the index's "home" Pe.  This is the Pe the element will
be created on, and also where messages to this element will
be forwarded by default.
*/

CkArrayMap::CkArrayMap(void) {}
CkArrayMap::~CkArrayMap() {}
int CkArrayMap::registerArray(const CkArrayIndex& numElements, CkArrayID aid)
{
  return 0;
}
void CkArrayMap::unregisterArray(int idx) {}

#define CKARRAYMAP_POPULATE_INITIAL(POPULATE_CONDITION)                                  \
  int i;                                                                                 \
  int index[6];                                                                          \
  int start_data[6], end_data[6], step_data[6];                                          \
  for (int d = 0; d < 6; d++)                                                            \
  {                                                                                      \
    start_data[d] = 0;                                                                   \
    end_data[d] = step_data[d] = 1;                                                      \
    if (end.dimension >= 4 && d < end.dimension)                                         \
    {                                                                                    \
      start_data[d] = ((short int*)start.data())[d];                                     \
      end_data[d] = ((short int*)end.data())[d];                                         \
      step_data[d] = ((short int*)step.data())[d];                                       \
    }                                                                                    \
    else if (d < end.dimension)                                                          \
    {                                                                                    \
      start_data[d] = start.data()[d];                                                   \
      end_data[d] = end.data()[d];                                                       \
      step_data[d] = step.data()[d];                                                     \
    }                                                                                    \
  }                                                                                      \
                                                                                         \
  for (index[0] = start_data[0]; index[0] < end_data[0]; index[0] += step_data[0])       \
  {                                                                                      \
    for (index[1] = start_data[1]; index[1] < end_data[1]; index[1] += step_data[1])     \
    {                                                                                    \
      for (index[2] = start_data[2]; index[2] < end_data[2]; index[2] += step_data[2])   \
      {                                                                                  \
        for (index[3] = start_data[3]; index[3] < end_data[3]; index[3] += step_data[3]) \
        {                                                                                \
          for (index[4] = start_data[4]; index[4] < end_data[4];                         \
               index[4] += step_data[4])                                                 \
          {                                                                              \
            for (index[5] = start_data[5]; index[5] < end_data[5];                       \
                 index[5] += step_data[5])                                               \
            {                                                                            \
              if (end.dimension == 1)                                                    \
              {                                                                          \
                i = index[0];                                                            \
                CkArrayIndex1D idx(index[0]);                                            \
                if (POPULATE_CONDITION)                                                  \
                {                                                                        \
                  mgr->insertInitial(idx, CkCopyMsg(&ctorMsg));                          \
                }                                                                        \
              }                                                                          \
              else if (end.dimension == 2)                                               \
              {                                                                          \
                i = index[0] * end_data[1] + index[1];                                   \
                CkArrayIndex2D idx(index[0], index[1]);                                  \
                if (POPULATE_CONDITION)                                                  \
                {                                                                        \
                  mgr->insertInitial(idx, CkCopyMsg(&ctorMsg));                          \
                }                                                                        \
              }                                                                          \
              else if (end.dimension == 3)                                               \
              {                                                                          \
                i = (index[0] * end_data[1] + index[1]) * end_data[2] + index[2];        \
                CkArrayIndex3D idx(index[0], index[1], index[2]);                        \
                if (POPULATE_CONDITION)                                                  \
                {                                                                        \
                  mgr->insertInitial(idx, CkCopyMsg(&ctorMsg));                          \
                }                                                                        \
              }                                                                          \
              else if (end.dimension == 4)                                               \
              {                                                                          \
                i = ((index[0] * end_data[1] + index[1]) * end_data[2] + index[2]) *     \
                        end_data[3] +                                                    \
                    index[3];                                                            \
                CkArrayIndex4D idx(index[0], index[1], index[2], index[3]);              \
                if (POPULATE_CONDITION)                                                  \
                {                                                                        \
                  mgr->insertInitial(idx, CkCopyMsg(&ctorMsg));                          \
                }                                                                        \
              }                                                                          \
              else if (end.dimension == 5)                                               \
              {                                                                          \
                i = (((index[0] * end_data[1] + index[1]) * end_data[2] + index[2]) *    \
                         end_data[3] +                                                   \
                     index[3]) *                                                         \
                        end_data[4] +                                                    \
                    index[4];                                                            \
                CkArrayIndex5D idx(index[0], index[1], index[2], index[3], index[4]);    \
                if (POPULATE_CONDITION)                                                  \
                {                                                                        \
                  mgr->insertInitial(idx, CkCopyMsg(&ctorMsg));                          \
                }                                                                        \
              }                                                                          \
              else if (end.dimension == 6)                                               \
              {                                                                          \
                i = ((((index[0] * end_data[1] + index[1]) * end_data[2] + index[2]) *   \
                          end_data[3] +                                                  \
                      index[3]) *                                                        \
                         end_data[4] +                                                   \
                     index[4]) *                                                         \
                        end_data[5] +                                                    \
                    index[5];                                                            \
                CkArrayIndex6D idx(index[0], index[1], index[2], index[3], index[4],     \
                                   index[5]);                                            \
                if (POPULATE_CONDITION)                                                  \
                {                                                                        \
                  mgr->insertInitial(idx, CkCopyMsg(&ctorMsg));                          \
                }                                                                        \
              }                                                                          \
            }                                                                            \
          }                                                                              \
        }                                                                                \
      }                                                                                  \
    }                                                                                    \
  }

void CkArrayMap::populateInitial(int arrayHdl, CkArrayOptions& options, void* ctorMsg,
                                 CkArray* mgr)
{
  CkArrayIndex start = options.getStart();
  CkArrayIndex end = options.getEnd();
  CkArrayIndex step = options.getStep();
  if (end.nInts == 0)
  {
    CkFreeMsg(ctorMsg);
    return;
  }
  int thisPe = CkMyPe();
  /* The CkArrayIndex is supposed to have at most 3 dimensions, which
     means that all the fields are ints, and numElements.nInts represents
     how many of them are used */
  CKARRAYMAP_POPULATE_INITIAL(CMK_RANK_0(procNum(arrayHdl, idx)) == thisPe);

  CkFreeMsg(ctorMsg);
}

void CkArrayMap::storeCkArrayOpts(CkArrayOptions options)
{
  // options will not be used on demand_creation arrays
  storeOpts = options;
}

void CkArrayMap::pup(PUP::er& p)
{
  p | storeOpts;
  p | dynamicIns;
}

CkGroupID _defaultArrayMapID;
CkGroupID _fastArrayMapID;

class RRMap : public CkArrayMap
{
private:
  CkArrayIndex maxIndex;
  uint64_t products[2 * CK_ARRAYINDEX_MAXLEN];
  bool productsInit;

public:
  RRMap(void)
  {
    DEBC((AA "Creating RRMap\n" AB));
    productsInit = false;
  }
  RRMap(CkMigrateMessage* m) : CkArrayMap(m) {}

  void indexInit()
  {
    productsInit = true;
    maxIndex = storeOpts.getEnd();
    products[maxIndex.dimension - 1] = 1;
    if (maxIndex.dimension <= CK_ARRAYINDEX_MAXLEN)
    {
      for (int dim = maxIndex.dimension - 2; dim >= 0; dim--)
      {
        products[dim] = products[dim + 1] * maxIndex.index[dim + 1];
      }
    }
    else
    {
      for (int dim = maxIndex.dimension - 2; dim >= 0; dim--)
      {
        products[dim] = products[dim + 1] * maxIndex.indexShorts[dim + 1];
      }
    }
  }  // End of indexInit

  int procNum(int arrayHdl, const CkArrayIndex& i)
  {
    if (i.dimension == 1)
    {
      // Map 1D integer indices in simple round-robin fashion
      int ans = (i.data()[0]) % CkNumPes();
      return ans;
    }
    else
    {
      if (dynamicIns.find(arrayHdl) != dynamicIns.end())
      {
        // Finding indicates that current array uses dynamic insertion
        // Map other indices based on their hash code, mod a big prime.
        unsigned int hash = (i.hash() + 739) % 1280107;
        int ans = (hash % CkNumPes());
        return ans;
      }
      else
      {
        if (!productsInit)
        {
          indexInit();
        }

        int indexOffset = 0;
        if (i.dimension <= CK_ARRAYINDEX_MAXLEN)
        {
          for (int dim = i.dimension - 1; dim >= 0; dim--)
          {
            indexOffset += (i.index[dim] * products[dim]);
          }
        }
        else
        {
          for (int dim = maxIndex.dimension - 1; dim >= 0; dim--)
          {
            indexOffset += (i.indexShorts[dim] * products[dim]);
          }
        }
        return indexOffset % CkNumPes();
      }
    }
  }

  void pup(PUP::er& p)
  {
    CkArrayMap::pup(p);
    p | maxIndex;
    p | productsInit;
    PUParray(p, products, 2 * CK_ARRAYINDEX_MAXLEN);
  }
};

/**
 * Class used to store the dimensions of the array and precalculate numChares,
 * binSize and other values for the DefaultArrayMap -- ASB
 */
class arrayMapInfo
{
public:
  CkArrayIndex _nelems;
  int _binSizeFloor; /* floor of numChares/numPes */
  int _binSizeCeil;  /* ceiling of numChares/numPes */
  int _numChares;    /* initial total number of chares */
  int _remChares;    /* numChares % numPes -- equals the number of
                        processors in the first set */
  int _numFirstSet;  /* _remChares X (_binSize + 1) -- number of
                        chares in the first set */

  int _nBinSizeFloor; /* floor of numChares/numNodes */
  int _nRemChares;    /* numChares % numNodes -- equals the number of
                         nodes in the first set */
  int _nNumFirstSet;  /* _remChares X (_binSize + 1) -- number of
                         chares in the first set of nodes */

  /** All processors are divided into two sets. Processors in the first set
   *  have one chare more than the processors in the second set. */

  arrayMapInfo(void) {}

  arrayMapInfo(const CkArrayIndex& n) : _nelems(n), _numChares(0) { compute_binsize(); }

  ~arrayMapInfo() {}

  void compute_binsize()
  {
    int numPes = CkNumPes();
    // Now assuming homogenous nodes where each node has the same number of PEs
    int numNodes = CkNumNodes();

    if (_nelems.dimension == 1)
    {
      _numChares = _nelems.data()[0];
    }
    else if (_nelems.dimension == 2)
    {
      _numChares = _nelems.data()[0] * _nelems.data()[1];
    }
    else if (_nelems.dimension == 3)
    {
      _numChares = _nelems.data()[0] * _nelems.data()[1] * _nelems.data()[2];
    }
    else if (_nelems.dimension == 4)
    {
      _numChares =
          (int)(((short int*)_nelems.data())[0] * ((short int*)_nelems.data())[1] *
                ((short int*)_nelems.data())[2] * ((short int*)_nelems.data())[3]);
    }
    else if (_nelems.dimension == 5)
    {
      _numChares =
          (int)(((short int*)_nelems.data())[0] * ((short int*)_nelems.data())[1] *
                ((short int*)_nelems.data())[2] * ((short int*)_nelems.data())[3] *
                ((short int*)_nelems.data())[4]);
    }
    else if (_nelems.dimension == 6)
    {
      _numChares =
          (int)(((short int*)_nelems.data())[0] * ((short int*)_nelems.data())[1] *
                ((short int*)_nelems.data())[2] * ((short int*)_nelems.data())[3] *
                ((short int*)_nelems.data())[4] * ((short int*)_nelems.data())[5]);
    }

    _remChares = _numChares % numPes;
    _binSizeFloor = (int)floor((double)_numChares / (double)numPes);
    _binSizeCeil = (int)ceil((double)_numChares / (double)numPes);
    _numFirstSet = _remChares * (_binSizeFloor + 1);

    _nRemChares = _numChares % numNodes;
    _nBinSizeFloor = _numChares / numNodes;
    _nNumFirstSet = _nRemChares * (_nBinSizeFloor + 1);
  }

  void pup(PUP::er& p)
  {
    p | _nelems;
    p | _binSizeFloor;
    p | _binSizeCeil;
    p | _numChares;
    p | _remChares;
    p | _numFirstSet;
    p | _nBinSizeFloor;
    p | _nRemChares;
    p | _nNumFirstSet;
  }
};

/**
 * The default map object -- This does blocked mapping in the general case and
 * calls the round-robin procNum for the dynamic insertion case -- ASB
 */
class DefaultArrayMap : public RRMap
{
public:
  /** This array stores information about different chare arrays in a Charm
   *  program (dimensions, binsize, numChares etc ... ) */
  CkPupPtrVec<arrayMapInfo> amaps;

public:
  DefaultArrayMap(void) { DEBC((AA "Creating DefaultArrayMap\n" AB)); }

  DefaultArrayMap(CkMigrateMessage* m) : RRMap(m) {}

  int registerArray(const CkArrayIndex& numElements, CkArrayID aid)
  {
    int idx = amaps.size();
    amaps.resize(idx + 1);
    amaps[idx] = new arrayMapInfo(numElements);
    return idx;
  }

  void unregisterArray(int idx)
  {
    delete amaps[idx];
    amaps[idx] = NULL;
  }

  int procNum(int arrayHdl, const CkArrayIndex& i)
  {
    int flati;
    auto& info = amaps[arrayHdl];
    if (info->_nelems.dimension == 0)
    {
      dynamicIns[arrayHdl] = true;
      return RRMap::procNum(arrayHdl, i);
    }

    if (i.dimension == 1)
    {
      flati = i.data()[0];
    }
    else if (i.dimension == 2)
    {
      flati = i.data()[0] * info->_nelems.data()[1] + i.data()[1];
    }
    else if (i.dimension == 3)
    {
      flati = (i.data()[0] * info->_nelems.data()[1] + i.data()[1]) *
                  info->_nelems.data()[2] +
              i.data()[2];
    }
    else if (i.dimension == 4)
    {
      flati = (int)(((((short int*)i.data())[0] *
                          ((short int*)info->_nelems.data())[1] +
                      ((short int*)i.data())[1]) *
                         ((short int*)info->_nelems.data())[2] +
                     ((short int*)i.data())[2]) *
                        ((short int*)info->_nelems.data())[3] +
                    ((short int*)i.data())[3]);
    }
    else if (i.dimension == 5)
    {
      flati = (int)((((((short int*)i.data())[0] *
                           ((short int*)info->_nelems.data())[1] +
                       ((short int*)i.data())[1]) *
                          ((short int*)info->_nelems.data())[2] +
                      ((short int*)i.data())[2]) *
                         ((short int*)info->_nelems.data())[3] +
                     ((short int*)i.data())[3]) *
                        ((short int*)info->_nelems.data())[4] +
                    ((short int*)i.data())[4]);
    }
    else if (i.dimension == 6)
    {
      flati = (int)(((((((short int*)i.data())[0] *
                            ((short int*)info->_nelems.data())[1] +
                        ((short int*)i.data())[1]) *
                           ((short int*)info->_nelems.data())[2] +
                       ((short int*)i.data())[2]) *
                          ((short int*)info->_nelems.data())[3] +
                      ((short int*)i.data())[3]) *
                         ((short int*)info->_nelems.data())[4] +
                     ((short int*)i.data())[4]) *
                        ((short int*)info->_nelems.data())[5] +
                    ((short int*)i.data())[5]);
    }
#if CMK_ERROR_CHECKING
    else
    {
      CkAbort("CkArrayIndex has more than 6 dimensions!");
    }
#endif

    if (useNodeBlkMapping)
    {
      if (flati < info->_numChares)
      {
        int numCharesOnNode = info->_nBinSizeFloor;
        int startNodeID, offsetInNode;
        if (flati < info->_nNumFirstSet)
        {
          numCharesOnNode++;
          startNodeID = flati / numCharesOnNode;
          offsetInNode = flati % numCharesOnNode;
        }
        else
        {
          startNodeID = info->_nRemChares +
                        (flati - info->_nNumFirstSet) / numCharesOnNode;
          offsetInNode = (flati - info->_nNumFirstSet) % numCharesOnNode;
        }
        int nodeSize = CkMyNodeSize();  // assuming every node has same number of PEs
        int elemsPerPE = numCharesOnNode / nodeSize;
        int remElems = numCharesOnNode % nodeSize;
        int firstSetPEs = remElems * (elemsPerPE + 1);
        if (offsetInNode < firstSetPEs)
        {
          return CkNodeFirst(startNodeID) + offsetInNode / (elemsPerPE + 1);
        }
        else
        {
          return CkNodeFirst(startNodeID) + remElems +
                 (offsetInNode - firstSetPEs) / elemsPerPE;
        }
      }
      else
        return (flati % CkNumPes());
    }
    // regular PE-based block mapping
    if (flati < info->_numFirstSet)
      return (flati / (info->_binSizeFloor + 1));
    else if (flati < info->_numChares)
      return (info->_remChares +
              (flati - info->_numFirstSet) / (info->_binSizeFloor));
    else
      return (flati % CkNumPes());
  }

  void pup(PUP::er& p)
  {
    RRMap::pup(p);
    int npes = CkNumPes();
    p | npes;
    p | amaps;
    if (p.isUnpacking() && npes != CkNumPes())
    {  // binSize needs update
      for (int i = 0; i < amaps.size(); i++)
        if (amaps[i])
          amaps[i]->compute_binsize();
    }
  }
};

/**
 *  A fast map for chare arrays which do static insertions and promise NOT
 *  to do late insertions -- ASB
 */
class FastArrayMap : public DefaultArrayMap
{
public:
  FastArrayMap(void) { DEBC((AA "Creating FastArrayMap\n" AB)); }

  FastArrayMap(CkMigrateMessage* m) : DefaultArrayMap(m) {}

  int registerArray(const CkArrayIndex& numElements, CkArrayID aid)
  {
    int idx;
    idx = DefaultArrayMap::registerArray(numElements, aid);

    return idx;
  }

  int procNum(int arrayHdl, const CkArrayIndex& i)
  {
    int flati = 0;
    if (amaps[arrayHdl]->_nelems.dimension == 0)
    {
      return RRMap::procNum(arrayHdl, i);
    }

    if (i.dimension == 1)
    {
      flati = i.data()[0];
    }
    else if (i.dimension == 2)
    {
      flati = i.data()[0] * amaps[arrayHdl]->_nelems.data()[1] + i.data()[1];
    }
    else if (i.dimension == 3)
    {
      flati = (i.data()[0] * amaps[arrayHdl]->_nelems.data()[1] + i.data()[1]) *
                  amaps[arrayHdl]->_nelems.data()[2] +
              i.data()[2];
    }
    else if (i.dimension == 4)
    {
      flati = (int)(((((short int*)i.data())[0] *
                          ((short int*)amaps[arrayHdl]->_nelems.data())[1] +
                      ((short int*)i.data())[1]) *
                         ((short int*)amaps[arrayHdl]->_nelems.data())[2] +
                     ((short int*)i.data())[2]) *
                        ((short int*)amaps[arrayHdl]->_nelems.data())[3] +
                    ((short int*)i.data())[3]);
    }
    else if (i.dimension == 5)
    {
      flati = (int)((((((short int*)i.data())[0] *
                           ((short int*)amaps[arrayHdl]->_nelems.data())[1] +
                       ((short int*)i.data())[1]) *
                          ((short int*)amaps[arrayHdl]->_nelems.data())[2] +
                      ((short int*)i.data())[2]) *
                         ((short int*)amaps[arrayHdl]->_nelems.data())[3] +
                     ((short int*)i.data())[3]) *
                        ((short int*)amaps[arrayHdl]->_nelems.data())[4] +
                    ((short int*)i.data())[4]);
    }
    else if (i.dimension == 6)
    {
      flati = (int)(((((((short int*)i.data())[0] *
                            ((short int*)amaps[arrayHdl]->_nelems.data())[1] +
                        ((short int*)i.data())[1]) *
                           ((short int*)amaps[arrayHdl]->_nelems.data())[2] +
                       ((short int*)i.data())[2]) *
                          ((short int*)amaps[arrayHdl]->_nelems.data())[3] +
                      ((short int*)i.data())[3]) *
                         ((short int*)amaps[arrayHdl]->_nelems.data())[4] +
                     ((short int*)i.data())[4]) *
                        ((short int*)amaps[arrayHdl]->_nelems.data())[5] +
                    ((short int*)i.data())[5]);
    }
#if CMK_ERROR_CHECKING
    else
    {
      CkAbort("CkArrayIndex has more than 6 dimensions!");
    }
#endif

    /** binSize used in DefaultArrayMap is the floor of numChares/numPes
     *  but for this FastArrayMap, we need the ceiling */
    return (flati / amaps[arrayHdl]->_binSizeCeil);
  }

  void pup(PUP::er& p) { DefaultArrayMap::pup(p); }
};

/* *
 * Hilbert map object -- This does hilbert mapping.
 * Convert array indices into 1D fashion according to their Hilbert filling curve
 */

typedef struct
{
  int intIndex;
  std::vector<int> coords;
} hilbert_pair;

bool operator==(hilbert_pair p1, hilbert_pair p2) { return p1.intIndex == p2.intIndex; }

bool myCompare(hilbert_pair p1, hilbert_pair p2) { return p1.intIndex < p2.intIndex; }

class HilbertArrayMap : public DefaultArrayMap
{
  std::vector<int> allpairs;
  std::vector<int> procList;

public:
  HilbertArrayMap(void)
  {
    procList.resize(CkNumPes());
    getHilbertList(procList.data());
    DEBC((AA "Creating HilbertArrayMap\n" AB));
  }

  HilbertArrayMap(CkMigrateMessage* m) : DefaultArrayMap(m) {}

  ~HilbertArrayMap() {}

  int registerArray(const CkArrayIndex& i, CkArrayID aid)
  {
    int idx;
    idx = DefaultArrayMap::registerArray(i, aid);

    if (i.dimension == 1)
    {
      // CkPrintf("1D %d\n", amaps[idx]->_nelems.data()[0]);
    }
    else if (i.dimension == 2)
    {
      // CkPrintf("2D %d:%d\n", amaps[idx]->_nelems.data()[0],
      // amaps[idx]->_nelems.data()[1]);
      const int dims = 2;
      int nDim0 = amaps[idx]->_nelems.data()[0];
      int nDim1 = amaps[idx]->_nelems.data()[1];
      int index;
      int counter = 0;
      std::vector<int> coords;
      allpairs.resize((size_t)nDim0 * nDim1);
      coords.resize(dims);
      for (int i = 0; i < nDim0; i++)
        for (int j = 0; j < nDim1; j++)
        {
          coords[0] = i;
          coords[1] = j;
          index = Hilbert_to_int(coords, dims);
          // CkPrintf("(%d:%d)----------> %d \n", i, j, index);
          allpairs[counter] = index;
          counter++;
        }
    }
    else if (i.dimension == 3)
    {
      // CkPrintf("3D %d:%d:%d\n", amaps[idx]->_nelems.data()[0],
      // amaps[idx]->_nelems.data()[1],
      //        amaps[idx]->_nelems.data()[2]);
      const int dims = 3;
      int nDim0 = amaps[idx]->_nelems.data()[0];
      int nDim1 = amaps[idx]->_nelems.data()[1];
      int nDim2 = amaps[idx]->_nelems.data()[2];
      int index;
      int counter = 0;
      std::vector<int> coords;
      allpairs.resize((size_t)nDim0 * nDim1 * nDim2);
      coords.resize(dims);
      for (int i = 0; i < nDim0; i++)
        for (int j = 0; j < nDim1; j++)
          for (int k = 0; k < nDim2; k++)
          {
            coords[0] = i;
            coords[1] = j;
            coords[2] = k;
            index = Hilbert_to_int(coords, dims);
            allpairs[counter] = index;
            counter++;
          }
    }
    else if (i.dimension == 4)
    {
      // CkPrintf("4D %hd:%hd:%hd:%hd\n", ((short int*)amaps[idx]->_nelems.data())[0],
      //        ((short int*)amaps[idx]->_nelems.data())[1], ((short
      //        int*)amaps[idx]->_nelems.data())[2],
      //        ((short int*)amaps[idx]->_nelems.data())[3]);
      const int dims = 4;
      int nDim[dims];
      for (int k = 0; k < dims; k++)
      {
        nDim[k] = (int)((short int*)amaps[idx]->_nelems.data())[k];
      }
      int index;
      int counter = 0;
      std::vector<int> coords;
      allpairs.resize((size_t)nDim[0] * nDim[1] * nDim[2] * nDim[3]);
      coords.resize(dims);
      for (int i = 0; i < nDim[0]; i++)
        for (int j = 0; j < nDim[1]; j++)
          for (int k = 0; k < nDim[2]; k++)
            for (int x = 0; x < nDim[3]; x++)
            {
              coords[0] = i;
              coords[1] = j;
              coords[2] = k;
              coords[3] = x;
              index = Hilbert_to_int(coords, dims);
              allpairs[counter] = index;
              counter++;
            }
    }
    else if (i.dimension == 5)
    {
      // CkPrintf("5D %hd:%hd:%hd:%hd:%hd\n", ((short int*)amaps[idx]->_nelems.data())[0],
      //        ((short int*)amaps[idx]->_nelems.data())[1], ((short
      //        int*)amaps[idx]->_nelems.data())[2],
      //        ((short int*)amaps[idx]->_nelems.data())[3], ((short
      //        int*)amaps[idx]->_nelems.data())[4]);
      const int dims = 5;
      int nDim[dims];
      for (int k = 0; k < dims; k++)
      {
        nDim[k] = (int)((short int*)amaps[idx]->_nelems.data())[k];
      }
      int index;
      int counter = 0;
      std::vector<int> coords;
      allpairs.resize((size_t)nDim[0] * nDim[1] * nDim[2] * nDim[3] * nDim[4]);
      coords.resize(dims);
      for (int i = 0; i < nDim[0]; i++)
        for (int j = 0; j < nDim[1]; j++)
          for (int k = 0; k < nDim[2]; k++)
            for (int x = 0; x < nDim[3]; x++)
              for (int y = 0; y < nDim[4]; y++)
              {
                coords[0] = i;
                coords[1] = j;
                coords[2] = k;
                coords[3] = x;
                coords[4] = y;
                index = Hilbert_to_int(coords, dims);
                allpairs[counter] = index;
                counter++;
              }
    }
    else if (i.dimension == 6)
    {
      // CkPrintf("6D %hd:%hd:%hd:%hd:%hd:%hd\n", ((short
      // int*)amaps[idx]->_nelems.data())[0],
      //        ((short int*)amaps[idx]->_nelems.data())[1], ((short
      //        int*)amaps[idx]->_nelems.data())[2],
      //        ((short int*)amaps[idx]->_nelems.data())[3], ((short
      //        int*)amaps[idx]->_nelems.data())[4],
      //        ((short int*)amaps[idx]->_nelems.data())[5]);
      const int dims = 6;
      int nDim[dims];
      for (int k = 0; k < dims; k++)
      {
        nDim[k] = (int)((short int*)amaps[idx]->_nelems.data())[k];
      }
      int index;
      int counter = 0;
      std::vector<int> coords;
      allpairs.resize((size_t)nDim[0] * nDim[1] * nDim[2] * nDim[3] * nDim[4] * nDim[5]);
      coords.resize(dims);
      for (int i = 0; i < nDim[0]; i++)
        for (int j = 0; j < nDim[1]; j++)
          for (int k = 0; k < nDim[2]; k++)
            for (int x = 0; x < nDim[3]; x++)
              for (int y = 0; y < nDim[4]; y++)
                for (int z = 0; z < nDim[5]; z++)
                {
                  coords[0] = i;
                  coords[1] = j;
                  coords[2] = k;
                  coords[3] = x;
                  coords[4] = y;
                  coords[5] = y;
                  index = Hilbert_to_int(coords, dims);
                  allpairs[counter] = index;
                  counter++;
                }
    }
    return idx;
  }

  int procNum(int arrayHdl, const CkArrayIndex& i)
  {
    int flati = 0;
    int myInt;
    if (amaps[arrayHdl]->_nelems.dimension == 0)
    {
      return RRMap::procNum(arrayHdl, i);
    }
    if (i.dimension == 1)
    {
      flati = i.data()[0];
    }
    else if (i.dimension == 2)
    {
      int nDim1 = amaps[arrayHdl]->_nelems.data()[1];
      myInt = i.data()[0] * nDim1 + i.data()[1];
      flati = allpairs[myInt];
    }
    else if (i.dimension == 3)
    {
      hilbert_pair mypair;
      mypair.coords.resize(3);
      int nDim[2];
      for (int j = 0; j < 2; j++)
      {
        nDim[j] = amaps[arrayHdl]->_nelems.data()[j + 1];
      }
      myInt = i.data()[0] * nDim[0] * nDim[1] + i.data()[1] * nDim[1] + i.data()[2];
      flati = allpairs[myInt];
    }
    else if (i.dimension == 4)
    {
      hilbert_pair mypair;
      mypair.coords.resize(4);
      short int nDim[3];
      for (int j = 0; j < 3; j++)
      {
        nDim[j] = ((short int*)amaps[arrayHdl]->_nelems.data())[j + 1];
      }
      myInt = (int)(((short int*)i.data())[0] * nDim[0] * nDim[1] * nDim[2] +
                    ((short int*)i.data())[1] * nDim[1] * nDim[2] +
                    ((short int*)i.data())[2] * nDim[2] + ((short int*)i.data())[3]);
      flati = allpairs[myInt];
    }
    else if (i.dimension == 5)
    {
      hilbert_pair mypair;
      mypair.coords.resize(5);
      short int nDim[4];
      for (int j = 0; j < 4; j++)
      {
        nDim[j] = ((short int*)amaps[arrayHdl]->_nelems.data())[j + 1];
      }
      myInt = (int)(((short int*)i.data())[0] * nDim[0] * nDim[1] * nDim[2] * nDim[3] +
                    ((short int*)i.data())[1] * nDim[1] * nDim[2] * nDim[3] +
                    ((short int*)i.data())[2] * nDim[2] * nDim[3] +
                    ((short int*)i.data())[3] * nDim[3] + ((short int*)i.data())[4]);
      flati = allpairs[myInt];
    }
    else if (i.dimension == 6)
    {
      hilbert_pair mypair;
      mypair.coords.resize(6);
      short int nDim[5];
      for (int j = 0; j < 5; j++)
      {
        nDim[j] = ((short int*)amaps[arrayHdl]->_nelems.data())[j + 1];
      }
      myInt = (int)(((short int*)i.data())[0] * nDim[0] * nDim[1] * nDim[2] * nDim[3] *
                        nDim[4] +
                    ((short int*)i.data())[1] * nDim[1] * nDim[2] * nDim[3] * nDim[4] +
                    ((short int*)i.data())[2] * nDim[2] * nDim[3] * nDim[4] +
                    ((short int*)i.data())[3] * nDim[3] * nDim[4] +
                    ((short int*)i.data())[4] * nDim[4] + ((short int*)i.data())[5]);
      flati = allpairs[myInt];
    }
#if CMK_ERROR_CHECKING
    else
    {
      CkAbort("CkArrayIndex has more than 6 dimensions!");
    }
#endif

    /** binSize used in DefaultArrayMap is the floor of numChares/numPes
     *  but for this FastArrayMap, we need the ceiling */
    int block = flati / amaps[arrayHdl]->_binSizeCeil;
    // for(int i=0; i<CkNumPes(); i++)
    //    CkPrintf("(%d:%d) ", i, procList[i]);
    // CkPrintf("\n");
    // CkPrintf("block [%d:%d]\n", block, procList[block]);
    return procList[block];
  }

  void pup(PUP::er& p) { DefaultArrayMap::pup(p); }
};

/**
 * This map can be used for topology aware mapping when the mapping is provided
 * through a file -- ASB
 */
class ReadFileMap : public DefaultArrayMap
{
private:
  std::vector<int> mapping;

public:
  ReadFileMap(void) { DEBC((AA "Creating ReadFileMap\n" AB)); }

  ReadFileMap(CkMigrateMessage* m) : DefaultArrayMap(m) {}

  int registerArray(const CkArrayIndex& numElements, CkArrayID aid)
  {
    int idx;
    idx = DefaultArrayMap::registerArray(numElements, aid);

    if (mapping.size() == 0)
    {
      int numChares;

      if (amaps[idx]->_nelems.dimension == 1)
      {
        numChares = amaps[idx]->_nelems.data()[0];
      }
      else if (amaps[idx]->_nelems.dimension == 2)
      {
        numChares = amaps[idx]->_nelems.data()[0] * amaps[idx]->_nelems.data()[1];
      }
      else if (amaps[idx]->_nelems.dimension == 3)
      {
        numChares = amaps[idx]->_nelems.data()[0] * amaps[idx]->_nelems.data()[1] *
                    amaps[idx]->_nelems.data()[2];
      }
      else if (amaps[idx]->_nelems.dimension == 4)
      {
        numChares = (int)(((short int*)amaps[idx]->_nelems.data())[0] *
                          ((short int*)amaps[idx]->_nelems.data())[1] *
                          ((short int*)amaps[idx]->_nelems.data())[2] *
                          ((short int*)amaps[idx]->_nelems.data())[3]);
      }
      else if (amaps[idx]->_nelems.dimension == 5)
      {
        numChares = (int)(((short int*)amaps[idx]->_nelems.data())[0] *
                          ((short int*)amaps[idx]->_nelems.data())[1] *
                          ((short int*)amaps[idx]->_nelems.data())[2] *
                          ((short int*)amaps[idx]->_nelems.data())[3] *
                          ((short int*)amaps[idx]->_nelems.data())[4]);
      }
      else if (amaps[idx]->_nelems.dimension == 6)
      {
        numChares = (int)(((short int*)amaps[idx]->_nelems.data())[0] *
                          ((short int*)amaps[idx]->_nelems.data())[1] *
                          ((short int*)amaps[idx]->_nelems.data())[2] *
                          ((short int*)amaps[idx]->_nelems.data())[3] *
                          ((short int*)amaps[idx]->_nelems.data())[4] *
                          ((short int*)amaps[idx]->_nelems.data())[5]);
      }
      else
      {
        CkAbort("CkArrayIndex has more than 6 dimension!");
      }

      mapping.resize(numChares);
      FILE* mapf = fopen("mapfile", "r");
      TopoManager tmgr;
      int x, y, z, t;

      for (int i = 0; i < numChares; i++)
      {
        if (fscanf(mapf, "%d %d %d %d", &x, &y, &z, &t) != 4)
        {
          CkAbort("ReadFileMap> reading from mapfile failed!");
        }
        mapping[i] = tmgr.coordinatesToRank(x, y, z, t);
      }
      fclose(mapf);
    }

    return idx;
  }

  int procNum(int arrayHdl, const CkArrayIndex& i)
  {
    int flati;

    if (i.dimension == 1)
    {
      flati = i.data()[0];
    }
    else if (i.dimension == 2)
    {
      flati = i.data()[0] * amaps[arrayHdl]->_nelems.data()[1] + i.data()[1];
    }
    else if (i.dimension == 3)
    {
      flati = (i.data()[0] * amaps[arrayHdl]->_nelems.data()[1] + i.data()[1]) *
                  amaps[arrayHdl]->_nelems.data()[2] +
              i.data()[2];
    }
    else if (i.dimension == 4)
    {
      flati = (int)(((((short int*)i.data())[0] *
                          ((short int*)amaps[arrayHdl]->_nelems.data())[1] +
                      ((short int*)i.data())[1]) *
                         ((short int*)amaps[arrayHdl]->_nelems.data())[2] +
                     ((short int*)i.data())[2]) *
                        ((short int*)amaps[arrayHdl]->_nelems.data())[3] +
                    ((short int*)i.data())[3]);
    }
    else if (i.dimension == 5)
    {
      flati = (int)((((((short int*)i.data())[0] *
                           ((short int*)amaps[arrayHdl]->_nelems.data())[1] +
                       ((short int*)i.data())[1]) *
                          ((short int*)amaps[arrayHdl]->_nelems.data())[2] +
                      ((short int*)i.data())[2]) *
                         ((short int*)amaps[arrayHdl]->_nelems.data())[3] +
                     ((short int*)i.data())[3]) *
                        ((short int*)amaps[arrayHdl]->_nelems.data())[4] +
                    ((short int*)i.data())[4]);
    }
    else if (i.dimension == 6)
    {
      flati = (int)(((((((short int*)i.data())[0] *
                            ((short int*)amaps[arrayHdl]->_nelems.data())[1] +
                        ((short int*)i.data())[1]) *
                           ((short int*)amaps[arrayHdl]->_nelems.data())[2] +
                       ((short int*)i.data())[2]) *
                          ((short int*)amaps[arrayHdl]->_nelems.data())[3] +
                      ((short int*)i.data())[3]) *
                         ((short int*)amaps[arrayHdl]->_nelems.data())[4] +
                     ((short int*)i.data())[4]) *
                        ((short int*)amaps[arrayHdl]->_nelems.data())[5] +
                    ((short int*)i.data())[5]);
    }
    else
    {
      CkAbort("CkArrayIndex has more than 6 dimensions!");
    }

    return mapping[flati];
  }

  void pup(PUP::er& p)
  {
    DefaultArrayMap::pup(p);
    p | mapping;
  }
};

/**
 * This map can be used for a simple (non-topology-aware) mapping of a 1D array when the
 * mapping is provided through a file
 */
class Simple1DFileMap : public DefaultArrayMap
{
private:
  std::vector<int> mapping;

public:
  Simple1DFileMap(void) {
    DEBC((AA "Creating Simple1DFileMap\n" AB));
  }

  Simple1DFileMap(CkMigrateMessage *m) : DefaultArrayMap(m){}

  int registerArray(const CkArrayIndex& numElements, CkArrayID aid)
  {
    int idx = DefaultArrayMap::registerArray(numElements, aid);

    if(mapping.size() == 0) {
      int numChares;

      if (amaps[idx]->_nelems.dimension == 1) {
        numChares = amaps[idx]->_nelems.data()[0];
      } else {
        CkAbort("CkArrayIndex has more than 1 dimension for a Simple1DFileMap!");
      }

      mapping.resize(numChares);
      FILE *mapf = fopen("mapfile", "r");
      if (mapf == NULL) {
        CkAbort("Simple1DFileMap failed to open file named 'mapfile'!");
      }
      int pe;

      for(int i=0; i<numChares; i++) {
        if (fscanf(mapf, "%d\n", &pe) != 1) {
          CkAbort("Simple1DFileMap> reading from mapfile failed! Expected one int per line, one line per chare array element...");
        }
        mapping[i] = pe;
      }
      fclose(mapf);
    }

    return idx;
  }

  int procNum(int arrayHdl, const CkArrayIndex &i) {
    int flati;

    if (i.dimension == 1) {
      flati = i.data()[0];
    } else {
      CkAbort("CkArrayIndex has more than 1 dimension for a 1D map!");
    }

    return mapping[flati];
  }

  void pup(PUP::er& p){
    DefaultArrayMap::pup(p);
    p|mapping;
  }
};

// This is currently  here for backwards compatibility
class BlockMap : public DefaultArrayMap
{
public:
  BlockMap() {}
  BlockMap(CkMigrateMessage* m) : DefaultArrayMap(m) {}
};

/**
 * map object-- use seed load balancer.
 */
class CldMap : public CkArrayMap
{
public:
  CldMap(void) { DEBC((AA "Creating CldMap\n" AB)); }
  CldMap(CkMigrateMessage* m) : CkArrayMap(m) {}
  int homePe(int /*arrayHdl*/, const CkArrayIndex& i)
  {
    if (i.dimension == 1)
    {
      // Map 1D integer indices in simple round-robin fashion
      return (i.data()[0]) % CkNumPes();
    }
    else
    {
      // Map other indices based on their hash code, mod a big prime.
      unsigned int hash = (i.hash() + 739) % 1280107;
      return (hash % CkNumPes());
    }
  }
  int procNum(int arrayHdl, const CkArrayIndex& i)
  {
    return CLD_ANYWHERE;  // -1
  }
  void populateInitial(int arrayHdl, CkArrayOptions& options, void* ctorMsg, CkArray* mgr)
  {
    CkArrayIndex start = options.getStart();
    CkArrayIndex end = options.getEnd();
    CkArrayIndex step = options.getStep();
    if (end.dimension == 0)
    {
      CkFreeMsg(ctorMsg);
      return;
    }
    int thisPe = CkMyPe();
    int numPes = CkNumPes();

    CKARRAYMAP_POPULATE_INITIAL(i % numPes == thisPe);
    CkFreeMsg(ctorMsg);
  }
};

/// A class responsible for parsing the command line arguments for the PE
/// to extract the format string passed in with +ConfigurableRRMap
class ConfigurableRRMapLoader
{
public:
  std::vector<int> locations;
  int objs_per_block;
  int PE_per_block;

  /// labels for states used when parsing the ConfigurableRRMap from ARGV
  enum ConfigurableRRMapLoadStatus : uint8_t
  {
    not_loaded,
    loaded_found,
    loaded_not_found
  };

  enum ConfigurableRRMapLoadStatus state;

  ConfigurableRRMapLoader()
  {
    state = not_loaded;
    objs_per_block = 0;
    PE_per_block = 0;
  }

  /// load configuration if possible, and return whether a valid configuration exists
  bool haveConfiguration()
  {
    if (state == not_loaded)
    {
      DEBUG(("[%d] loading ConfigurableRRMap configuration\n", CkMyPe()));
      char** argv = CkGetArgv();
      char* configuration = NULL;
      bool found = CmiGetArgString(argv, "+ConfigurableRRMap", &configuration);
      if (!found)
      {
        DEBUG(("Couldn't find +ConfigurableRRMap command line argument\n"));
        state = loaded_not_found;
        return false;
      }
      else
      {
        DEBUG(("Found +ConfigurableRRMap command line argument in %p=\"%s\"\n",
               configuration, configuration));

        std::istringstream instream(configuration);
        CkAssert(instream.good());

        // Example line:
        // 10 8 0 1 2 3 4 5 6 7 7 7 7
        // Map 10 objects to 8 PEs, with each object's index among the 8 PEs.

        // extract first integer
        instream >> objs_per_block >> PE_per_block;
        CkAssert(instream.good());
        CkAssert(objs_per_block > 0);
        CkAssert(PE_per_block > 0);
        locations.resize(objs_per_block);
        for (int i = 0; i < objs_per_block; i++)
        {
          locations[i] = 0;
          CkAssert(instream.good());
          instream >> locations[i];
          CkAssert(locations[i] < PE_per_block);
        }
        state = loaded_found;
        return true;
      }
    }
    else
    {
      DEBUG(("[%d] ConfigurableRRMap has already been loaded\n", CkMyPe()));
      return state == loaded_found;
    }
  }
};

CkpvDeclare(ConfigurableRRMapLoader, myConfigRRMapState);

void _initConfigurableRRMap()
{
  CkpvInitialize(ConfigurableRRMapLoader, myConfigRRMapState);
}

/// Try to load the command line arguments for ConfigurableRRMap
bool haveConfigurableRRMap()
{
  DEBUG(("haveConfigurableRRMap()\n"));
  ConfigurableRRMapLoader& loader = CkpvAccess(myConfigRRMapState);
  return loader.haveConfiguration();
}

class ConfigurableRRMap : public RRMap
{
public:
  ConfigurableRRMap(void) { DEBC((AA "Creating ConfigurableRRMap\n" AB)); }
  ConfigurableRRMap(CkMigrateMessage* m) : RRMap(m) {}

  void populateInitial(int arrayHdl, CkArrayOptions& options, void* ctorMsg, CkArray* mgr)
  {
    CkArrayIndex end = options.getEnd();
    // Try to load the configuration from command line argument
    CkAssert(haveConfigurableRRMap());
    ConfigurableRRMapLoader& loader = CkpvAccess(myConfigRRMapState);
    if (end.dimension == 0)
    {
      CkFreeMsg(ctorMsg);
      return;
    }
    int thisPe = CkMyPe();
    int maxIndex = end.data()[0];
    DEBUG(("[%d] ConfigurableRRMap: index=%d,%d,%d\n", CkMyPe(), (int)end.data()[0],
           (int)end.data()[1], (int)end.data()[2]));

    if (end.dimension != 1)
    {
      CkAbort("ConfigurableRRMap only supports dimension 1!");
    }

    for (int index = 0; index < maxIndex; index++)
    {
      CkArrayIndex1D idx(index);

      int cyclic_block = index / loader.objs_per_block;
      int cyclic_local = index % loader.objs_per_block;
      int l = loader.locations[cyclic_local];
      int PE = (cyclic_block * loader.PE_per_block + l) % CkNumPes();

      DEBUG(("[%d] ConfigurableRRMap: index=%d is located on PE %d l=%d\n", CkMyPe(),
             (int)index, (int)PE, l));

      if (PE == thisPe)
        mgr->insertInitial(idx, CkCopyMsg(&ctorMsg));
    }
    //        CKARRAYMAP_POPULATE_INITIAL(PE == thisPe);

    CkFreeMsg(ctorMsg);
  }
};

CkpvStaticDeclare(double*, rem);

class arrInfo
{
private:
  CkArrayIndex _nelems;
  std::vector<int> _map;

public:
  arrInfo() {}
  arrInfo(const CkArrayIndex& n, int* _speeds)
      : _nelems(n), _map(_nelems.getCombinedCount())
  {
    distrib(_speeds);
  }
  ~arrInfo() {}
  int getMap(const CkArrayIndex& i);
  void distrib(int* speeds);
  void pup(PUP::er& p)
  {
    p | _nelems;
    p | _map;
  }
};

static int cmp(const void* first, const void* second)
{
  int fi = *((const int*)first);
  int si = *((const int*)second);
  return ((CkpvAccess(rem)[fi] == CkpvAccess(rem)[si])
              ? 0
              : ((CkpvAccess(rem)[fi] < CkpvAccess(rem)[si]) ? 1 : (-1)));
}

void arrInfo::distrib(int* speeds)
{
  int _nelemsCount = _nelems.getCombinedCount();
  double total = 0.0;
  int npes = CkNumPes();
  int i, j, k;
  for (i = 0; i < npes; i++) total += (double)speeds[i];
  std::vector<double> nspeeds(npes);
  for (i = 0; i < npes; i++) nspeeds[i] = (double)speeds[i] / total;
  std::vector<int> cp(npes);
  for (i = 0; i < npes; i++) cp[i] = (int)(nspeeds[i] * _nelemsCount);
  int nr = 0;
  for (i = 0; i < npes; i++) nr += cp[i];
  nr = _nelemsCount - nr;
  if (nr != 0)
  {
    CkpvAccess(rem) = new double[npes];
    for (i = 0; i < npes; i++)
      CkpvAccess(rem)[i] = (double)_nelemsCount * nspeeds[i] - cp[i];
    std::vector<int> pes(npes);
    for (i = 0; i < npes; i++) pes[i] = i;
    qsort(pes.data(), npes, sizeof(int), cmp);
    for (i = 0; i < nr; i++) cp[pes[i]]++;
    delete[] CkpvAccess(rem);
  }
  k = 0;
  for (i = 0; i < npes; i++)
  {
    for (j = 0; j < cp[i]; j++) _map[k++] = i;
  }
}

int arrInfo::getMap(const CkArrayIndex& i)
{
  if (i.dimension == 1)
    return _map[i.data()[0]];
  else
    return _map[((i.hash() + 739) % 1280107) % _nelems.getCombinedCount()];
}

// Speeds maps processor number to "speed" (some sort of iterations per second counter)
// It is initialized by processor 0.
static int* globalSpeeds;

#if CMK_USE_PROP_MAP
typedef struct _speedmsg
{
  char hdr[CmiMsgHeaderSizeBytes];
  int node;
  int speed;
} speedMsg;

static void _speedHdlr(void* m)
{
  speedMsg* msg = (speedMsg*)m;
  if (CmiMyRank() == 0)
    for (int pe = 0; pe < CmiNodeSize(msg->node); pe++)
      globalSpeeds[CmiNodeFirst(msg->node) + pe] = msg->speed;
  CmiFree(m);
}

// initnode call
void _propMapInit(void)
{
  globalSpeeds = new int[CkNumPes()];
  int hdlr = CkRegisterHandler(_speedHdlr);
  CmiPrintf("[%d]Measuring processor speed for prop. mapping...\n", CkMyPe());
  int s = LBManager::ProcessorSpeed();
  speedMsg msg;
  CmiSetHandler(&msg, hdlr);
  msg.node = CkMyNode();
  msg.speed = s;
  CmiSyncBroadcastAllAndFree(sizeof(msg), &msg);
  for (int i = 0; i < CkNumNodes(); i++) CmiDeliverSpecificMsg(hdlr);
}
#else
void _propMapInit(void)
{
  globalSpeeds = new int[CkNumPes()];
  int i;
  for (i = 0; i < CkNumPes(); i++) globalSpeeds[i] = 1;
}
#endif
/**
 * A proportional map object-- tries to map more objects to
 * faster processors and fewer to slower processors.  Also
 * attempts to ensure good locality by mapping nearby elements
 * together.
 */
class PropMap : public CkArrayMap
{
private:
  CkPupPtrVec<arrInfo> arrs;

public:
  PropMap(void)
  {
    CkpvInitialize(double*, rem);
    DEBC((AA "Creating PropMap\n" AB));
  }
  PropMap(CkMigrateMessage* m) {}
  int registerArray(const CkArrayIndex& numElements, CkArrayID aid)
  {
    int idx = arrs.size();
    arrs.resize(idx + 1);
    arrs[idx] = new arrInfo(numElements, globalSpeeds);
    return idx;
  }
  void unregisterArray(int idx) { arrs[idx].destroy(); }
  int procNum(int arrayHdl, const CkArrayIndex& i) { return arrs[arrayHdl]->getMap(i); }
  void pup(PUP::er& p)
  {
    int oldNumPes = -1;
    if (p.isPacking())
    {
      oldNumPes = CkNumPes();
    }
    p | oldNumPes;
    p | arrs;
    if (p.isUnpacking() && oldNumPes != CkNumPes())
    {
      for (int idx = 0; idx < arrs.length(); ++idx)
      {
        arrs[idx]->distrib(globalSpeeds);
      }
    }
  }
};

class CkMapsInit : public Chare
{
public:
  CkMapsInit(CkArgMsg* msg)
  {
    //_defaultArrayMapID = CProxy_HilbertArrayMap::ckNew();
    _defaultArrayMapID = CProxy_DefaultArrayMap::ckNew();
    _fastArrayMapID = CProxy_FastArrayMap::ckNew();
    delete msg;
  }

  CkMapsInit(CkMigrateMessage* m) {}
};

// given an envelope of a Charm msg, find the recipient object pointer
CkMigratable* CkArrayMessageObjectPtr(envelope* env)
{
  if (env->getMsgtype() != ForArrayEltMsg)
    return NULL;  // not an array msg

  ///@todo: Delegate this to the array manager which can then deal with ForArrayEltMsg
  CkArray* mgr = CProxy_CkArray(env->getArrayMgr()).ckLocalBranch();
  return mgr ? mgr->lookup(ck::ObjID(env->getRecipientID()).getElementID()) : NULL;
}

/****************************** Out-of-Core support ********************/

#if CMK_OUT_OF_CORE
CooPrefetchManager CkArrayElementPrefetcher;
CkpvDeclare(int, CkSaveRestorePrefetch);

/**
 * Return the out-of-core objid (from CooRegisterObject)
 * that this Converse message will access.  If the message
 * will not access an object, return -1.
 */
int CkArrayPrefetch_msg2ObjId(void* msg)
{
  envelope* env = (envelope*)msg;
  CkMigratable* elt = CkArrayMessageObjectPtr(env);
  return elt ? elt->prefetchObjID : -1;
}

/**
 * Write this object (registered with RegisterObject)
 * to this writable file.
 */
void CkArrayPrefetch_writeToSwap(FILE* swapfile, void* objptr)
{
  CkMigratable* elt = (CkMigratable*)objptr;

  // Save the element's data to disk:
  PUP::toDisk p(swapfile);
  elt->virtual_pup(p);

  // Call the element's destructor in-place (so pointer doesn't change)
  CkpvAccess(CkSaveRestorePrefetch) = 1;
  elt->~CkMigratable();  //< because destructor is virtual, destroys user class too.
  CkpvAccess(CkSaveRestorePrefetch) = 0;
}

/**
 * Read this object (registered with RegisterObject)
 * from this readable file.
 */
void CkArrayPrefetch_readFromSwap(FILE* swapfile, void* objptr)
{
  CkMigratable* elt = (CkMigratable*)objptr;
  // Call the element's migration constructor in-place
  CkpvAccess(CkSaveRestorePrefetch) = 1;
  int ctorIdx = _chareTable[elt->thisChareType]->migCtor;
  elt->myRec->invokeEntry(elt, (CkMigrateMessage*)0, ctorIdx, true);
  CkpvAccess(CkSaveRestorePrefetch) = 0;

  // Restore the element's data from disk:
  PUP::fromDisk p(swapfile);
  elt->virtual_pup(p);
}

static void _CkMigratable_prefetchInit(void)
{
  CkpvExtern(int, CkSaveRestorePrefetch);
  CkpvAccess(CkSaveRestorePrefetch) = 0;
  CkArrayElementPrefetcher.msg2ObjId = CkArrayPrefetch_msg2ObjId;
  CkArrayElementPrefetcher.writeToSwap = CkArrayPrefetch_writeToSwap;
  CkArrayElementPrefetcher.readFromSwap = CkArrayPrefetch_readFromSwap;
  CooRegisterManager(&CkArrayElementPrefetcher, _charmHandlerIdx);
}
#endif

/****************************** CkMigratable ***************************/
/**
 * This tiny class is used to convey information to the
 * newly created CkMigratable object when its constructor is called.
 */

CkpvDeclare(CkMigratable_initInfo, mig_initInfo);

void _CkMigratable_initInfoInit(void)
{
  CkpvInitialize(CkMigratable_initInfo, mig_initInfo);
#if CMK_OUT_OF_CORE
  _CkMigratable_prefetchInit();
#endif
}

void CkMigratable::commonInit(void)
{
  CkMigratable_initInfo& i = CkpvAccess(mig_initInfo);
#if CMK_OUT_OF_CORE
  isInCore = true;
  if (CkpvAccess(CkSaveRestorePrefetch))
    return;            /* Just restoring from disk--don't touch object */
  prefetchObjID = -1;  // Unregistered
#endif
  myRec = i.locRec;
  thisIndexMax = myRec->getIndex();
  thisChareType = i.chareType;
  usesAtSync = false;
  usesAutoMeasure = true;
  barrierRegistered = false;

  local_state = OFF;
  prev_load = 0.0;
  can_reset = false;

#if CMK_LBDB_ON
  if (_lb_args.metaLbOn())
  {
    atsync_iteration = myRec->getMetaBalancer()->get_iteration();
    myRec->getMetaBalancer()->AdjustCountForNewContributor(atsync_iteration);
  }
#endif
}

CkMigratable::CkMigratable(void)
{
  DEBC((AA "In CkMigratable constructor\n" AB));
  commonInit();
}
CkMigratable::CkMigratable(CkMigrateMessage* m) : Chare(m) { commonInit(); }

int CkMigratable::ckGetChareType(void) const { return thisChareType; }

void CkMigratable::pup(PUP::er& p)
{
  DEBM((AA "In CkMigratable::pup %s\n" AB, idx2str(thisIndexMax)));
  Chare::pup(p);
  p | thisIndexMax;
  p | usesAtSync;
  p | can_reset;
  p | usesAutoMeasure;

#if CMK_LBDB_ON
  bool readyMigrate = false;
  if (p.isPacking())
    readyMigrate = myRec->isReadyMigrate();
  p | readyMigrate;
  if (p.isUnpacking())
    myRec->ReadyMigrate(readyMigrate);
#endif

  int epoch = -1;
  // Only pup epoch when migrating. Will result in problems if pup'd when
  // checkpointing since it will not match the value of the freshly inited barrier
  // upon restart
  if (usesAtSync && p.isMigration())
  {
    if (p.isPacking())
    {
      epoch = (*ldBarrierHandle)->epoch;
    }
    p | epoch;
  }

  std::vector<LBRealType> position;
  if (!p.isUnpacking())
    position = myRec->getObjPosition();
  p | position;
  if (p.isUnpacking())
    myRec->setObjPosition(position);

  if (p.isUnpacking())
    ckFinishConstruction(epoch);
}

void CkMigratable::ckDestroy(void) {}
void CkMigratable::ckAboutToMigrate(void) {}
void CkMigratable::ckJustMigrated(void) {}
void CkMigratable::ckJustRestored(void) {}

CkMigratable::~CkMigratable()
{
  DEBC((AA "In CkMigratable::~CkMigratable %s\n" AB, idx2str(thisIndexMax)));
#if CMK_OUT_OF_CORE
  isInCore = false;
  if (CkpvAccess(CkSaveRestorePrefetch))
    return; /* Just saving to disk--don't deregister anything. */
  /* We're really leaving or dying-- unregister from the ooc system*/
  if (prefetchObjID != -1)
  {
    CooDeregisterObject(prefetchObjID);
    prefetchObjID = -1;
  }
#endif
#if CMK_LBDB_ON
  if (barrierRegistered)
  {
    DEBL((AA "Removing barrier for element %s\n" AB, idx2str(thisIndexMax)));
    if (usesAtSync)
      myRec->getSyncBarrier()->removeClient(ldBarrierHandle);
  }

  if (_lb_args.metaLbOn())
  {
    myRec->getMetaBalancer()->AdjustCountForDeadContributor(atsync_iteration);
  }
#endif
  myRec->destroy(); /* Attempt to delete myRec if it's no longer in use */
  // To detect use-after-delete
  thisIndexMax.nInts = 0;
  thisIndexMax.dimension = 0;
}

void CkMigratable::CkAbort(const char* format, ...) const
{
  char newmsg[256];
  va_list args;
  va_start(args, format);
  vsnprintf(newmsg, sizeof(newmsg), format, args);
  va_end(args);

  ::CkAbort("CkMigratable '%s' aborting: %s", _chareTable[thisChareType]->name, newmsg);
}

void CkMigratable::ResumeFromSync(void) {}

void CkMigratable::UserSetLBLoad()
{
  CkAbort("::UserSetLBLoad() not defined for this array element!\n");
}

#if CMK_LBDB_ON  // For load balancing:
// user can call this helper function to set obj load (for model-based lb)
void CkMigratable::setObjTime(double cputime) { myRec->setObjTime(cputime); }
void CkMigratable::setObjPosition(const std::vector<LBRealType>& pos)
{
  myRec->setObjPosition(pos);
}
double CkMigratable::getObjTime() { return myRec->getObjTime(); }

#  if CMK_LB_USER_DATA
/**
 * Use this method to set user specified data to the lbdatabase.
 *
 * Eg usage:
 * In the application code,
 *   void *data = getObjUserData(CkpvAccess(_lb_obj_index));
 *   *(int *) data = val;
 *
 * In the loadbalancer or wherever this data is used do
 *   for (int i = 0; i < stats->n_objs; i++ ) {
 *     LDObjData &odata = stats->objData[i];
 *     int* udata = (int *) odata.getUserData(CkpvAccess(_lb_obj_index));
 *   }
 *
 * For a complete example look at tests/charm++/load_balancing/lb_userdata_test/
 */
void* CkMigratable::getObjUserData(int idx) { return myRec->getObjUserData(idx); }
#  endif

void CkMigratable::clearMetaLBData()
{
  //  if (can_reset) {
  local_state = OFF;
  atsync_iteration = -1;
  prev_load = 0.0;
  can_reset = false;
  //  }
}

void CkMigratable::recvLBPeriod(void* data)
{
  if (atsync_iteration < 0)
  {
    return;
  }
  int lb_period = *((int*)data);
  DEBAD(("\t[obj %s] Received the LB Period %d current iter %d state %d on PE %d\n",
         idx2str(thisIndexMax), lb_period, atsync_iteration, local_state, CkMyPe()));

  bool is_tentative;
  if (local_state == LOAD_BALANCE)
  {
    CkAssert(lb_period == myRec->getMetaBalancer()->getPredictedLBPeriod(is_tentative));
    return;
  }

  if (local_state == PAUSE)
  {
    if (atsync_iteration < lb_period)
    {
      local_state = DECIDED;
      ResumeFromSync();
      return;
    }
    local_state = LOAD_BALANCE;

    can_reset = true;
    return;
  }
  local_state = DECIDED;
}

void CkMigratable::metaLBCallLB()
{
  if (usesAtSync)
    myRec->getSyncBarrier()->atBarrier(ldBarrierHandle);
}

void CkMigratable::ckFinishConstruction(int epoch)
{
  //	if ((!usesAtSync) || barrierRegistered) return;
  if (usesAtSync && _lb_args.lbperiod() != -1.0)
    CkAbort("You must use AtSync or Periodic LB separately!\n");

  myRec->setMeasure(usesAutoMeasure);
  if (barrierRegistered)
    return;
  DEBL((AA "Registering barrier client for %s\n" AB, idx2str(thisIndexMax)));
  if (usesAtSync)
  {
    ldBarrierHandle = myRec->getSyncBarrier()->addClient(
        this, [=]() { this->ResumeFromSyncHelper(); }, epoch);
  }
  barrierRegistered = true;
}

void CkMigratable::AtSync(int waitForMigration)
{
  if (!usesAtSync)
    CkAbort(
        "You must set usesAtSync=true in your array element constructor to use "
        "AtSync!\n");
  // Only actually call AtSync when a receiver exists, otherwise skip it and
  // directly call ResumeFromSync
  if (!myRec->getSyncBarrier()->hasReceivers())
  {
    ResumeFromSync();
    return;
  }
  myRec->AsyncMigrate(!waitForMigration);
  if (waitForMigration)
    ReadyMigrate(true);
  ckFinishConstruction();
  DEBL((AA "Element %s going to sync\n" AB, idx2str(thisIndexMax)));
  // model-based load balancing, ask user to provide cpu load
  if (usesAutoMeasure == false)
    UserSetLBLoad();

  if (_lb_psizer_on || _lb_args.metaLbOn())
  {
    PUP::sizer ps(PUP::er::IS_MIGRATION);
    this->virtual_pup(ps);
    if (_lb_psizer_on)
      setPupSize(ps.size());
    if (_lb_args.metaLbOn())
      myRec->getMetaBalancer()->SetCharePupSize(ps.size());
  }

  if (!_lb_args.metaLbOn())
  {
    myRec->getSyncBarrier()->atBarrier(ldBarrierHandle);
    return;
  }

  // When MetaBalancer is turned on

  if (atsync_iteration == -1)
  {
    can_reset = false;
    local_state = OFF;
    prev_load = 0.0;
  }

  atsync_iteration++;
  // CkPrintf("[pe %s] atsync_iter %d && predicted period %d state: %d\n",
  //    idx2str(thisIndexMax), atsync_iteration,
  //    myRec->getMetaBalancer()->getPredictedLBPeriod(), local_state);
  double tmp = prev_load;
  prev_load = myRec->getObjTime();
  double current_load = prev_load - tmp;

  // If the load for the chares are based on certain model, then set the
  // current_load to be whatever is the obj load.
  if (!usesAutoMeasure)
  {
    current_load = myRec->getObjTime();
  }

  if (atsync_iteration <= myRec->getMetaBalancer()->get_finished_iteration())
  {
    CkPrintf("[%d:%s] Error!! Contributing to iter %d < current iter %d\n", CkMyPe(),
             idx2str(thisIndexMax), atsync_iteration,
             myRec->getMetaBalancer()->get_finished_iteration());
    CkAbort("Not contributing to the right iteration\n");
  }

  if (atsync_iteration != 0)
  {
    myRec->getMetaBalancer()->AddLoad(atsync_iteration, current_load);
  }

  bool is_tentative;
  if (atsync_iteration < myRec->getMetaBalancer()->getPredictedLBPeriod(is_tentative))
  {
    ResumeFromSync();
  }
  else if (is_tentative)
  {
    local_state = PAUSE;
  }
  else if (local_state == DECIDED)
  {
    DEBAD(("[%d:%s] Went to load balance iter %d\n", CkMyPe(), idx2str(thisIndexMax),
           atsync_iteration));
    local_state = LOAD_BALANCE;
    can_reset = true;
  }
  else
  {
    DEBAD(("[%d:%s] Went to pause state iter %d\n", CkMyPe(), idx2str(thisIndexMax),
           atsync_iteration));
    local_state = PAUSE;
  }
}

void CkMigratable::ReadyMigrate(bool ready) { myRec->ReadyMigrate(ready); }

void CkMigratable::ResumeFromSyncHelper()
{
  DEBL((AA "Element %s resuming from sync\n" AB, idx2str(thisIndexMax)));

  if (_lb_args.metaLbOn())
  {
    clearMetaLBData();
  }

  CkLocMgr* localLocMgr = myRec->getLocMgr();
  auto iter = localLocMgr->bufferedActiveRgetMsgs.find(ckGetID());
  if (iter != localLocMgr->bufferedActiveRgetMsgs.end())
  {
    localLocMgr->toBeResumeFromSynced.emplace(ckGetID(), this);
  }
  else
  {
    ResumeFromSync();
  }
}

void CkMigratable::setMigratable(int migratable) { myRec->setMigratable(migratable); }

void CkMigratable::setPupSize(size_t obj_pup_size) { myRec->setPupSize(obj_pup_size); }

void CkMigratable::CkAddThreadListeners(CthThread tid, void* msg)
{
  Chare::CkAddThreadListeners(tid, msg);  // for trace
  CthSetThreadID(tid, thisIndexMax.data()[0], thisIndexMax.data()[1],
                 thisIndexMax.data()[2]);
}
#else
void CkMigratable::setObjTime(double cputime) {}
void CkMigratable::setObjPosition(const std::vector<LBRealType> pos) {}
double CkMigratable::getObjTime() { return 0.0; }

#  if CMK_LB_USER_DATA
void* CkMigratable::getObjUserData(int idx) { return NULL; }
#  endif

/* no load balancer: need dummy implementations to prevent link error */
void CkMigratable::CkAddThreadListeners(CthThread tid, void* msg) {}
#endif

/************************** Location Records: *********************************/

/*----------------- Local:
Matches up the array index with the local index, an
interfaces with the load balancer on behalf of the
represented array elements.
*/
CkLocRec::CkLocRec(CkLocMgr* mgr, bool fromMigration, bool ignoreArrival,
                   const CkArrayIndex& idx_, CmiUInt8 id_)
    : myLocMgr(mgr), idx(idx_), id(id_), deletedMarker(NULL), running(false)
{
#if CMK_LBDB_ON
  DEBL((AA "Registering element %s with load balancer\n" AB, idx2str(idx)));
  nextPe = -1;
  asyncMigrate = false;
  readyMigrate = true;
  enable_measure = true;
  syncBarrier = CkSyncBarrier::object();
  lbmgr = mgr->getLBMgr();
  if (_lb_args.metaLbOn())
    the_metalb = mgr->getMetaBalancer();
#  if CMK_GLOBAL_LOCATION_UPDATE
  id_ = ck::ObjID(mgr->getGroupID(), id_).getID();
#  endif
  ldHandle = lbmgr->RegisterObj(mgr->getOMHandle(), id_, (void*)this, 1);
  if (fromMigration)
  {
    DEBL((AA "Element %s migrated in\n" AB, idx2str(idx)));
    if (!ignoreArrival)
    {
      lbmgr->Migrated(ldHandle, true);
      // load balancer should ignore this objects movement
      //  AsyncMigrate(true);
    }
  }
#endif
}
CkLocRec::~CkLocRec()
{
  if (deletedMarker != NULL)
    *deletedMarker = true;
#if CMK_LBDB_ON
  stopTiming();
  DEBL((AA "Unregistering element %s from load balancer\n" AB, idx2str(idx)));
  lbmgr->UnregisterObj(ldHandle);
#endif
}
void CkLocRec::migrateMe(int toPe)  // Leaving this processor
{
  // This will pack us up, send us off, and delete us
  //	printf("[%d] migrating migrateMe to %d \n",CkMyPe(),toPe);
  myLocMgr->emigrate(this, toPe);
}

#if CMK_LBDB_ON
void CkLocRec::startTiming(int ignore_running)
{
  if (!ignore_running)
    running = true;
  DEBL((AA "Start timing for %s at %.3fs {\n" AB, idx2str(idx), CkWallTimer()));
  if (enable_measure)
    lbmgr->ObjectStart(ldHandle);
}
void CkLocRec::stopTiming(int ignore_running)
{
  DEBL((AA "} Stop timing for %s at %.3fs\n" AB, idx2str(idx), CkWallTimer()));
  if ((ignore_running || running) && enable_measure)
    lbmgr->ObjectStop(ldHandle);
  if (!ignore_running)
    running = false;
}
void CkLocRec::setObjTime(double cputime) { lbmgr->EstObjLoad(ldHandle, cputime); }
void CkLocRec::setObjPosition(const std::vector<LBRealType>& pos)
{
  lbmgr->SetObjPosition(ldHandle, pos);
}
double CkLocRec::getObjTime()
{
  LBRealType walltime, cputime;
  lbmgr->GetObjLoad(ldHandle, walltime, cputime);
  return walltime;
}
const std::vector<LBRealType>& CkLocRec::getObjPosition()
{
  return lbmgr->GetObjPosition(ldHandle);
}
#  if CMK_LB_USER_DATA
void* CkLocRec::getObjUserData(int idx) { return lbmgr->GetDBObjUserData(ldHandle, idx); }
#  endif
#endif

// Attempt to destroy this record. If the location manager is done with the
// record (because all array elements were destroyed) then it will be deleted.
void CkLocRec::destroy(void) { myLocMgr->reclaim(this); }

/****************************************************************************/

bool CkLocRec::invokeEntry(CkMigratable* obj, void* msg, int epIdx, bool doFree)
{
  DEBS((AA "   Invoking entry %d on element %s\n" AB, epIdx, idx2str(idx)));
  bool isDeleted = false;  // Enables us to detect deletion during processing
  deletedMarker = &isDeleted;

#if CMK_TRACE_ENABLED
  if (msg)
  { /* Tracing: */
    envelope* env = UsrToEnv(msg);
    //	CkPrintf("ckLocation.C beginExecuteDetailed %d %d
    //\n",env->getEvent(),env->getsetArraySrcPe());
    if (_entryTable[epIdx]->traceEnabled)
    {
      CmiObjId projID = idx.getProjectionID();
      _TRACE_BEGIN_EXECUTE_DETAILED(env->getEvent(), ForChareMsg, epIdx, env->getSrcPe(),
                                    env->getTotalsize(), &projID, obj);
      if (_entryTable[epIdx]->appWork)
        _TRACE_BEGIN_APPWORK();
    }
  }
#endif

  if (doFree)
    CkDeliverMessageFree(epIdx, msg, obj);
  else /* !doFree */
    CkDeliverMessageReadonly(epIdx, msg, obj);

#if CMK_TRACE_ENABLED
  if (msg)
  { /* Tracing: */
    if (_entryTable[epIdx]->traceEnabled)
    {
      if (_entryTable[epIdx]->appWork)
        _TRACE_END_APPWORK();
      _TRACE_END_EXECUTE();
    }
  }
#endif
#if CMK_LBDB_ON
  if (!isDeleted)
    checkBufferedMigration();  // check if should migrate
#endif
  if (isDeleted)
    return false;  // We were deleted
  deletedMarker = NULL;
  return true;
}

#if CMK_LBDB_ON

void CkLocRec::staticMetaLBResumeWaitingChares(LDObjHandle h, int lb_ideal_period)
{
  CkLocRec* el = (CkLocRec*)(LBManager::Object()->GetObjUserData(h));
  DEBL((AA "MetaBalancer wants to resume waiting chare %s\n" AB, idx2str(el->idx)));
  el->myLocMgr->informLBPeriod(el, lb_ideal_period);
}

void CkLocRec::staticMetaLBCallLBOnChares(LDObjHandle h)
{
  CkLocRec* el = (CkLocRec*)(LBManager::Object()->GetObjUserData(h));
  DEBL((AA "MetaBalancer wants to call LoadBalance on chare %s\n" AB, idx2str(el->idx)));
  el->myLocMgr->metaLBCallLB(el);
}

void CkLocRec::staticMigrate(LDObjHandle h, int dest)
{
  CkLocRec* el = (CkLocRec*)(LBManager::Object()->GetObjUserData(h));
  DEBL((AA "Load balancer wants to migrate %s to %d\n" AB, idx2str(el->idx), dest));
  el->recvMigrate(dest);
}

void CkLocRec::recvMigrate(int toPe)
{
  // we are in the mode of delaying actual migration
  // till readyMigrate()
  if (readyMigrate)
  {
    migrateMe(toPe);
  }
  else
    nextPe = toPe;
}

void CkLocRec::AsyncMigrate(bool use)
{
  asyncMigrate = use;
  lbmgr->UseAsyncMigrate(ldHandle, use);
}

bool CkLocRec::checkBufferedMigration()
{
  // we don't migrate in user's code when calling ReadyMigrate(true)
  // we postphone the action to here until we exit from the user code.
  if (readyMigrate && nextPe != -1)
  {
    int toPe = nextPe;
    nextPe = -1;
    // don't migrate inside the object call
    migrateMe(toPe);
    // don't do anything
    return true;
  }
  return false;
}

int CkLocRec::MigrateToPe()
{
  int pe = nextPe;
  nextPe = -1;
  return pe;
}

void CkLocRec::setMigratable(int migratable)
{
  if (migratable)
    lbmgr->Migratable(ldHandle);
  else
    lbmgr->NonMigratable(ldHandle);
}

void CkLocRec::setPupSize(size_t obj_pup_size)
{
  lbmgr->setPupSize(ldHandle, obj_pup_size);
}

#endif

// Call ckDestroy for each record, which deletes the record, and ~CkLocRec()
// removes it from the hash table, which would invalidate an iterator.
void CkLocMgr::flushLocalRecs(void)
{
  while (hash.size())
  {
    CkLocRec* rec = hash.begin()->second;
    callMethod(rec, &CkMigratable::ckDestroy);
  }
}

// All records are local records after the 64bit ID update
void CkLocMgr::flushAllRecs(void) { flushLocalRecs(); }

/*************************** LocCache **************************/
const CkLocEntry CkLocEntry::nullEntry = CkLocEntry();
void CkLocCache::pup(PUP::er& p)
{
#if __FAULT__
  if (!p.isUnpacking())
  {
    /**
     * pack the indexes of elements which have their homes on this processor
     * but dont exist on it.. needed for broadcast after a restart
     * indexes of local elements dont need to be packed since they will be
     * recreated later anyway
     */
    std::vector<CkLocEntry> entries;
    for (const auto& itr : locMap)
    {
      if (homePe(itr.first) == CmiMyPe() && itr.second.pe != CmiMyPe())
      {
        entries.push_back(itr.second);
      }
    }

    int count = entries.size();
    p | count;
    for (int i = 0; i < count; i++)
    {
      p | entries[i];
    }
  }
  else
  {
    int count;
    p | count;
    for (int i = 0; i < count; i++)
    {
      CkLocEntry e;
      p | e;
      e.epoch = 0;
      updateLocation(e);
      if (homePe(e.id) != CkMyPe())
      {
        thisProxy[homePe(e.id)].updateLocation(e);
      }
      CkAssert(getPe(e.id) == e.pe);
    }
  }
#endif
}

void CkLocCache::requestLocation(CmiUInt8 id)
{
  int home = homePe(id);
  if (home != CkMyPe())
  {
    thisProxy[home].requestLocation(id, CkMyPe());
  }
}

void CkLocCache::requestLocation(CmiUInt8 id, const int peToTell)
{
  if (peToTell == CkMyPe()) return;

  LocationMap::const_iterator itr = locMap.find(id);
  // TODO: If the location is not found, we probably need to buffer this request. Should
  // only effect very weird corner cases at the moment, and is a problem that already
  // existed, but should be addressed in the upcoming delivery/buffering cleanup.
  if (itr != locMap.end())
  {
    thisProxy[peToTell].updateLocation(itr->second);
  }
}

void CkLocCache::updateLocation(const CkLocEntry& newEntry)
{
  CkAssert(newEntry.pe != -1);
  CkLocEntry& oldEntry = locMap[newEntry.id];
  if (newEntry.epoch > oldEntry.epoch)
  {
    oldEntry = newEntry;
    notifyListeners(newEntry.id, newEntry.pe);
  }
}

void CkLocCache::recordEmigration(CmiUInt8 id, int pe)
{
  LocationMap::iterator itr = locMap.find(id);

  CkAssert(itr != locMap.end());
  CkAssert(itr->second.pe == CkMyPe());

  itr->second.pe = pe;
  itr->second.epoch++;
}


void CkLocCache::insert(CmiUInt8 id, int epoch)
{
  CkLocEntry& e = locMap[id];
  // TODO: This should be > probably, but demand creation needs some fixing up
  CkAssert(epoch >= e.epoch);
  e.id = id;
  e.pe = CkMyPe();
  e.epoch = epoch;
  notifyListeners(e.id, e.pe);
}

/*************************** LocMgr: CREATION *****************************/
CkLocMgr::CkLocMgr(CkArrayOptions opts)
    : bounds(opts.getBounds()),
      idCounter(1),
      thisProxy(thisgroup),
      thislocalproxy(thisgroup, CkMyPe())
{
  DEBC((AA "Creating new location manager %d\n" AB, thisgroup.idx));

  duringMigration = false;

  // Register with the map object
  mapID = opts.getMap();
  map = static_cast<CkArrayMap*>(CkLocalBranch(mapID));
  if (map == nullptr)
    CkAbort("ERROR! Local branch of array map is NULL!");
  // TODO: Should this be registered here, CkArray, CkLocMgr, or none?
  mapHandle = map->registerArray(opts.getEnd(), thisgroup);

  cacheID = opts.getLocationCache();
  cache = static_cast<CkLocCache*>(CkLocalBranch(cacheID));
  if (cache == nullptr)
    CkAbort("ERROR! Local branch of location cache is NULL!\n");

  // Figure out the mapping from indices to object IDs if one is possible
  compressor = ck::FixedArrayIndexCompressor::make(bounds);

  // Find and register with the load balancer
#if CMK_LBDB_ON
  lbmgrID = _lbmgr;
  metalbID = _metalb;
  initLB(lbmgrID, metalbID);
#endif
}

CkLocMgr::CkLocMgr(CkMigrateMessage* m)
    : IrrGroup(m), thisProxy(thisgroup), thislocalproxy(thisgroup, CkMyPe())
{
  duringMigration = false;
}

CkLocMgr::~CkLocMgr()
{
#if CMK_LBDB_ON
  syncBarrier->removeBeginReceiver(lbBarrierBeginReceiver);
  syncBarrier->removeEndReceiver(lbBarrierEndReceiver);
  lbmgr->UnregisterOM(myLBHandle);
#endif
  map->unregisterArray(mapHandle);
}

void CkLocMgr::pup(PUP::er& p)
{
  if (p.isPacking() && pendingImmigrate.size() > 0)
  {
    CkAbort(
        "Attempting to pup location manager with buffered migration messages."
        " Likely cause is checkpointing before array creation has fully completed\n");
  }
  IrrGroup::pup(p);
  p | mapID;
  p | mapHandle;
  p | cacheID;
  p | bounds;
  p | idCounter;
  if (p.isUnpacking())
  {
    thisProxy = thisgroup;
    CProxyElement_CkLocMgr newlocalproxy(thisgroup, CkMyPe());
    thislocalproxy = newlocalproxy;

    // Register with the map object
    map = static_cast<CkArrayMap*>(CkLocalBranch(mapID));
    if (map == nullptr)
      CkAbort("ERROR! Local branch of array map is NULL!");

    // Register with the cache object
    cache = static_cast<CkLocCache*>(CkLocalBranch(cacheID));
    if (cache == nullptr)
      CkAbort("ERROR! Local branch of location cache is NULL!");

    compressor = ck::FixedArrayIndexCompressor::make(bounds);
  }

#if CMK_LBDB_ON
  p | lbmgrID;
  p | metalbID;
  if (p.isUnpacking())
  {
    initLB(lbmgrID, metalbID);
  }
#endif

  // Delay doneInserting when it is unpacking during restart to prevent load balancing
  // from kicking in.
  if (p.isUnpacking() && !CkInRestarting())
  {
    doneInserting();
  }
}

/// Add a new local array manager to our list.
void CkLocMgr::addManager(CkArrayID id, CkArray* mgr)
{
  CK_MAGICNUMBER_CHECK
  DEBC((AA "Adding new array manager\n" AB));
  managers[id] = mgr;
  auto i = pendingImmigrate.begin();
  while (i != pendingImmigrate.end())
  {
    auto msg = *i;
    if (msg->nManagers <= managers.size())
    {
      i = pendingImmigrate.erase(i);
      immigrate(msg);
    }
    else
    {
      i++;
    }
  }
}

void CkLocMgr::deleteManager(CkArrayID id, CkArray* mgr)
{
  CkAssert(managers[id] == mgr);
  managers.erase(id);
  if (managers.empty())
    delete this;
}

// Tell this element's home processor it now lives "there"
void CkLocMgr::informHome(const CkArrayIndex& idx, int nowOnPe)
{
  int home = homePe(idx);
  // TODO: If home == CkMyPe() should we call update locally?
  if (home != CkMyPe() && home != nowOnPe)
  {
    // TODO: This may not need to be an idx update. Pretty sure the home will always
    // know the idx to id mapping.
    CmiUInt8 id = lookupID(idx);
    thisProxy[home].updateLocation(idx, cache->getLocationEntry(id));
  }
}

CkLocRec* CkLocMgr::createLocal(const CkArrayIndex& idx, bool forMigration,
                                bool ignoreArrival, bool notifyHome, int epoch)
{
  DEBC((AA "Adding new record for element %s\n" AB, idx2str(idx)));
  CmiUInt8 id = lookupID(idx);

  CkLocRec* rec = new CkLocRec(this, forMigration, ignoreArrival, idx, id);
  insertRec(rec, id);
  cache->insert(id, epoch);
  updateLocation(idx, cache->getLocationEntry(id));

  if (notifyHome)
  {
    informHome(idx, CkMyPe());
  }
  return rec;
}

// Used to handle messages that were buffered because of active rgets in progress
void CkLocMgr::processAfterActiveRgetsCompleted(CmiUInt8 id)
{
  // Call ckJustMigrated
  CkLocRec* myLocRec = elementNrec(id);
  callMethod(myLocRec, &CkMigratable::ckJustMigrated);

  // Call ResumeFromSync on elements that were waiting for rgets
  auto iter2 = toBeResumeFromSynced.find(id);
  if (iter2 != toBeResumeFromSynced.end())
  {
    iter2->second->ResumeFromSync();
    toBeResumeFromSynced.erase(iter2);
  }

  // Deliver buffered messages to the elements that were waiting on rgets
  auto iter = bufferedActiveRgetMsgs.find(id);
  if (iter != bufferedActiveRgetMsgs.end())
  {
    std::vector<CkArrayMessage*> bufferedMsgs = iter->second;
    bufferedActiveRgetMsgs.erase(iter);
    for (auto msg : bufferedMsgs)
    {
      CmiHandleMessage(UsrToEnv(msg));
    }
  }
}

CmiUInt8 CkLocMgr::getNewObjectID(const CkArrayIndex& idx)
{
  CmiUInt8 id;
  if (!lookupID(idx, id))
  {
    id = idCounter++ + ((CmiUInt8)CkMyPe() << CMK_OBJID_ELEMENT_BITS);
    insertID(idx, id);
  }
  return id;
}

CkLocRec* CkLocMgr::registerNewElement(const CkArrayIndex& idx)
{
  CmiUInt8 id = getNewObjectID(idx);
  CkLocRec* rec = elementNrec(id);
  if (rec == nullptr)
  {
    // TODO: This is going to end up needlessly calling getNewObjectID(...) again
    rec = createLocal(idx, false, false, true);
  }

  return rec;
}

bool CkLocMgr::addElementToRec(CkLocRec* rec, CkArray* mgr, CkMigratable* elt,
                               int ctorIdx, void* ctorMsg)
{
  // Insert the new element into its manager's local list
  CmiUInt8 id = lookupID(rec->getIndex());
  if (mgr->getEltFromArrMgr(id))
  {
    CkAbort("Cannot insert array element twice!");
  }
  mgr->putEltInArrMgr(id, elt);  // Local element table

  // Call the element's constructor
  DEBC((AA "Constructing element %s of array\n" AB, idx2str(rec->getIndex())));
  CkMigratable_initInfo& i = CkpvAccess(mig_initInfo);
  i.locRec = rec;
  i.chareType = _entryTable[ctorIdx]->chareIdx;

#ifndef CMK_CHARE_USE_PTR
  int callingChareIdx = CkpvAccess(currentChareIdx);
  CkpvAccess(currentChareIdx) = -1;
#endif

  if (!rec->invokeEntry(elt, ctorMsg, ctorIdx, true))
    return false;

#ifndef CMK_CHARE_USE_PTR
  CkpvAccess(currentChareIdx) = callingChareIdx;
#endif

#if CMK_OUT_OF_CORE
  /* Register new element with out-of-core */
  PUP::sizer p_getSize;
  elt->virtual_pup(p_getSize);
  elt->prefetchObjID =
      CooRegisterObject(&CkArrayElementPrefetcher, p_getSize.size(), elt);
#endif

  return true;
}

// TODO: This might be not needed
void CkLocMgr::requestLocation(CmiUInt8 id)
{
  cache->requestLocation(id);
}

void CkLocMgr::requestLocation(const CkArrayIndex& idx)
{
  CmiUInt8 id;
  if (lookupID(idx, id))
  {
    requestLocation(id);
  }
  else
  {
    int home = homePe(idx);
    if (home != CkMyPe())
    {
      thisProxy[home].requestLocation(idx, CkMyPe());
    }
  }
}

bool CkLocMgr::requestLocation(const CkArrayIndex& idx, const int peToTell)
{
  CkAssert(peToTell != CkMyPe());

  CmiUInt8 id;
  if (lookupID(idx, id))
  {
    // We found the ID so update the location for peToTell
    thisProxy[peToTell].updateLocation(idx, cache->getLocationEntry(id));
    return true;
  }
  else
  {
    // We don't know the ID so buffer the location request
    DEBN(("%d Buffering ID/location req for %s\n", CkMyPe(), idx2str(idx)));
    bufferedLocationRequests[idx].push_back(peToTell);
    return false;
  }
}

void CkLocMgr::updateLocation(const CkArrayIndex& idx, const CkLocEntry& e)
{
  CkAssert(e.pe != -1);
  // Set the mapping from idx to id
  insertID(idx, e.id);

  // Update the location information
  cache->updateLocation(e);

  // Any location requests that we had to buffer because we didn't know how the index
  // mapped to the id can now be replied to.
  auto itr = bufferedLocationRequests.find(idx);
  if (itr != bufferedLocationRequests.end())
  {
    for (int pe : itr->second)
    {
      DEBN(("%d Replying to buffered ID/location req to pe %d\n", CkMyPe(), pe));
      if (pe != CkMyPe())
        thisProxy[pe].updateLocation(idx, e);
    }
    bufferedLocationRequests.erase(itr);
  }

  notifyListeners(idx, e.id, e.pe);
}

/*************************** LocMgr: DELETION *****************************/
// This index may no longer be used -- check if any of our managers are still
// using it, and if not delete it and clean up all traces of it on other PEs.
void CkLocMgr::reclaim(CkLocRec* rec)
{
  CK_MAGICNUMBER_CHECK
  // Return early if the record is still in use by any of our arrays
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    if (itr->second->lookup(rec->getID()))
      return;
  }
  removeFromTable(rec->getID());

  DEBC((AA "Destroying record for element %s\n" AB, idx2str(rec->getIndex())));
  if (!duringMigration)
  {  // This is a local element dying a natural death
    int home = homePe(rec->getIndex());
    if (home != CkMyPe())
#if CMK_MEM_CHECKPOINT
      if (!CkInRestarting())  // all array elements are removed anyway
#endif
        if (!duringDestruction)
          thisProxy[home].reclaimRemote(rec->getIndex(), CkMyPe());
  }
  delete rec;
}

// TODO: Rename to something more basic. This is just clearing entries from the
// cache. Will be called by reclaim remote, but maybe other things as well.
void CkLocMgr::reclaimRemote(const CkArrayIndex& idx, int deletedOnPe)
{
  DEBC((AA "Our element %s died on PE %d\n" AB, idx2str(idx), deletedOnPe));

  CmiUInt8 id;
  if (!lookupID(idx, id))
    CkAbort("Cannot find ID for the given index\n");

  // Delete the ID from location caching. Do not delete the idx to id mapping because
  // that remains constant and we don't want messages to end up stranded because they
  // can't figure out their index.
  cache->erase(id);
}

void CkLocMgr::removeFromTable(const CmiUInt8 id)
{
#if CMK_ERROR_CHECKING
  // Make sure it's actually in the table before we delete it
  if (NULL == elementNrec(id))
    CkAbort("CkLocMgr::removeFromTable called on invalid index!");
#endif
  hash.erase(id);
  // Don't erase this during migration because the entry will be updated to reflect
  // the new location by calling recordEmigration
  if (!duringMigration)
    cache->erase(id);
#if CMK_ERROR_CHECKING
  // Make sure it's really gone
  if (NULL != elementNrec(id))
    CkAbort("CkLocMgr::removeFromTable called, but element still there!");
#endif
}

// This message took several hops to reach us-- fix it
void CkLocMgr::multiHop(CkArrayMessage* msg)
{
  CK_MAGICNUMBER_CHECK
  int srcPe = msg->array_getSrcPe();
  if (srcPe == CkMyPe())
    DEB((AA "Odd routing: local element %" PRIu64 " is %d hops away!\n" AB,
         msg->array_element_id(), msg->array_hops()));
  else
  {  // Send a routing message letting original sender know new element location
    DEBS((AA "Sending update back to %d for element %" PRIu64 "\n" AB, srcPe,
          msg->array_element_id()));
    cache->requestLocation(msg->array_element_id(), srcPe);
  }
}

bool CkLocMgr::checkInBounds(const CkArrayIndex& idx) const
{
  if (bounds.nInts > 0)
  {
    CkAssert(idx.dimension == bounds.dimension);
    bool shorts = idx.dimension > 3;

    for (int i = 0; i < idx.dimension; ++i)
    {
      unsigned int thisDim = shorts ? idx.indexShorts[i] : idx.index[i];
      unsigned int thatDim = shorts ? bounds.indexShorts[i] : bounds.index[i];
      if (thisDim >= thatDim) return false;
    }
  }
  return true;
}

/************************** LocMgr: ITERATOR *************************/
CkLocation::CkLocation(CkLocMgr* mgr_, CkLocRec* rec_) : mgr(mgr_), rec(rec_) {}

const CkArrayIndex& CkLocation::getIndex(void) const { return rec->getIndex(); }

CmiUInt8 CkLocation::getID() const { return rec->getID(); }

void CkLocation::destroyAll() { mgr->callMethod(rec, &CkMigratable::ckDestroy); }

void CkLocation::pup(PUP::er& p)
{
  mgr->pupElementsFor(p, rec, CkElementCreation_migrate);
}

CkLocIterator::~CkLocIterator() {}

/// Iterate over our local elements:
void CkLocMgr::iterate(CkLocIterator& dest)
{
  // Poke through the hash table for local ArrayRecs.
  for (LocRecHash::iterator it = hash.begin(); it != hash.end(); it++)
  {
    CkLocation loc(this, it->second);
    dest.addLocation(loc);
  }
}

/************************** LocMgr: MIGRATION *************************/
void CkLocMgr::pupElementsFor(PUP::er& p, CkLocRec* rec, CkElementCreation_t type,
                              bool rebuild)
{
  p.comment("-------- Array Location --------");

  // First pup the element types
  // (A separate loop so ckLocal works even in element pup routines)
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    int elCType;
    CkArray* arr = itr->second;
    if (!p.isUnpacking())
    {  // Need to find the element's existing type
      CkMigratable* elt = arr->getEltFromArrMgr(rec->getID());
      if (elt)
        elCType = elt->ckGetChareType();
      else
        elCType = -1;  // Element hasn't been created
    }
    p(elCType);
    if (p.isUnpacking() && elCType != -1)
    {
      // Create the element
      CkMigratable* elt = arr->allocateMigrated(elCType, type);
      int migCtorIdx = _chareTable[elCType]->getMigCtor();
      // Insert into our tables and call migration constructor
      if (!addElementToRec(rec, arr, elt, migCtorIdx, NULL))
        return;
      if (type == CkElementCreation_resume)
      {  // HACK: Re-stamp elements on checkpoint resume--
        //  this restores, e.g., reduction manager's gcount
        arr->stampListenerData(elt);
      }
    }
  }
  // Next pup the element data
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    CkMigratable* elt = itr->second->getEltFromArrMgr(rec->getID());
    if (elt != NULL)
    {
      elt->virtual_pup(p);
#if CMK_ERROR_CHECKING
      if (p.isUnpacking())
        elt->sanitycheck();
#endif
    }
  }
#if CMK_MEM_CHECKPOINT
  if (rebuild)
  {
    ArrayElement* elt;
    std::vector<CkMigratable*> list;
    migratableList(rec, list);
    CmiAssert(!list.empty());
    for (int l = 0; l < list.size(); l++)
    {
      //    reset, may not needed now
      // for now.
      for (int i = 0; i < CK_ARRAYLISTENER_MAXLEN; i++)
      {
        ArrayElement* elt = (ArrayElement*)list[l];
        contributorInfo* c = (contributorInfo*)&elt->listenerData[i];
        if (c)
          c->redNo = 0;
      }
    }
  }
#endif
}

/// Call this member function on each element of this location:
void CkLocMgr::callMethod(CkLocRec* rec, CkMigratable_voidfn_t fn)
{
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    CkMigratable* el = itr->second->getEltFromArrMgr(rec->getID());
    if (el)
      (el->*fn)();
  }
}

/// Call this member function on each element of this location:
void CkLocMgr::callMethod(CkLocRec* rec, CkMigratable_voidfn_arg_t fn, void* data)
{
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    CkMigratable* el = itr->second->getEltFromArrMgr(rec->getID());
    if (el)
      (el->*fn)(data);
  }
}

/// return a list of migratables in this local record
void CkLocMgr::migratableList(CkLocRec* rec, std::vector<CkMigratable*>& list)
{
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    CkMigratable* elt = itr->second->getEltFromArrMgr(rec->getID());
    if (elt)
      list.push_back(elt);
  }
}

/// Migrate this local element away to another processor.
void CkLocMgr::emigrate(CkLocRec* rec, int toPe)
{
  CK_MAGICNUMBER_CHECK
  if (toPe == CkMyPe())
    return;  // You're already there!

  CkArrayIndex idx = rec->getIndex();
  CmiUInt8 id = rec->getID();

#if CMK_OUT_OF_CORE
  /* Load in any elements that are out-of-core */
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    CkMigratable* el = itr->second->getEltFromArrMgr(rec->getID());
    if (el)
      if (!el->isInCore)
        CooBringIn(el->prefetchObjID);
  }
#endif

  // Let all the elements know we're leaving
  callMethod(rec, &CkMigratable::ckAboutToMigrate);

  // First pass: find size of migration message
  size_t bufSize;
  {
    PUP::sizer p(PUP::er::IS_MIGRATION);
    pupElementsFor(p, rec, CkElementCreation_migrate);
    bufSize = p.size();
  }
#if CMK_ERROR_CHECKING
  if (bufSize > std::numeric_limits<int>::max())
  {
    CkAbort("Cannot migrate an object with size greater than %d bytes!\n",
            std::numeric_limits<int>::max());
  }
#endif

  // Allocate and pack into message
  CkArrayElementMigrateMessage* msg =
      new (bufSize, 0) CkArrayElementMigrateMessage(idx, id,
#if CMK_LBDB_ON
                                                    rec->isAsyncMigrate(),
#else
                                                    false,
#endif
                                                    bufSize, managers.size(),
                                                    cache->getEpoch(id) + 1);

  {
    PUP::toMem p(msg->packData, PUP::er::IS_MIGRATION);
    p.becomeDeleting();
    pupElementsFor(p, rec, CkElementCreation_migrate);
    if (p.size() != bufSize)
    {
      CkError(
          "ERROR! Array element claimed it was %zu bytes to a "
          "sizing PUP::er, but copied %zu bytes into the packing PUP::er!\n",
          bufSize, p.size());
      CkAbort("Array element's pup routine has a direction mismatch.\n");
    }
  }

  DEBM((AA "Migrated index size %s to %d \n" AB, idx2str(idx), toPe));

  thisProxy[toPe].immigrate(msg);

  duringMigration = true;
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    itr->second->deleteElt(id);
  }
  duringMigration = false;

  cache->recordEmigration(id, toPe);
  informHome(idx, toPe);

#if !CMK_LBDB_ON && CMK_GLOBAL_LOCATION_UPDATE
  DEBM((AA "Global location update. idx %s "
           "assigned to %d \n" AB,
        idx2str(idx), toPe));
  thisProxy.updateLocation(id, toPe);
#endif

  CK_MAGICNUMBER_CHECK
}

#if CMK_LBDB_ON
void CkLocMgr::informLBPeriod(CkLocRec* rec, int lb_ideal_period)
{
  callMethod(rec, &CkMigratable::recvLBPeriod, (void*)&lb_ideal_period);
}

void CkLocMgr::metaLBCallLB(CkLocRec* rec)
{
  callMethod(rec, &CkMigratable::metaLBCallLB);
}
#endif

/**
  Migrating array element is arriving on this processor.
*/
void CkLocMgr::immigrate(CkArrayElementMigrateMessage* msg)
{
  const CkArrayIndex& idx = msg->idx;

  PUP::fromMem p(msg->packData, PUP::er::IS_MIGRATION);

  if (msg->nManagers < managers.size())
    CkAbort("Array element arrived from location with fewer managers!\n");
  if (msg->nManagers > managers.size())
  {
    // Some array managers haven't registered yet -- buffer the message
    DEBM((AA "Buffering %s immigrate msg waiting for array registration\n" AB,
          idx2str(idx)));
    pendingImmigrate.push_back(msg);
    return;
  }

  insertID(idx, msg->id);

  // Create a record for this element
  CkLocRec* rec =
      createLocal(idx, true, msg->ignoreArrival, false /* home told on departure */, msg->epoch);

  CmiAssert(CpvAccess(newZCPupGets).empty());  // Ensure that vector is empty
  // Create the new elements as we unpack the message
  pupElementsFor(p, rec, CkElementCreation_migrate);
  bool zcRgetsActive = !CpvAccess(newZCPupGets).empty();
  if (zcRgetsActive)
  {
    // newZCPupGets is not empty, rgets need to be launched
    // newZCPupGets is populated with NcpyOperationInfo during pupElementsFor by
    // pup_buffer calls that require Rgets Issue Rgets using the populated newZCPupGets
    // vector
    zcPupIssueRgets(msg->id, this);
  }
  CpvAccess(newZCPupGets).clear();  // Clear this to reuse the vector
  if (p.size() != msg->length)
  {
    CkError(
        "ERROR! Array element claimed it was %d bytes to a"
        "packing PUP::er, but %zu bytes in the unpacking PUP::er!\n",
        msg->length, p.size());
    CkError("(I have %zu managers; it claims %d managers)\n", managers.size(),
            msg->nManagers);

    CkAbort("Array element's pup routine has a direction mismatch.\n");
  }

  if (!zcRgetsActive)
  {
    // Let all the elements know we've arrived
    callMethod(rec, &CkMigratable::ckJustMigrated);
  }

  delete msg;
}

void CkLocMgr::restore(const CkArrayIndex& idx, CmiUInt8 id, PUP::er& p)
{
  insertID(idx, id);

  CkLocRec* rec = createLocal(idx, false, false, false);

  // Create the new elements as we unpack the message
  pupElementsFor(p, rec, CkElementCreation_restore);

  callMethod(rec, &CkMigratable::ckJustRestored);
}

/// Insert and unpack this array element from this checkpoint (e.g., from CkLocation::pup)
void CkLocMgr::resume(const CkArrayIndex& idx, CmiUInt8 id, PUP::er& p, bool notify,
                      bool rebuild)
{
  insertID(idx, id);

  CkLocRec* rec = createLocal(idx, false, false, notify /* home doesn't know yet */);

  // Create the new elements as we unpack the message
  pupElementsFor(p, rec, CkElementCreation_resume, rebuild);

  callMethod(rec, &CkMigratable::ckJustMigrated);
}

/********************* LocMgr: UTILITY ****************/
void CkMagicNumber_impl::badMagicNumber(int expected, const char* file, int line,
                                        void* obj) const
{
  CkError(
      "FAILURE on pe %d, %s:%d> Expected %p's magic number "
      "to be 0x%08x; but found 0x%08x!\n",
      CkMyPe(), file, line, obj, expected, magic);
  CkAbort(
      "Bad magic number detected!  This implies either\n"
      "the heap or a message was corrupted!\n");
}
CkMagicNumber_impl::CkMagicNumber_impl(int m) : magic(m) {}

/// Return true if this array element lives on another processor
bool CkLocMgr::isRemote(const CkArrayIndex& idx, int* onPe) const
{
  int pe = whichPe(idx);
  /* not definitely a remote element */
  if (pe == -1 || pe == CkMyPe())
    return false;
  // element is indeed remote
  *onPe = pe;
  return true;
}

static const char* rec2str[] = {
    "base (INVALID)",  // Base class (invalid type)
    "local",           // Array element that lives on this Pe
};

// If we are deleting our last array manager set duringDestruction to true to
// avoid sending out unneeded reclaimRemote messages.
void CkLocMgr::setDuringDestruction(bool _duringDestruction)
{
  duringDestruction = (_duringDestruction && managers.size() == 1);
}

// Add given element array record at idx, replacing the existing record
void CkLocMgr::insertRec(CkLocRec* rec, const CmiUInt8& id)
{
  CkLocRec* old_rec = elementNrec(id);
  hash[id] = rec;
  delete old_rec;
}

// Call this on an unrecognized array index
static void abort_out_of_bounds(const CkArrayIndex& idx)
{
  CkPrintf("ERROR! Unknown array index: %s\n", idx2str(idx));
  CkAbort("Array index out of bounds\n");
}

// Look up array element in hash table.  Return NULL if not there.
CkLocRec* CkLocMgr::elementNrec(const CmiUInt8 id) const
{
  LocRecHash::const_iterator it = hash.find(id);
  return it == hash.end() ? NULL : it->second;
}

struct LocalElementCounter : public CkLocIterator
{
  unsigned int count;
  LocalElementCounter() : count(0) {}
  void addLocation(CkLocation& loc) { ++count; }
};

unsigned int CkLocMgr::numLocalElements()
{
  LocalElementCounter c;
  iterate(c);
  return c.count;
}

/********************* LocMgr: LOAD BALANCE ****************/

#if !CMK_LBDB_ON
// Empty versions of all load balancer calls
void CkLocMgr::startInserting(void) {}
void CkLocMgr::doneInserting(void) {}
#endif

#if CMK_LBDB_ON
void CkLocMgr::initLB(CkGroupID lbmgrID_, CkGroupID metalbID_)
{  // Find and register with the load balancer
  lbmgr = (LBManager*)CkLocalBranch(lbmgrID_);
  if (lbmgr == nullptr)
    CkAbort("LBManager not yet created?\n");
  DEBL((AA "Connected to load balancer %p\n" AB, lbmgr));
  if (_lb_args.metaLbOn())
  {
    the_metalb = (MetaBalancer*)CkLocalBranch(metalbID_);
    if (the_metalb == 0)
      CkAbort("MetaBalancer not yet created?\n");
  }
  syncBarrier = CkSyncBarrier::object();
  if (syncBarrier == nullptr)
    CkAbort("CkSyncBarrier not yet created?\n");
  // Register myself as an object manager
  LDOMid myId;
  myId.id = thisgroup;
  LDCallbacks myCallbacks;
  myCallbacks.migrate = (LDMigrateFn)CkLocRec::staticMigrate;
  myCallbacks.setStats = NULL;
  myCallbacks.queryEstLoad = NULL;
  myCallbacks.metaLBResumeWaitingChares =
      (LDMetaLBResumeWaitingCharesFn)CkLocRec::staticMetaLBResumeWaitingChares;
  myCallbacks.metaLBCallLBOnChares =
      (LDMetaLBCallLBOnCharesFn)CkLocRec::staticMetaLBCallLBOnChares;
  myLBHandle = lbmgr->RegisterOM(myId, this, myCallbacks);

  // Tell the lbdb that I'm registering objects
  lbmgr->RegisteringObjects(myLBHandle);

  // Set up callbacks for this LocMgr to call Registering/DoneRegistering during
  // each AtSync.
  lbBarrierBeginReceiver = syncBarrier->addBeginReceiver([=]() {
    DEBL((AA "CkLocMgr AtSync Receiver called\n" AB));
    lbmgr->RegisteringObjects(myLBHandle);
  });
  lbBarrierEndReceiver =
      syncBarrier->addEndReceiver([=]() { lbmgr->DoneRegisteringObjects(myLBHandle); });
}

void CkLocMgr::startInserting(void) { lbmgr->RegisteringObjects(myLBHandle); }
void CkLocMgr::doneInserting(void) { lbmgr->DoneRegisteringObjects(myLBHandle); }
#endif

#include "CkLocation.def.h"
