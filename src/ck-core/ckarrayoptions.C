#include "charm++.h"
#include "ck.h"
#include "ckarrayoptions.h"

CkArrayOptions::CkArrayOptions(void)  // Default: empty array
    : start(),
      end(),
      step(),
      numInitial(),
      bounds(),
      map(_defaultArrayMapID) {
  init();
}

CkArrayOptions::CkArrayOptions(int ndims, int dims[]) // With initial elements (nD)
    : start(CkArrayIndex(ndims, 0)),
      end(CkArrayIndex(ndims, dims)),
      step(CkArrayIndex(ndims, 1)),
      numInitial(end),
      bounds(end),
      map(_defaultArrayMapID) {
  init();
}

CkArrayOptions::CkArrayOptions(int ni1)  // With initial elements (1D)
    : start(CkArrayIndex1D(0)),
      end(CkArrayIndex1D(ni1)),
      step(CkArrayIndex1D(1)),
      numInitial(end),
      bounds(end),
      map(_defaultArrayMapID) {
  init();
}

CkArrayOptions::CkArrayOptions(int ni1, int ni2)  // With initial elements (2D)
    : start(CkArrayIndex2D(0, 0)),
      end(CkArrayIndex2D(ni1, ni2)),
      step(CkArrayIndex2D(1, 1)),
      numInitial(end),
      bounds(end),
      map(_defaultArrayMapID) {
  init();
}

CkArrayOptions::CkArrayOptions(int ni1, int ni2, int ni3)  // With initial elements (3D)
    : start(CkArrayIndex3D(0, 0, 0)),
      end(CkArrayIndex3D(ni1, ni2, ni3)),
      step(CkArrayIndex3D(1, 1, 1)),
      numInitial(end),
      bounds(end),
      map(_defaultArrayMapID) {
  init();
}

CkArrayOptions::CkArrayOptions(short int ni1, short int ni2, short int ni3,
                               short int ni4)  // With initial elements (4D)
    : start(CkArrayIndex4D(0, 0, 0, 0)),
      end(CkArrayIndex4D(ni1, ni2, ni3, ni4)),
      step(CkArrayIndex4D(1, 1, 1, 1)),
      numInitial(end),
      bounds(end),
      map(_defaultArrayMapID) {
  init();
}

CkArrayOptions::CkArrayOptions(short int ni1, short int ni2, short int ni3, short int ni4,
                               short int ni5)  // With initial elements (5D)
    : start(CkArrayIndex5D(0, 0, 0, 0, 0)),
      end(CkArrayIndex5D(ni1, ni2, ni3, ni4, ni5)),
      step(CkArrayIndex5D(1, 1, 1, 1, 1)),
      numInitial(end),
      bounds(end),
      map(_defaultArrayMapID) {
  init();
}

CkArrayOptions::CkArrayOptions(short int ni1, short int ni2, short int ni3, short int ni4,
                               short int ni5,
                               short int ni6)  // With initial elements (6D)
    : start(CkArrayIndex6D(0, 0, 0, 0, 0, 0)),
      end(CkArrayIndex6D(ni1, ni2, ni3, ni4, ni5, ni6)),
      step(CkArrayIndex6D(1, 1, 1, 1, 1, 1)),
      numInitial(end),
      bounds(end),
      map(_defaultArrayMapID) {
  init();
}

CkArrayOptions::CkArrayOptions(CkArrayIndex s, CkArrayIndex e, CkArrayIndex step)
    : start(s),
      end(e),
      step(step),
      numInitial(end),
      bounds(end),
      map(_defaultArrayMapID) {
  init();
}

void CkArrayOptions::init() {
  locMgr.setZero();
  mCastMgr.setZero();
  anytimeMigration = _isAnytimeMigration;
  insertionType = InsertionType::UNSET;
  reductionClient.type = CkCallback::invalid;
  disableNotifyChildInRed = !_isNotifyChildInRed;
  broadcastViaScheduler = false;
  sectionAutoDelegate = true;
}

CkArrayOptions& CkArrayOptions::setStaticInsertion(bool b) {
  insertionType = b ? InsertionType::STATIC : InsertionType::DYNAMIC;
  if (b && map == _defaultArrayMapID) map = _fastArrayMapID;
  return *this;
}

/// Bind our elements to this array
CkArrayOptions& CkArrayOptions::bindTo(const CkArrayID& b) {
  CkLocMgr* mgr = CProxy_CkArray(b).ckLocalBranch()->getLocMgr();
  // Stupid bug: need a way for arrays to stay the same size *FOREVER*,
  // not just initially.
  // setNumInitial(arr->getNumInitial());
  setMap(mgr->getMap());
  setLocationCache(mgr->getLocationCache());
  return setLocationManager(mgr->getGroupID());
}

CkArrayOptions& CkArrayOptions::addListener(CkArrayListener* listener) {
  arrayListeners.push_back(listener);
  return *this;
}

void CkArrayOptions::updateIndices() {
  bool shorts = numInitial.dimension > 3;
  start = step = end = numInitial;

  for (int d = 0; d < numInitial.dimension; d++) {
    if (shorts) {
      ((short*)start.data())[d] = 0;
      ((short*)step.data())[d] = 1;
    } else {
      start.data()[d] = 0;
      step.data()[d] = 1;
    }
  }
}

void CkArrayOptions::updateNumInitial() {
  if (end.dimension != start.dimension || end.dimension != step.dimension) {
    return;
  }

  bool shorts = end.dimension > 3;
  numInitial = end;
  for (int d = 0; d < end.dimension; d++) {
    int diff, increment, num;

    // Extract the current dimension of the indices
    if (shorts) {
      diff = ((short*)end.data())[d] - ((short*)start.data())[d];
      increment = ((short*)step.data())[d];
    } else {
      diff = end.data()[d] - start.data()[d];
      increment = step.data()[d];
    }

    // Compute the number of initial elements in this dimension
    num = diff / increment;
    if (diff < 0) {
      num = 0;
    } else if (diff % increment > 0) {
      num++;
    }

    // Set the current dimension of numInitial
    if (shorts) {
      ((short*)numInitial.data())[d] = (short)num;
    } else {
      numInitial.data()[d] = num;
    }
  }
}

void CkArrayOptions::pup(PUP::er& p) {
  p | start;
  p | end;
  p | step;
  p | numInitial;
  p | bounds;
  p | map;
  p | locMgr;
  p | mCastMgr;
  p | locCache;
  p | arrayListeners;
  p | reductionClient;
  p | initCallback;
  p | anytimeMigration;
  p | disableNotifyChildInRed;
  p | insertionType;
  p | broadcastViaScheduler;
  p | sectionAutoDelegate;
}

CkArrayListener::CkArrayListener(int nInts_) : nInts(nInts_) { dataOffset = -1; }
CkArrayListener::CkArrayListener(CkMigrateMessage* m) {
  nInts = -1;
  dataOffset = -1;
}
void CkArrayListener::pup(PUP::er& p) {
  p | nInts;
  p | dataOffset;
}

void CkArrayListener::ckRegister(CkArray* arrMgr, int dataOffset_) {
  if (dataOffset != -1) CkAbort("Cannot register an ArrayListener twice!\n");
  dataOffset = dataOffset_;
}
