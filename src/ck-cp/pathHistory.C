#include <charm++.h>
#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <utility>

//#define DEBUG  1
#include "PathHistory.decl.h"
#include "pathHistory.h"
#include <register.h> 

#include "trace-projections.h"

/**
 *  \addtogroup CriticalPathFramework
 *   @{
 *
 */

/*readonly*/ CProxy_pathHistoryManager pathHistoryManagerProxy;

CkpvDeclare(int, traceLastHop);

CkpvDeclare(MergeablePathHistory, currentlyExecutingPath); // The maximal incoming path for the node
CkpvDeclare(double, timeEntryMethodStarted);

/** A table to store all the local nodes in the parallel dependency graph */
CkpvDeclare(PathHistoryTableType, pathHistoryTable);
/** A counter that defines the new keys for the entries in the pathHistoryTable */
CkpvDeclare(int, pathHistoryTableLastIdx);


/// A mainchare that is used just to create a group at startup
class pathHistoryMain : public CBase_pathHistoryMain {
public:
  pathHistoryMain(CkArgMsg* args){
#if USE_CRITICAL_PATH_HEADER_ARRAY
    pathHistoryManagerProxy = CProxy_pathHistoryManager::ckNew();
#endif
    delete args;
  }
  ~pathHistoryMain(){}
};

pathHistoryManager::pathHistoryManager(){ }

/** Trace perform a traversal backwards over the critical path specified as a 
 * table index for the processor upon which this is called.
 *
 * The callback cb will be called with the resulting msg after the path has 
 * been traversed to its origin.  
 **/

void pathHistoryManager::traceCriticalPathBackStepByStep(pathInformationMsg *msg){
   int count = CkpvAccess(pathHistoryTable).count(msg->table_idx);

#if DEBUGPRINT > 2
   CkPrintf("Table entry %d on pe %d occurs %d times in table\n", msg->table_idx, CkMyPe(), count);
#endif
   CkAssert(count==0 || count==1);

    if(count > 0){ 
      PathHistoryTableEntry & path = CkpvAccess(pathHistoryTable)[msg->table_idx];
      int idx = path.sender_history_table_idx;
      int pe = path.sender_pe;
//#if DEBUGPRINT > 2
#if DEBUG
      CkPrintf("Table entry %d on pe %d points to pe=%d idx=%d history size %d \n", msg->table_idx, CkMyPe(), pe, idx, msg->historySize);
#endif

      // Make a copy of the message as we forward it along
      pathInformationMsg *newmsg = new(msg->historySize+1) pathInformationMsg;
      for(int i=0;i<msg->historySize;i++){
          newmsg->history[i] = msg->history[i];
      }
      newmsg->history[msg->historySize] = path;
      newmsg->historySize = msg->historySize+1;
      newmsg->saveAsProjectionsUserEvents = msg->saveAsProjectionsUserEvents;
      newmsg->cb = msg->cb;
      newmsg->hops = msg->hops -1;
      newmsg->table_idx = idx;
        
      if(msg->hops > 0 ){
          // Not yet at origin, keep tracing the path back
          CkAssert(pe < CkNumPes() && pe >= 0);
          thisProxy[pe].traceCriticalPathBackStepByStep(newmsg);
      } else {
          if(msg->saveAsProjectionsUserEvents){

              // Keep a message for returning to the user's callback
              pathForUser = new(msg->historySize+1) pathInformationMsg;
              for(int i=0;i<msg->historySize;i++){
                  pathForUser->history[i] = msg->history[i];
              }
              pathForUser->history[msg->historySize] = path;
              pathForUser->historySize = msg->historySize+1;
              pathForUser->saveAsProjectionsUserEvents = msg->saveAsProjectionsUserEvents;
              pathForUser->cb = msg->cb;
              pathForUser->table_idx = idx;

              CkPrintf("Broadcasting it to all PE\n");
              thisProxy.broadcastCriticalPathProjections(newmsg);
          } else {
              newmsg->cb.send(newmsg);
          }

      }
    } else {
        CkAbort("ERROR: Traced critical path back to a nonexistent table entry.\n");
    }

    delete msg;
  }


void pathHistoryManager::broadcastCriticalPathProjections(pathInformationMsg *msg){

  CkPrintf("[%d] Received broadcast of critical path\n", CkMyPe());
  int me = CkMyPe();
  int intersectsLocalPE = false;

  // Create user events for critical path

  for(int i=msg->historySize-1;i>=0;i--){
    if(CkMyPe() == msg->history[i].local_pe){
      // Part of critical path is local
      // Create user event for it

      traceUserBracketEvent(32000, msg->history[i].get_start_time(), msg->history[i].get_start_time() + msg->history[i].get_local_path_time());

      intersectsLocalPE = true;
    }

  }

  traceRegisterUserEvent("Critical Path", 32000);
  
  
#define PRUNE_CRITICAL_PATH_LOGS 0

#if PRUNE_CRITICAL_PATH_LOGS
  // Tell projections tracing to only output log entries if I contain part of the critical path

  enableTraceLogOutput();
  if(! intersectsLocalPE){
    disableTraceLogOutput();
    CkPrintf("PE %d doesn't intersect the critical path, so its log files won't be created\n", CkMyPe() );
  }
#endif
  
#if TRACE_ALL_PATH_TABLE_ENTRIES
  // Create user events for all table entries
  std::map< int, PathHistoryTableEntry >::iterator iter;
  for(iter=pathHistoryTable.begin(); iter != pathHistoryTable.end(); iter++){
    double startTime = iter->second.get_start_time();
    double endTime = iter->second.get_start_time() + iter->second.get_local_path_time();
    traceUserBracketEvent(32001, startTime, endTime);
  }
#endif

  int data=1;
  CkCallback cb(CkIndex_pathHistoryManager::criticalPathProjectionsDone(NULL),thisProxy[0]); 
  contribute(sizeof(int), &data, CkReduction::sum_int, cb);


}

void pathHistoryManager::criticalPathProjectionsDone(CkReductionMsg *msg){
  CkPrintf("[%d] All PEs have received the critical path information. Sending critical path to user supplied callback.\n", CkMyPe());
  pathForUser->cb.send(pathForUser);
  pathForUser = NULL;
}

/// An interface callable by the application.
void useThisCriticalPathForPriorities(){
  pathHistoryManagerProxy.ckLocalBranch()->useCriticalPathForPriories();
}


/// Callable from inside charm++ delivery mechanisms (after envelope contains epIdx):
void automaticallySetMessagePriority(envelope *env){
    int ep = env->getEpIdx();
    if (ep==CkIndex_CkArray::recvBroadcast(0))
        ep = env->getsetArrayBcastEp();
#if DEBUG 
    CkPrintf("----------- ep = %d  %s \n", ep, _entryTable[ep]->name); 
    if(env->getPriobits() == 8*sizeof(int)){
    CkPrintf("[%d] priorities for env=%p are integers\n", CkMyPe(), env);
  } else if(env->getPriobits() == 0) {
    CkPrintf("[%d] priorities for env=%p are not allocated in message\n", CkMyPe(), env);
  } else {
    CkPrintf("[%d] priorities for env=%p are not integers: %d priobits\n", CkMyPe(), env, env->getPriobits());
  }
#endif
  
 
  const std::map< int, int> & criticalPathForPriorityCounts = pathHistoryManagerProxy.ckLocalBranch()->getCriticalPathForPriorityCounts();

  if(criticalPathForPriorityCounts.size() > 0 && env->getPriobits() == 8*sizeof(int)) {
    switch(env->getMsgtype()) {
    case ForArrayEltMsg:
    case ForIDedObjMsg:
    case ForChareMsg:
    case ForNodeBocMsg:
    case ForBocMsg:
    case ArrayEltInitMsg:
        {        
          const int arr = env->getArrayMgrIdx();
          const int count = criticalPathForPriorityCounts.count(ep);
#if DEBUG
          CkPrintf("[%d] destination array,ep occurs %d times along stored critical path\n", CkMyPe(), count);
#endif
      	
          if(count > 0){
              // Set the integer priority to high
#if DEBUG 
              CkPrintf("Prio auto high %d  %s \n", ep, _entryTable[ep]->name);
#endif
              *(int*)(env->getPrioPtr()) = -5;
          } else {
              // Set the integer priority to low
#if DEBUG 
              CkPrintf("Prio auto low: %d,%d\n", arr, ep);
#endif
              *(int*)(env->getPrioPtr()) = 0;
          }

      }
      break;
      
    default:
      //CkPrintf("Can't Critical Path Autoprioritize messages of [unknown type]\n");
      break;
    }
      
  }
}

void pathHistoryManager::useCriticalPathForPriories(){
  // Request a critical path that will be stored everywhere for future use in autotuning message priorities
  // The resulting critical path should be broadcast to saveCriticalPathForPriorities() on all PEs
  CkCallback cb(CkIndex_pathHistoryManager::saveCriticalPathForPriorities(NULL),thisProxy); 
  traceCriticalPathBack(cb, false);
}

void pathHistoryManager::saveCriticalPathForPriorities(pathInformationMsg *msg){
  //CkPrintf("[%d] saveCriticalPathForPriorities() Receiving critical paths\n", CkMyPe());
  fflush(stdout);
  
  criticalPathForPriorityCounts.clear();
  
  // Save a list of which entries are along the critical path
  for(int i=msg->historySize-1;i>=0;i--){
    
    PathHistoryTableEntry &e = msg->history[i];
    
//#if 1 
#if DEBUG
    if(CkMyPe() == 0){
        char name[100];
        if(e.local_ep >=0)
            strcpy(name, _entryTable[e.local_ep]->name);
        else
            strcpy(name, "unknow");

      CkPrintf("\t[%d] Path Step %d: local_path_time=%lf ep=%d starttime=%lf preceding path time=%lf pe=%d name=%s\n",CkMyPe(), i, e.get_local_path_time(), e.local_ep, e.get_start_time(), e.get_preceding_path_time(), e.local_pe, name);
    }
#endif
    
    if(criticalPathForPriorityCounts.count(e.local_ep) == 1)
      criticalPathForPriorityCounts[e.local_ep]++;
    else
      criticalPathForPriorityCounts[e.local_ep] = 1;  
  }

  // print out the list just for debugging purposes
  if(CkMyPe() == 0){
    std::map< int, int>::iterator iter;
    for(iter=criticalPathForPriorityCounts.begin();iter!=criticalPathForPriorityCounts.end();++iter){
      int epidx = iter->first;
      const int c = iter->second;

#if DEBUG
      CkPrintf("[%d] On critical path EP %d occurs %d times\n", CkMyPe(), epidx, c);
#endif

    }
  }
}

/// Add an entry for this path history into the table, and write the corresponding information into the outgoing envelope
int PathHistoryTableEntry::addToTableAndEnvelope(envelope *env){
  // Add to table
  int new_idx = addToTable();

#if USE_CRITICAL_PATH_HEADER_ARRAY
  // Fill in outgoing envelope
  CkAssert(env != NULL);
  env->pathHistory.set_sender_history_table_idx(new_idx);
  env->pathHistory.setTime(local_path_time + preceding_path_time);
#if DEBUG
  if(local_path_time > 0.1)
      CkPrintf("------########## %d generating new msg env   critical path length %f:%f:%f app time %f id:%d\n", CkMyPe(), local_path_time , preceding_path_time, local_path_time+preceding_path_time, CkWallTimer(), new_idx);
#endif
#endif

  return new_idx;
}

/// Add an entry for this path history into the table. Returns the index in the table for it.
int PathHistoryTableEntry::addToTable(){
  int new_idx = CkpvAccess(pathHistoryTableLastIdx) ++;
  CkpvAccess(pathHistoryTable)[new_idx] = *this;
#if DEBUG
  CkPrintf("-------- add to entry  %d   ----- %d  %d %d  \n", new_idx, local_ep,  local_pe, sender_history_table_idx); 
#endif
  return new_idx;
}

void initializeCriticalPath(void){
  CkpvInitialize(MergeablePathHistory, currentlyExecutingPath); // The maximal incoming path for the node
  CkpvInitialize(double, timeEntryMethodStarted);
  CkpvAccess(timeEntryMethodStarted) = 0.0;
  CkpvInitialize(PathHistoryTableType, pathHistoryTable);
  CkpvInitialize(int, pathHistoryTableLastIdx);
  CkpvAccess(pathHistoryTableLastIdx) = 0;
  CkpvInitialize(int, traceLastHop);
  CkpvAccess(traceLastHop) = 0;
}


void resetThisEntryPath(void) {
  CkpvAccess(currentlyExecutingPath).reset();
}


/// A debugging routine that outputs critical path info as user events.
void  saveCurrentPathAsUserEvent(const char* prefix){
  if(CkpvAccess(currentlyExecutingPath).getTotalTime() > 0.0){
    //traceUserEvent(5020);

#if 0
    char *note = new char[4096];
    sprintf(note, "%s<br> saveCurrentPathAsUserEvent()<br> ", prefix);
    CkpvAccess(currentlyExecutingPath).printHTMLToString(note+strlen(note));
    traceUserSuppliedNote(note); // stores a copy of the string
    delete[] note;
#endif

  } else {
#if 0
    traceUserEvent(5010);
#endif
  }
 
}


void setCurrentlyExecutingPathTo100(void){
    CkpvAccess(currentlyExecutingPath).setDebug100();
}

/// Acquire the critical path and deliver it to the user supplied callback
void traceCriticalPathBack(CkCallback cb, bool saveToProjectionsTraces){
    pathInformationMsg *newmsg = new pathInformationMsg;
    newmsg->historySize = 0;
    newmsg->cb = cb;
    newmsg->hops = CkpvAccess(currentlyExecutingPath).hops - CkpvAccess(traceLastHop) -1;
    CkpvAccess(traceLastHop) = CkpvAccess(currentlyExecutingPath).hops;
    newmsg->saveAsProjectionsUserEvents = saveToProjectionsTraces;
    newmsg->table_idx = CkpvAccess(currentlyExecutingPath).sender_history_table_idx;
    int pe = CkpvAccess(currentlyExecutingPath).sender_pe;
    //CkPrintf("Starting tracing of critical path  current PE : %d, current entry %d to pe=%d table_idx=%d\n", CkMyPe(), pe,  CkpvAccess(currentlyExecutingPath).local_ep, CkpvAccess(currentlyExecutingPath).sender_history_table_idx);
    CkAssert(pe < CkNumPes() && pe >= 0);
    pathHistoryManagerProxy[pe].traceCriticalPathBackStepByStep(newmsg);
}

/// A debugging routine that prints the number of EPs for the program, and the size of the envelope's path fields
void  printEPInfo(){
  CkPrintf("printEPInfo():\n");
  CkPrintf("There are %d EPs\n", (int)_entryTable.size());
  for (int epIdx=0;epIdx<_entryTable.size();epIdx++)
    CkPrintf("EP %d is %s\n", epIdx, _entryTable[epIdx]->name);
}

#if USE_CRITICAL_PATH_HEADER_ARRAY

void criticalPath_setep(int epIdx){ 
    CkpvAccess(currentlyExecutingPath).local_ep  = epIdx;
#if DEBUG
    if(epIdx >= 0)
        CkPrintf(" setting current method name %d %s",epIdx,  _entryTable[epIdx]->name);  
    CkPrintf("\n");
#endif
}

/// Save information about the critical path contained in the message that is about to execute.
void criticalPath_start(envelope * env){ 
  CkpvAccess(currentlyExecutingPath).sender_pe = env->getSrcPe();
  CkpvAccess(currentlyExecutingPath).sender_history_table_idx = env->pathHistory.get_sender_history_table_idx();
  CkpvAccess(currentlyExecutingPath).preceding_path_time = env->pathHistory.getTotalTime();
  CkpvAccess(currentlyExecutingPath).hops =  env->pathHistory.getHops() + 1;

  CkpvAccess(currentlyExecutingPath).sanity_check();
  
  CkpvAccess(currentlyExecutingPath).local_ep  = env->getEpIdx();

  double now = CmiWallTimer();
  CkpvAccess(currentlyExecutingPath).timeEntryMethodStarted = now;
  CkpvAccess(timeEntryMethodStarted) = now;

  switch(env->getMsgtype()) {
  case ForArrayEltMsg:
  case ArrayEltInitMsg:
    CkpvAccess(currentlyExecutingPath).local_ep = env->getsetArrayEp();
    break;

  case ForNodeBocMsg:
    break;

  case ForBocMsg:
    //CkPrintf("Critical Path Detection handling a ForBocMsg\n");    
    break;

  case ForChareMsg:
    //CkPrintf("Critical Path Detection handling a ForChareMsg\n");        
    break;

  default:
    break;
    //CkPrintf("Critical Path Detection can't yet handle message type %d\n", (int)env->getMsgtype());
  }

  if(CkpvAccess(currentlyExecutingPath).local_ep == CkIndex_CkArray::recvBroadcast(0))
      CkpvAccess(currentlyExecutingPath).local_ep = env->getsetArrayBcastEp();

#if DEBUG
  CkPrintf("criticalPath_start(envelope * env) srcpe=%d sender table idx=%d  time=%lf current ep=%d ", env->getSrcPe(),  env->pathHistory.get_sender_history_table_idx(), env->pathHistory.getTotalTime(), env->getEpIdx() );
  if(env->getEpIdx() >= 0)
      CkPrintf(" current method name %d  %s", CkpvAccess(currentlyExecutingPath).local_ep, _entryTable[CkpvAccess(currentlyExecutingPath).local_ep]->name);  
  CkPrintf("\n");
#endif

  saveCurrentPathAsUserEvent("criticalPath_start()<br> ");
}


/// Modify the envelope of a message that is being sent for critical path detection and store an entry in a table on this PE.
void criticalPath_send(envelope * sendingEnv){
#if DEBUG
  CkPrintf("criticalPath_send(envelope * sendingEnv)\n");
#endif
  double now = CmiWallTimer();
  PathHistoryTableEntry entry(CkpvAccess(currentlyExecutingPath), CkpvAccess(timeEntryMethodStarted), now);
  entry.addToTableAndEnvelope(sendingEnv);
  sendingEnv->pathHistory.setHops(CkpvAccess(currentlyExecutingPath).hops);
  automaticallySetMessagePriority(sendingEnv);
}

/// Handle the end of the entry method in the critical path detection processes. This should create a forward dependency for the object.
void criticalPath_end(){
  saveCurrentPathAsUserEvent("criticalPath_end()<br> ");

  CkpvAccess(currentlyExecutingPath).reset();

#if DEBUG
  CkPrintf("criticalPath_end()\n");
#endif
}

#endif

/// Split an entry method invocation into multiple logical tasks for the critical path analysis.
/// SDAG doen's break up the code in useful ways, so I'll make it add calls to this in the generated code.
void criticalPath_split(){
  saveCurrentPathAsUserEvent("criticalPath_split()<br> ");

  // save an entry in the table
  double now = CmiWallTimer();
  PathHistoryTableEntry entry(CkpvAccess(currentlyExecutingPath), now-CkpvAccess(timeEntryMethodStarted) );
  int tableidx = entry.addToTable();
  // end the old task
  // start the new task
  CkpvAccess(currentlyExecutingPath).sender_pe = CkMyPe();
  CkpvAccess(currentlyExecutingPath).sender_history_table_idx = tableidx;
  CkpvAccess(currentlyExecutingPath).preceding_path_time = entry.getTotalTime();
  CkpvAccess(currentlyExecutingPath).timeEntryMethodStarted = now;
  CkpvAccess(timeEntryMethodStarted) = now;
}

MergeablePathHistory* saveCurrentPath()
{
    MergeablePathHistory *savedPath = new MergeablePathHistory(CkpvAccess(currentlyExecutingPath));
    return savedPath;
}


void mergePathHistory(MergeablePathHistory* tmp)
{
    CkpvAccess(currentlyExecutingPath).updateMax(*tmp);
}

#include "PathHistory.def.h"

/*! @} */
