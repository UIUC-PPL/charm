#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "converse.h"
#include "charm-api.h"

#include <vector>
#include <deque>
#include <queue>

/**
 * Structure to hold the requisites for a callback
 */
struct ccd_callback {
  CcdVoidFn fn;
  void *arg;
  int pe;			/* the pe that sets the callback */

  ccd_callback(CcdVoidFn f, void *a, int p)
    : fn{f}, arg{a}, pe{p}
  { }
};



/**
 * A list of callbacks stored as an array and handled like a list
 */
struct ccd_cblist {
  std::deque<ccd_callback> elems{};
  unsigned char flag{};
};



/** Remove element referred to by given list index idx. */
static inline void remove_elem(ccd_cblist & l, int idx)
{
  l.elems.erase(l.elems.begin() + idx);
}



/** Remove n elements from the beginning of the list. */
static inline void remove_n_elems(ccd_cblist & l, size_t n)
{
  if (n == 0 || l.elems.size() < n)
    return;

  l.elems.erase(l.elems.begin(), l.elems.begin() + (n-1));
}



/** Append callback to the given cblist, and return the index. */
static inline int append_elem(ccd_cblist & l, CcdVoidFn fn, void *arg, int pe)
{
  l.elems.emplace_back(fn, arg, pe);
  return l.elems.size()-1;
}



/**
 * Trigger the callbacks in the provided callback list and *retain* them
 * after they are called. 
 *
 * Callbacks that are added after this function is started (e.g. callbacks 
 * registered from other callbacks) are ignored. 
 * @note: It is illegal to cancel callbacks from within ccd callbacks.
 */
static void call_cblist_keep(const ccd_cblist & l, double curWallTime)
{
  // save the length in case callbacks are added during execution
  const size_t len = l.elems.size();

  // we must iterate this way because insertion invalidates deque iterators
  for (size_t i = 0; i < len; ++i)
  {
    const ccd_callback & cb = l.elems[i];
    int old = CmiSwitchToPE(cb.pe);
    (*(cb.fn))(cb.arg, curWallTime);
    int unused = CmiSwitchToPE(old);
  }
}



/**
 * Trigger the callbacks in the provided callback list and *remove* them
 * from the list after they are called.
 *
 * Callbacks that are added after this function is started (e.g. callbacks 
 * registered from other callbacks) are ignored. 
 * @note: It is illegal to cancel callbacks from within ccd callbacks.
 */
static void call_cblist_remove(ccd_cblist & l, double curWallTime)
{
  /* reentrant */
  if (l.flag)
    return;
  l.flag = 1;

  // save the length in case callbacks are added during execution
  const size_t len = l.elems.size();

  // we must iterate this way because insertion invalidates deque iterators
#if ! CMK_BIGSIM_CHARM
  for (size_t i = 0; i < len; ++i)
#else
  // i < len is correct. after i==0, unsigned underflow will wrap to SIZE_MAX
  for (size_t i = len-1; i < len; --i)
#endif
  {
    const ccd_callback & cb = l.elems[i];
    int old = CmiSwitchToPE(cb.pe);
    (*(cb.fn))(cb.arg, curWallTime);
    int unused = CmiSwitchToPE(old);
  }

  remove_n_elems(l, len);

  l.flag = 0;
}



#define CBLIST_INIT_LEN   8
#define MAXNUMCONDS       (CcdUSERMAX + 1)

/**
 * Lists of conditional callbacks that are maintained by the scheduler
 */
struct ccd_cond_callbacks {
  ccd_cblist condcb[MAXNUMCONDS];
  ccd_cblist condcb_keep[MAXNUMCONDS];
};

/***/
CpvStaticDeclare(ccd_cond_callbacks, conds);   


// Default resolution of .005 seconds aka 5 milliseconds
#define CCD_DEFAULT_RESOLUTION 5.0e-3

/*Make sure this matches the CcdPERIODIC_* list in converse.h*/
#define CCD_PERIODIC_MAX 13
static constexpr const double periodicCallInterval[CCD_PERIODIC_MAX]=
{0.001, 0.010, 0.100, 1.0, 5.0, 10.0, 60.0, 2*60.0, 5*60.0, 10*60.0, 3600.0, 12*3600.0, 24*3600.0};

/**
 * List of periodic callbacks maintained by the scheduler
 */
struct ccd_periodic_callbacks {
	int nSkip;/*Number of opportunities to skip*/
	double lastCheck;/*Time of last check*/
	double resolution;
	double nextCall[CCD_PERIODIC_MAX];
};

/** */
CpvStaticDeclare(ccd_periodic_callbacks, pcb);
CpvDeclare(int, _ccd_numchecks);




/**
 * Structure used to manage callbacks in a heap
 */
struct ccd_heap_elem {
  double time;
  ccd_callback cb;

  ccd_heap_elem(double t, CcdVoidFn fn, void *arg, int pe)
    : time{t}, cb{fn, arg, pe}
  { }

  bool operator<(const ccd_heap_elem & rhs) const
  {
    return this->time < rhs.time;
  }
};


/** time-scheduled callbacks */
CpvStaticDeclare(std::priority_queue<ccd_heap_elem>, ccd_heap);



/**
 * Insert a new callback into the heap
 */
static inline void ccd_heap_insert(double t, CcdVoidFn fnp, void *arg, int pe)
{
  auto & h = CpvAccess(ccd_heap);
  h.emplace(t, fnp, arg, pe);
}



/**
 * Identify any (over)due callbacks that were scheduled
 * and trigger them. 
 */
static void ccd_heap_update(double curWallTime)
{
  auto & h = CpvAccess(ccd_heap);

  std::vector<ccd_callback> expired;

  /* Pull out all expired heap entries */
  for (const ccd_heap_elem * e = &h.top(); !h.empty() && e->time < curWallTime; h.pop(), e = &h.top())
    expired.emplace_back(e->cb);

  /* Now execute those heap entries.  This must be
     separated from the removal phase because executing
     an entry may change the heap. 
  */
  for (const ccd_callback & cb : expired)
  {
    int old = CmiSwitchToPE(cb.pe);
    (*(cb.fn))(cb.arg, curWallTime);
    int unused = CmiSwitchToPE(old);
  }
}



CLINKAGE void CcdCallBacksReset(void *ignored,double curWallTime);

/**
 * Initialize the callback containers
 */
void CcdModuleInit(char **ignored)
{
   int i;
   double curTime;
   CpvInitialize(std::priority_queue<ccd_heap_elem>, ccd_heap);
   CpvInitialize(ccd_cond_callbacks, conds);
   CpvInitialize(ccd_periodic_callbacks, pcb);
   CpvInitialize(int, _ccd_numchecks);

   CpvAccess(_ccd_numchecks) = 1;
   CpvAccess(pcb).nSkip = 1;
   curTime=CmiWallTimer();
   CpvAccess(pcb).lastCheck = curTime;
   for (i=0;i<CCD_PERIODIC_MAX;i++)
	   CpvAccess(pcb).nextCall[i]=curTime+periodicCallInterval[i];
   CpvAccess(pcb).resolution = CCD_DEFAULT_RESOLUTION;
   CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE,CcdCallBacksReset,0);
   CcdCallOnConditionKeep(CcdPROCESSOR_END_IDLE,CcdCallBacksReset,0);
}



/**
 * Register a callback function that will be triggered when the specified
 * condition is raised the next time
 */
int CcdCallOnCondition(int condnum, CcdVoidFn fnp, void *arg)
{
  CmiAssert(condnum < MAXNUMCONDS);
  return append_elem(CpvAccess(conds).condcb[condnum], fnp, arg, CcdIGNOREPE);
} 

/** 
 * Register a callback function that will be triggered on the specified PE
 * when the specified condition is raised the next time 
 */
int CcdCallOnConditionOnPE(int condnum, CcdVoidFn fnp, void *arg, int pe)
{
  CmiAssert(condnum < MAXNUMCONDS);
  return append_elem(CpvAccess(conds).condcb[condnum], fnp, arg, pe);
} 

/**
 * Register a callback function that will be triggered *whenever* the specified
 * condition is raised
 */
int CcdCallOnConditionKeep(int condnum, CcdVoidFn fnp, void *arg)
{
  CmiAssert(condnum < MAXNUMCONDS);
  return append_elem(CpvAccess(conds).condcb_keep[condnum], fnp, arg, CcdIGNOREPE);
} 

/**
 * Register a callback function that will be triggered on the specified PE
 * *whenever* the specified condition is raised
 */
int CcdCallOnConditionKeepOnPE(int condnum, CcdVoidFn fnp, void *arg, int pe)
{
  CmiAssert(condnum < MAXNUMCONDS);
  return append_elem(CpvAccess(conds).condcb_keep[condnum], fnp, arg, pe);
} 


/**
 * Cancel a previously registered conditional callback
 */
void CcdCancelCallOnCondition(int condnum, int idx)
{
  CmiAssert(condnum < MAXNUMCONDS);
  remove_elem(CpvAccess(conds).condcb[condnum], idx);
}


/**
 * Cancel a previously registered conditional callback
 */
void CcdCancelCallOnConditionKeep(int condnum, int idx)
{
  CmiAssert(condnum < MAXNUMCONDS);
  remove_elem(CpvAccess(conds).condcb_keep[condnum], idx);
}


/**
 * Register a callback function that will be triggered on the specified PE
 * after a minimum delay of deltaT
 */
void CcdCallFnAfterOnPE(CcdVoidFn fnp, void *arg, double deltaT, int pe)
{
    double ctime  = CmiWallTimer();
    double tcall = ctime + deltaT/1000.0;
    ccd_heap_insert(tcall, fnp, arg, pe);
} 

/**
 * Register a callback function that will be triggered after a minimum 
 * delay of deltaT
 */
void CcdCallFnAfter(CcdVoidFn fnp, void *arg, double deltaT)
{
    CcdCallFnAfterOnPE(fnp, arg, deltaT, CcdIGNOREPE);
} 


/**
 * Raise a condition causing all registered callbacks corresponding to 
 * that condition to be triggered
 */
void CcdRaiseCondition(int condnum)
{
  CmiAssert(condnum < MAXNUMCONDS);
  double curWallTime=CmiWallTimer();
  call_cblist_remove(CpvAccess(conds).condcb[condnum], curWallTime);
  call_cblist_keep(CpvAccess(conds).condcb_keep[condnum], curWallTime);
}


/**
 * Internal helper function that updates the polling resolution for time
 * based callbacks to minimum of two arguments and ensures appropriate
 * counters etc are reset
 */
double CcdSetMinResolution(double newResolution, double minResolution) {
  ccd_periodic_callbacks* o = &CpvAccess(pcb);
  double oldResolution = o->resolution;

  o->resolution = fmin(newResolution, minResolution);

  // Ensure we don't miss the new quantum
  if (o->resolution < oldResolution) {
    CcdCallBacksReset(NULL, CmiWallTimer());
  }

  return oldResolution;
}

/**
 * Set the polling resolution for time based callbacks
 */
double CcdSetResolution(double newResolution) {
  return CcdSetMinResolution(newResolution, CCD_DEFAULT_RESOLUTION);
}

/**
 * Reset the polling resolution for time based callbacks to its default value
 */
double CcdResetResolution() {
  ccd_periodic_callbacks* o = &CpvAccess(pcb);
  double oldResolution = o->resolution;

  o->resolution = CCD_DEFAULT_RESOLUTION;

  return oldResolution;
}

/**
 * "Safe" operation that only ever increases the polling resolution for time
 * based callbacks
 */
double CcdIncreaseResolution(double newResolution) {
  return CcdSetMinResolution(newResolution, CpvAccess(pcb).resolution);
}

/* 
 * Trigger callbacks periodically, and also the time-indexed
 * functions if their time has arrived
 */
void CcdCallBacks(void)
{
  int i;
  ccd_periodic_callbacks *o=&CpvAccess(pcb);
  
  /* Figure out how many times to skip Ccd processing */
  double curWallTime = CmiWallTimer();

  unsigned int nSkip=o->nSkip;
#if 1
/* Dynamically adjust the number of messages to skip */
  double elapsed = curWallTime - o->lastCheck;
  // Adjust the number of skipped messages by a multiple between .5, if we
  // skipped too many messages last time, and 2, if we skipped too few.
  // Ideally elapsed = resolution and we keep nSkip the same i.e. multiply by 1
  if (elapsed > 0.0) {
    nSkip = (int)(nSkip * fmax(0.5, fmin(2.0, o->resolution / elapsed)));
  }

/* Keep skipping within a sensible range */
#define minSkip 1u
#define maxSkip 20u
  if (nSkip<minSkip) nSkip=minSkip;
  else if (nSkip>maxSkip) nSkip=maxSkip;
#else
/* Always skip a fixed number of messages */
  nSkip=1;
#endif

  CpvAccess(_ccd_numchecks)=o->nSkip=nSkip;
  o->lastCheck=curWallTime;
  
  ccd_heap_update(curWallTime);
  
  for (i=0;i<CCD_PERIODIC_MAX;i++) 
    if (o->nextCall[i]<=curWallTime) {
      CcdRaiseCondition(CcdPERIODIC+i);
      o->nextCall[i]=curWallTime+periodicCallInterval[i];
    }
    else 
      break; /*<- because intervals are multiples of one another*/
} 



/**
 * Called when something drastic changes-- restart ccd_num_checks
 */
void CcdCallBacksReset(void *ignored,double curWallTime)
{
  ccd_periodic_callbacks *o=&CpvAccess(pcb);
  CpvAccess(_ccd_numchecks)=o->nSkip=1;
  o->lastCheck=curWallTime;
}


