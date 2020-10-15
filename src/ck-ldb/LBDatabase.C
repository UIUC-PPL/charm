#include "LBDatabase.h"
#include "cksyncbarrier.h"

LBDatabase::LBDatabase() {
  omCount = omsRegistering = 0;
  obj_walltime = 0;
  statsAreOn = false;
  obj_running = false;
  objsEmptyHead = -1;
  commTable = new LBCommTable;
  syncBarrier = CkSyncBarrier::object();
}

LDOMHandle LBDatabase::RegisterOM(LDOMid userID, void* userPtr, LDCallbacks cb) {
  LDOMHandle newHandle;
  newHandle.id = userID;

  LBOM* om = new LBOM(this, userID, userPtr, cb);
  if (om != nullptr) {
    newHandle.handle = oms.size();
    oms.push_back(om);
  } else newHandle.handle = -1;
  om->DepositHandle(newHandle);
  omCount++;
  return newHandle;
}

void LBDatabase::UnregisterOM(LDOMHandle omh) {
  delete oms[omh.handle];
  oms[omh.handle] = nullptr;
  omCount--;
}

void LBDatabase::RegisteringObjects(LDOMHandle omh) {
  // for an unregistered anonymous OM to join and control the barrier
  if (omh.id.id.idx == 0) {
    if (omsRegistering == 0)
      syncBarrier->turnOff();
    omsRegistering++;
  }
  else {
    LBOM* om = oms[omh.handle];
    if (!om->RegisteringObjs()) {
      if (omsRegistering == 0)
        syncBarrier->turnOff();
      omsRegistering++;
      om->SetRegisteringObjs(true);
    }
  }
}

void LBDatabase::DoneRegisteringObjects(LDOMHandle omh)
{
  // for an unregistered anonymous OM to join and control the barrier
  if (omh.id.id.idx == 0) {
    omsRegistering--;
    if (omsRegistering == 0)
      syncBarrier->turnOn();
  }
  else {
    LBOM* om = oms[omh.handle];
    if (om->RegisteringObjs()) {
      omsRegistering--;
      om->SetRegisteringObjs(false);
      if (omsRegistering == 0)
        // This call to turnOn must come after the decrement of omsRegistering and the
        // call to SetRegisteringObjs(false) because turnOn() can start off a chain that
        // calls RegisteringObjects(omh), so this ensures that the variables are in the correct
        // state if flow reaches there.
        syncBarrier->turnOn();
    }
  }
}


LDObjHandle LBDatabase::RegisterObj(LDOMHandle omh, CmiUInt8 id,
                                    void* userPtr, int migratable) {
#if CMK_LBDB_ON
  LDObjHandle newhandle;

  newhandle.omhandle = omh;
  newhandle.id = id;

  {
    // objsEmptyHead maintains a linked list of empty positions within the objs array
    // If objsEmptyHead == LDObjEntry::DEFAULT_NEXT, there are no vacant positions, so add to the back
    // If objsEmptyHead != LDObjEntry::DEFAULT_NEXT, we place the new object at index objsEmptyHead and advance
    // objsEmptyHead to the next empty position.
    if (objsEmptyHead == LBObjEntry::DEFAULT_NEXT) {
      newhandle.handle = objs.size();
      LBObj *obj = new LBObj(newhandle, userPtr, migratable);
      objs.emplace_back(obj);
    } else {
      newhandle.handle = objsEmptyHead;
      LBObj *obj = new LBObj(newhandle, userPtr, migratable);
      objs[newhandle.handle].obj = obj;

      objsEmptyHead = objs[newhandle.handle].nextEmpty;
      objs[newhandle.handle].nextEmpty = LBObjEntry::DEFAULT_NEXT;
    }
  }

  return newhandle;
#endif
}

void LBDatabase::UnregisterObj(LDObjHandle h)
{
  delete objs[h.handle].obj;

  objs[h.handle].obj = nullptr;
  // Maintain the linked list of empty positions by adding the newly removed
  // index as the new objsEmptyHead
  objs[h.handle].nextEmpty = objsEmptyHead;
  objsEmptyHead = h.handle;
}

void LBDatabase::Send(const LDOMHandle &destOM, const CmiUInt8 &destID, unsigned int bytes, int destObjProc, int force)
{
#if CMK_LBDB_ON
  if (force || (StatsOn() && _lb_args.traceComm())) {
    LBCommData* item_ptr;

    if (obj_running) {
      const LDObjHandle &runObj = RunningObj();

      // Don't record self-messages from an object to an object
      if (runObj.omhandle.id == destOM.id
          && runObj.id == destID )
        return;

      // In the future, we'll have to eliminate processor to same
      // processor messages as well

      LBCommData item(runObj, destOM.id, destID, destObjProc);
      item_ptr = commTable->HashInsertUnique(item);
    } else {
      LBCommData item(CkMyPe(), destOM.id, destID, destObjProc);
      item_ptr = commTable->HashInsertUnique(item);
    }
    item_ptr->addMessage(bytes);
  }
#endif
}

void LBDatabase::MulticastSend(const LDOMHandle &destOM, CmiUInt8 *destIDs, int nDests, unsigned int bytes, int nMsgs)
{
#if CMK_LBDB_ON
  if (StatsOn() && _lb_args.traceComm()) {
    LBCommData* item_ptr;
    if (obj_running) {
      const LDObjHandle &runObj = RunningObj();

      LBCommData item(runObj, destOM.id, destIDs, nDests);
      item_ptr = commTable->HashInsertUnique(item);
      item_ptr->addMessage(bytes, nMsgs);
    }
  }
#endif
}

int LBDatabase::GetObjDataSz()
{
  int nitems = 0;
  int i;
  if (_lb_args.migObjOnly()) {
  for(i = 0; i < objs.size(); i++)
    if (objs[i].obj && (objs[i].obj)->data.migratable)
      nitems++;
  } else {
  for(i = 0; i < objs.size(); i++)
    if (objs[i].obj)
      nitems++;
  }
  return nitems;
}

void LBDatabase::GetObjData(LDObjData *dp)
{
  if (_lb_args.migObjOnly()) {
    for (int i = 0; i < objs.size(); i++) {
      LBObj* obj = objs[i].obj;
      if (obj && obj->data.migratable)
        *dp++ = obj->ObjData();
    }
  } else {
    for (int i = 0; i < objs.size(); i++) {
      LBObj* obj = objs[i].obj;
      if (obj)
        *dp++ = obj->ObjData();
    }
  }
}

void LBDatabase::BackgroundLoad(LBRealType* walltime, LBRealType* cputime)
{
  LBRealType total_walltime;
  LBRealType total_cputime;
  TotalTime(&total_walltime, &total_cputime);

  LBRealType idletime;
  IdleTime(&idletime);

  *walltime = total_walltime - idletime - obj_walltime;
  if (*walltime < 0) *walltime = 0.;
#if CMK_LB_CPUTIMER
  *cputime = total_cputime - obj_cputime;
#else
  *cputime = *walltime;
#endif
}

void LBDatabase::GetTime(LBRealType *total_walltime, LBRealType *total_cputime,
                         LBRealType *idletime, LBRealType *bg_walltime,
                         LBRealType *bg_cputime)
{
  TotalTime(total_walltime,total_cputime);

  IdleTime(idletime);

  *bg_walltime = *total_walltime - *idletime - obj_walltime;
  if (*bg_walltime < 0) *bg_walltime = 0.;
#if CMK_LB_CPUTIMER
  *bg_cputime = *total_cputime - obj_cputime;
#else
  *bg_cputime = *bg_walltime;
#endif
  //CkPrintf("HERE [%d] total: %f %f obj: %f %f idle: %f bg: %f\n", CkMyPe(), *total_walltime, *total_cputime, obj_walltime, obj_cputime, *idletime, *bg_walltime);
}

void LBDatabase::ClearLoads(void)
{
  int i;
  for (i = 0; i < objs.size(); i++) {
    LBObj *obj = objs[i].obj;
    if (obj)
    {
      if (obj->data.wallTime > 0.0) {
        obj->lastWallTime = obj->data.wallTime;
#if CMK_LB_CPUTIMER
        obj->lastCpuTime = obj->data.cpuTime;
#endif
      }
      obj->data.wallTime = 0.0;
#if CMK_LB_CPUTIMER
      obj->data.cpuTime = 0.0;
#endif
    }
  }
  delete commTable;
  commTable = new LBCommTable;
  machineUtil.Clear();
  obj_walltime = 0;
#if CMK_LB_CPUTIMER
  obj_cputime = 0;
#endif
}

int LBDatabase::Migrate(LDObjHandle h, int dest)
{
  if (h.handle >= objs.size()) {
    CmiAbort("[%d] LBDB::Migrate: Handle %d out of range 0-%zu\n",CkMyPe(),h.handle,objs.size());
  }
  else if (!(objs[h.handle].obj)) {
    CmiAbort("[%d] LBDB::Migrate: Handle %d no longer registered, range 0-%zu\n", CkMyPe(),h.handle,objs.size());
  }

  LBOM *const om = oms[(objs[h.handle].obj)->parentOM().handle];
  om->Migrate(h, dest);
  return 1;
}

void LBDatabase::MetaLBResumeWaitingChares(int lb_ideal_period) {
#if CMK_LBDB_ON
  for (int i = 0; i < objs.size(); i++) {
    LBObj* obj = objs[i].obj;
    if (obj) {
      LBOM *om = oms[obj->parentOM().handle];
      LDObjHandle h = obj->GetLDObjHandle();
      om->MetaLBResumeWaitingChares(h, lb_ideal_period);
    }
  }
#endif
}

void LBDatabase::MetaLBCallLBOnChares() {
#if CMK_LBDB_ON
  for (int i = 0; i < objs.size(); i++) {
    LBObj* obj = objs[i].obj;
    if (obj) {
      LBOM *om = oms[obj->parentOM().handle];
      LDObjHandle h = obj->GetLDObjHandle();
      om->MetaLBCallLBOnChares(h);
    }
  }
#endif
}

int LBDatabase::useMem() {
  int size = sizeof(LBDatabase);
  size += oms.size() * sizeof(LBOM);
  size += GetObjDataSz() * sizeof(LBObj);
  size += commTable->useMem();
  return size;
}

void LBDatabase::EstObjLoad(const LDObjHandle &_h, double cputime)
{
#if CMK_LBDB_ON
  LBObj *const obj = LbObj(_h);

  CmiAssert(obj != NULL);
  obj->setTiming(cputime);
#endif
}

void LBDatabase::EstObjLoad(const LDObjHandle &_h, double cputime, int phase)
{
#if CMK_LBDB_ON
  LBObj *const obj = LbObj(_h);

  CmiAssert(obj != NULL);
  obj->setTiming(cputime);
#endif
}
