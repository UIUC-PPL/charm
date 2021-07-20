#ifndef __CKARRAYOPTIONS_H
#define __CKARRAYOPTIONS_H

#include "ckarrayindex.h"
#include "ckmulticast.h"

/********************* CkArrayListener ****************/
/// An arrayListener is an object that gets informed whenever
/// an array element is created, migrated, or destroyed.
/// This abstract superclass just ignores everything sent to it.
class ArrayElement;
class CkArrayListener : public PUP::able {
  int nInts;       // Number of ints of data to store per element
  int dataOffset;  // Int offset of our data within the element
 public:
  CkArrayListener(int nInts_);
  CkArrayListener(CkMigrateMessage* m);
  virtual void pup(PUP::er& p);
  PUPable_abstract(CkArrayListener)

      /// Register this array type.  Our data is stored in the element at dataOffset
      virtual void ckRegister(CkArray* arrMgr, int dataOffset_);

  /// Return the number of ints of data to store per element
  inline int ckGetLen(void) const { return nInts; }
  /// Return the offset of our data into the element
  inline int ckGetOffset(void) const { return dataOffset; }
  /// Return our data associated with this array element
  inline int* ckGetData(ArrayElement* el) const;

  /// Elements may be being created
  virtual void ckBeginInserting(void) {}
  /// No more elements will be created (for now)
  virtual void ckEndInserting(void) {}

  // The stamp/creating/created/died sequence happens, in order, exactly
  // once per array element.  Migrations don't show up here.
  /// Element creation message is about to be sent
  virtual void ckElementStamp(int* eltInfo) { (void)eltInfo; }
  /// Element is about to be created on this processor
  virtual void ckElementCreating(ArrayElement* elt) { (void)elt; }
  /// Element was just created on this processor
  /// Return false if the element migrated away or deleted itself.
  virtual bool ckElementCreated(ArrayElement* elt) {
    (void)elt;
    return true;
  }

  /// Element is about to be destroyed
  virtual void ckElementDied(ArrayElement* elt) { (void)elt; }

  // The leaving/arriving seqeunce happens once per migration.
  /// Element is about to leave this processor (so about to call pup)
  virtual void ckElementLeaving(ArrayElement* elt) { (void)elt; }

  /// Element just arrived on this processor (so just called pup)
  /// Return false if the element migrated away or deleted itself.
  virtual bool ckElementArriving(ArrayElement* elt) {
    (void)elt;
    return true;
  }

  /// used by checkpointing to reset the states
  virtual void flushState() {}
};

/*********************** CkArrayOptions *******************************/
/// Arguments for array creation:
class CkArrayOptions {
  friend class CkArray;

  CkArrayIndex start, end, step;
  CkArrayIndex numInitial;  ///< Number of elements to create
  /// Limits of element counts in each dimension of this and all bound arrays
  CkArrayIndex bounds;
  CkGroupID map;       ///< Array location map object
  CkGroupID locMgr;    ///< Location manager to bind to
  CkGroupID mCastMgr;  /// <ckmulticast mgr to bind to, for sections
  CkGroupID locCache;
  CkPupAblePtrVec<CkArrayListener> arrayListeners;  // CkArrayListeners for this array
  CkCallback reductionClient;                       // Default target of reductions
  CkCallback initCallback; // Callback to be invoked after chare array creation is complete
  bool anytimeMigration;                            // Elements are allowed to move freely
  bool disableNotifyChildInRed;  // Child elements are not notified when reduction starts
  bool broadcastViaScheduler;    // broadcast inline or through scheduler
  bool sectionAutoDelegate;      // Create a mCastMgr and auto-delegate all sections

  enum class InsertionType : char {
    UNSET,
    DYNAMIC,
    STATIC
  };
  InsertionType insertionType; // -1 = unset, 0 = false, 1 = true

  /// Set various safe defaults for all the constructors
  void init();

  /// Helper functions to keep numInitial and start/step/end consistent
  void updateIndices();
  void updateNumInitial();

 public:
  // Used by external world:
  CkArrayOptions(void);                         ///< Default: empty array
  CkArrayOptions(int ndims, int dims[]);        ///< With initial elements nD
  CkArrayOptions(int ni1_);                     ///< With initial elements 1D
  CkArrayOptions(int ni1_, int ni2_);           ///< With initial elements 2D
  CkArrayOptions(int ni1_, int ni2_, int ni3);  ///< With initial elements 3D
  CkArrayOptions(short ni1_, short ni2_, short ni3,
                 short ni4_);  ///< With initial elements 4D
  CkArrayOptions(short ni1_, short ni2_, short ni3, short ni4_,
                 short ni5_);  ///< With initial elements 5D
  CkArrayOptions(short ni1_, short ni2_, short ni3, short ni4_, short ni5_,
                 short ni6_);  ///< With initial elements 6D
  CkArrayOptions(CkArrayIndex s, CkArrayIndex e,
                 CkArrayIndex step);  ///< Initialize the start, end, and step

  /**
   * These functions return "this" so you can string them together, e.g.:
   *   foo(CkArrayOptions().setMap(mid).bindTo(aid));
   */

  /// Set the start, end, and step for the initial elements to populate
  CkArrayOptions& setStart(CkArrayIndex s) {
    start = s;
    updateNumInitial();
    return *this;
  }
  CkArrayOptions& setEnd(CkArrayIndex e) {
    end = e;
    updateNumInitial();
    return *this;
  }
  CkArrayOptions& setStep(CkArrayIndex s) {
    step = s;
    updateNumInitial();
    return *this;
  }

  /// Create this many initial elements 1D
  CkArrayOptions& setNumInitial(int ni) {
    numInitial = CkArrayIndex1D(ni);
    updateIndices();
    return *this;
  }
  /// Create this many initial elements 2D
  CkArrayOptions& setNumInitial(int ni1, int ni2) {
    numInitial = CkArrayIndex2D(ni1, ni2);
    updateIndices();
    return *this;
  }
  /// Create this many initial elements 3D
  CkArrayOptions& setNumInitial(int ni1, int ni2, int ni3) {
    numInitial = CkArrayIndex3D(ni1, ni2, ni3);
    updateIndices();
    return *this;
  }
  /// Create this many initial elements 4D
  CkArrayOptions& setNumInitial(short ni1, short ni2, short ni3, short ni4) {
    numInitial = CkArrayIndex4D(ni1, ni2, ni3, ni4);
    updateIndices();
    return *this;
  }
  /// Create this many initial elements 5D
  CkArrayOptions& setNumInitial(short ni1, short ni2, short ni3, short ni4, short ni5) {
    numInitial = CkArrayIndex5D(ni1, ni2, ni3, ni4, ni5);
    updateIndices();
    return *this;
  }
  /// Create this many initial elements 6D
  CkArrayOptions& setNumInitial(short ni1, short ni2, short ni3, short ni4, short ni5,
                                short ni6) {
    numInitial = CkArrayIndex6D(ni1, ni2, ni3, ni4, ni5, ni6);
    updateIndices();
    return *this;
  }

  /// Allow up to this many elements in 1D
  CkArrayOptions& setBounds(int ni) {
    bounds = CkArrayIndex1D(ni);
    return *this;
  }
  /// Allow up to this many elements in 2D
  CkArrayOptions& setBounds(int ni1, int ni2) {
    bounds = CkArrayIndex2D(ni1, ni2);
    return *this;
  }
  /// Allow up to this many elements in 3D
  CkArrayOptions& setBounds(int ni1, int ni2, int ni3) {
    bounds = CkArrayIndex3D(ni1, ni2, ni3);
    return *this;
  }
  /// Allow up to this many elements in 4D
  CkArrayOptions& setBounds(short ni1, short ni2, short ni3, short ni4) {
    bounds = CkArrayIndex4D(ni1, ni2, ni3, ni4);
    return *this;
  }
  /// Allow up to this many elements in 5D
  CkArrayOptions& setBounds(short ni1, short ni2, short ni3, short ni4, short ni5) {
    bounds = CkArrayIndex5D(ni1, ni2, ni3, ni4, ni5);
    return *this;
  }
  /// Allow up to this many elements in 6D
  CkArrayOptions& setBounds(short ni1, short ni2, short ni3, short ni4, short ni5,
                            short ni6) {
    bounds = CkArrayIndex6D(ni1, ni2, ni3, ni4, ni5, ni6);
    return *this;
  }

  /// Use this location map
  CkArrayOptions& setMap(const CkGroupID& m) {
    map = m;
    return *this;
  }

  /// Bind our elements to this array
  CkArrayOptions& bindTo(const CkArrayID& b);

  /// Use this location manager
  CkArrayOptions& setLocationManager(const CkGroupID& l) {
    locMgr = l;
    return *this;
  }

  /// Use this ckmulticast manager
  CkArrayOptions& setMcastManager(const CkGroupID& m) {
    mCastMgr = m;
    return *this;
  }

  CkArrayOptions& setLocationCache(const CkGroupID& l) {
    locCache = l;
    return *this;
  }

  /// Add an array listener component to this array (keeps the new'd listener)
  CkArrayOptions& addListener(CkArrayListener* listener);

  CkArrayOptions& setAnytimeMigration(bool b) {
    anytimeMigration = b;
    return *this;
  }
  CkArrayOptions& setStaticInsertion(bool b);
  bool isStaticInsertion() {
    if (insertionType == InsertionType::STATIC) return true;
    if (insertionType == InsertionType::DYNAMIC || numInitial.dimension == 0) return false;

    bool empty = false;
    bool shorts = numInitial.dimension > 3;
    for (int i = 0; i < numInitial.dimension; i++) {
      if (shorts && ((short*)numInitial.data())[i] == 0) {
        empty = true;
        break;
      } else if (!shorts && numInitial.data()[i] == 0) {
        empty = true;
        break;
      }
    }
    if (empty) return false;
    else return true;
  }

  CkArrayOptions& setBroadcastViaScheduler(bool b) {
    broadcastViaScheduler = b;
    return *this;
  }
  CkArrayOptions& setSectionAutoDelegate(bool b) {
    sectionAutoDelegate = b;
    return *this;
  }
  CkArrayOptions& setReductionClient(CkCallback cb) {
    reductionClient = cb;
    return *this;
  }
  CkArrayOptions &setInitCallback(CkCallback cb) {
    initCallback = cb;
    return *this;
  }

  // Used by the array manager:
  const CkArrayIndex& getStart(void) const { return start; }
  const CkArrayIndex& getEnd(void) const { return end; }
  const CkArrayIndex& getStep(void) const { return step; }
  const CkArrayIndex& getNumInitial(void) const { return numInitial; }
  const CkArrayIndex& getBounds(void) const { return bounds; }
  const CkGroupID& getMap(void) const { return map; }
  const CkGroupID& getLocationManager(void) const { return locMgr; }
  const CkGroupID& getMcastManager(void) const { return mCastMgr; }
  const CkGroupID& getLocationCache(void) const { return locCache; }
  bool isSectionAutoDelegated(void) const { return sectionAutoDelegate; }
  const CkCallback &getInitCallback(void) const {return initCallback;}
  int getListeners(void) const { return arrayListeners.size(); }
  CkArrayListener* getListener(int listenerNum) {
    CkArrayListener* ret = arrayListeners[listenerNum];
    arrayListeners[listenerNum] = NULL;  // Don't throw away this listener
    return ret;
  }

  void pup(PUP::er& p);
};

#endif
