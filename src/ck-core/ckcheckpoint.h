/*
Charm++ File: Checkpoint Library
added 01/03/2003 by Chao Huang, chuang10@uiuc.edu

CkStartCheckpoint() is a function to start the procedure
of saving the status of a Charm++ program into disk files.
A corresponding restarting mechanism can later use the
files saved to restore the execution. A callback should
be provided to continue after the checkpoint is done.

Checkpoint manager is a Group to aid the saving and
restarting of Charm++ programs. ...

--- Updated 12/14/2003 by Gengbin, gzheng@uiuc.edu
    rewrote to allow code reuse with following 5 functions, 
    these functions each handle both packing and unpacking of a system data:
	void CkPupROData(PUP::er &p);
	void CkPupMainChareData(PUP::er &p);
	void CkPupGroupData(PUP::er &p);
	void CkPupNodeGroupData(PUP::er &p);
	void CkPupArrayElementsData(PUP::er &p);
    Completely changed the data file format for array elements to become
    one file for each processor. 
    Two main checkpoint/restart subroutines are greatly simplified.
*/
#ifndef _CKCHECKPOINT_H
#define _CKCHECKPOINT_H

#include <pup.h>
#include <ckcallback.h>
#include <ckmessage.h>
#include "CkCheckpointStatus.decl.h"

// loop over all CkLocMgr and do "code"
#define  CKLOCMGR_LOOP(code)	\
  for(i=0;i<numGroups;i++) {	\
    IrrGroup *obj = CkpvAccess(_groupTable)->find((*CkpvAccess(_groupIDTable))[i]).getObj();	\
    if(obj && obj->isLocMgr())  {	\
      CkLocMgr *mgr = (CkLocMgr*)obj;	\
      code	\
    }	\
  }

// utility functions to pup system global tables
void CkPupROData(PUP::er &p);
void CkPupMainChareData(PUP::er &p, CkArgMsg *args);
void CkPupChareData(PUP::er &p);
void CkPupGroupData(PUP::er &p);
void CkPupNodeGroupData(PUP::er &p);
void CkPupArrayElementsData(PUP::er &p, int notifyListeners=1);
void CkPupProcessorData(PUP::er &p);
void CkRemoveArrayElements();
//void CkTestArrayElements();

// If writersPerNode <= 0 the number of writers is unchanged, if > 0, then set to
// min(writersPerNode, CkMyNodeSize())
void CkStartCheckpoint(const char* dirname, const CkCallback& cb,
                       bool requestStatus = false, int writersPerNode = 0);
void CkRestartMain(const char* dirname, CkArgMsg* args);
#if CMK_SHRINK_EXPAND
void CkResumeRestartMain(char *msg);
#endif
// also used by the in-memory checkpoint, which does not need __FAULT__
#if __FAULT__ || CMK_MEM_CHECKPOINT
int  CkCountArrayElements();
#endif

#if CMK_SHRINK_EXPAND
enum realloc_state : uint8_t { NO_REALLOC=0, REALLOC_MSG_RECEIVED=1, REALLOC_IN_PROGRESS=2 };
extern realloc_state pending_realloc_state;
extern CkGroupID _lbmgr;
#endif

// some useful flags (for disk checkpointing)
extern bool _inrestart;          // 1: if is during restart process
extern bool _restarted;          // 1: if this run is after restart
extern int _oldNumPes;           // number of processors in the last run
extern bool _chareRestored;      // 1: if chare is restored at restart

enum{CK_CHECKPOINT_SUCCESS, CK_CHECKPOINT_FAILURE};

class CkCheckpointStatusMsg:public CMessage_CkCheckpointStatusMsg{
public:
  int status;
  CkCheckpointStatusMsg(int _status): status(_status){}
};

#endif //_CKCHECKPOINT_H
