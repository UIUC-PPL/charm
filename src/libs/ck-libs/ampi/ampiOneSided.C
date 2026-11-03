/*************************************************************
 * File: ampiOneSided.C
 *       This file contains one-sided communication functions
 *       win_obj class definition and AMPI_* implementations.
 *************************************************************/

#include "ampiimpl.h"
/*************************************************************
 * Local flags used for Win_obj class:
 *     WIN_ERROR -- the operation fails
 *     WIN_SUCCESS -- the operation succeeds
 *************************************************************/
#define WIN_SUCCESS 0
#define WIN_ERROR   (-1)

extern int AMPI_RDMA_THRESHOLD;

win_obj::win_obj() noexcept {
  baseAddr = NULL;
  comm = MPI_COMM_NULL;
  initflag = false;
}

win_obj::win_obj(const char *name, void *base, MPI_Aint size, int disp_unit,
                 MPI_Comm comm) noexcept {
  create(name, base, size, disp_unit, comm);
  owner = -1;  // the lock is not owned by anyone yet
}

void win_obj::setName(const char *src) noexcept {
  CkDDT_SetName(winName, src);
}

void win_obj::getName(char *name, int *len) noexcept {
  int length = *len = winName.size();
  memcpy(name, winName.data(), length);
  name[length] = '\0';
}

win_obj::~win_obj() noexcept {
  free();
}

// Note that this is supposed to be used for migration.
// We should not hava a remote methos which has to pack the win data --- Inefficient
void win_obj::pup(PUP::er &p) noexcept {
#if 0
  p|winSize;
  p|disp_unit;
  p|comm;
  p|initflag;

  p|winName;
  p|attributes;

  int size = 0;
  if(baseAddr) size = winSize;
  p|size;
  if(p.isUnpacking()) baseAddr = new char[size+1];
  p(baseAddr, size);
#endif
}

int win_obj::create(const char *name, void *base, MPI_Aint size, int disp_unit, MPI_Comm comm) noexcept {
  if (name) setName(name);
  baseAddr = base;
  winSize = size; // MPI_Win_create's size is in bytes
  this->disp_unit = disp_unit;
  this->comm = comm;
  // assume : memory pointed by base has been allocated
  initflag = true;
  return WIN_SUCCESS;
}

int win_obj::free() noexcept {
  // Assume : memory will be deallocated by user
  initflag = false;
  return WIN_SUCCESS;
}

// MPI-3.1 section 11.2.1: the target of an RMA call starts at byte offset
// disp_unit*target_disp of the window, and every byte it touches must lie in
// [0, size). checkTargetBytes takes the touched bytes as [lo, hi) relative to
// that offset; checkTarget computes them for targcnt elements of tddt.
void win_obj::checkTargetBytes(const char *opname, MPI_Aint targdisp, MPI_Aint lo,
                               MPI_Aint hi) const noexcept {
  if (!initflag) {
    CkAbort("AMPI> %s on a freed or non-existent MPI_Win\n", opname);
  }
  MPI_Aint offset = (MPI_Aint)disp_unit * targdisp;
  if (hi > lo && (offset + lo < 0 || offset + hi > winSize)) {
    CkAbort("AMPI> %s: target bytes [%ld, %ld) are outside the MPI_Win of %ld bytes "
            "(target_disp %ld, disp_unit %d)\n", opname, (long)(offset + lo), (long)(offset + hi),
            (long)winSize, (long)targdisp, disp_unit);
  }
}

void win_obj::checkTarget(const char *opname, MPI_Aint targdisp, int targcnt,
                          const CkDDT_DataType *tddt) const noexcept {
  if (targcnt <= 0) {
    checkTargetBytes(opname, targdisp, 0, 0); // nothing touched; window must exist
    return;
  }
  // Element k occupies [k*extent + trueLB, k*extent + trueLB + trueExtent);
  // the first and the last element bound the range (extent may be negative).
  MPI_Aint firstLo = tddt->getTrueLB();
  MPI_Aint lastLo = firstLo + (MPI_Aint)(targcnt - 1) * tddt->getExtent();
  MPI_Aint lo = std::min(firstLo, lastLo);
  MPI_Aint hi = std::max(firstLo, lastLo) + tddt->getTrueExtent();
  checkTargetBytes(opname, targdisp, lo, hi);
}

int win_obj::accumulate(void *orgaddr, int count, MPI_Aint targdisp, MPI_Datatype targtype,
                        MPI_Op op, ampiParent* pptr) noexcept
{
  //when called from winRemote entry methods, pptr must be taken from the ampi instance, not getAmpiParent().
  CkAssert(pptr != NULL);
  pptr->applyOp(targtype, op, count, orgaddr, (void*)((char*)baseAddr+disp_unit*targdisp));
  return WIN_SUCCESS;
}

int win_obj::fence() noexcept {
  return WIN_SUCCESS;
}

int win_obj::lock(int requestRank, int lock_type) noexcept {
  owner = requestRank;
  return WIN_SUCCESS;
}

int win_obj::unlock(int requestRank) noexcept {
  if (owner != requestRank){
    CkPrintf("    ERROR: Can't unlock a lock which you don't own.\n");
    return WIN_ERROR;
  }
  owner = -1;

  // dequeue from queue itself
  dequeue();

  return WIN_SUCCESS;
}

void win_obj::dequeue() noexcept {
  lockQueueEntry *lq = lockQueue.deq();
  delete lq;
}

void win_obj::enqueue(int requestRank, int lock_type) noexcept {
  lockQueueEntry *lq = new lockQueueEntry(requestRank, lock_type);
  lockQueue.enq(lq);
}

bool win_obj::emptyQueue() noexcept {
  return (lockQueue.length()==0);
}

void win_obj::lockTopQueue() noexcept {
  lockQueueEntry *lq = lockQueue.deq();
  lock(lq->requestRank, lq->lock_type);
  lockQueue.insert(0, lq);
}

/* these four functions are yet to implement */
int win_obj::wait() noexcept {
  return -1;
}

int win_obj::post() noexcept {
  return -1;
}

int win_obj::start() noexcept {
  return -1;
}

int win_obj::complete() noexcept {
  return -1;
}

int ampiParent::addWinStruct(WinStruct* win) noexcept {
  winStructList.push_back(win);
  return winStructList.size()-1;
}

WinStruct *ampiParent::getWinStruct(MPI_Win win) const noexcept {
#ifdef AMPI_ERROR_CHECKING
  if (winStructList.size() <= (int) win || win < 0)
    CkAbort("AMPI> Error: MPI_Win parameter invalid.");
#endif
  return winStructList[(int)win];
}

void ampiParent::removeWinStruct(WinStruct *win) noexcept {/*winStructList.remove(win);*/}

int ampi::winPut(const void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
                 MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, WinStruct *win) noexcept {
  CkDDT_DataType *ddt = getDDT()->getType(orgtype);
  int orgtotalsize = ddt->getSize(orgcnt);
  AMPI_DEBUG("    Rank[%d:%d] invoke Remote put at [%d]\n", thisIndex, myRank, rank);

  if (ddt->isContig()) {
    ampi *destPtr = thisProxy[rank].ckLocal();
    if (destPtr != NULL) {
      destPtr->winRemotePut(orgtotalsize, (char*)orgaddr, orgcnt, orgtype, targdisp,
                            targcnt, targtype, win->index);
    }
#if AMPI_RDMA_IMPL
    else if (orgtotalsize >= AMPI_RDMA_THRESHOLD) {
      AmpiRequestList& reqs = getReqs();
      SendReq* ampiReq = parent->reqPool.newReq<SendReq>(orgtype, myComm->getComm(), getDDT());
      MPI_Request req = reqs.insert(ampiReq);
      CkCallback completedSendCB(CkIndex_ampi::completedRdmaSend(NULL), thisProxy[thisIndex], true/*inline*/);
      completedSendCB.setRefnum(req);
      thisProxy[rank].winRemotePut(orgtotalsize, CkSendBuffer(orgaddr, completedSendCB), orgcnt, orgtype,
                                   targdisp, targcnt, targtype, win->index);
      parent = ampiReq->wait(parent, MPI_STATUS_IGNORE);
      parent->getReqs().freeNonPersReq(parent, req);
    }
#endif
    else {
      thisProxy[rank].winRemotePut(orgtotalsize, (char*)orgaddr, orgcnt, orgtype, targdisp,
                                   targcnt, targtype, win->index);
    }
  }
  else {
    std::vector<char> sorgaddr(orgtotalsize);
    int orgsize = getDDT()->getType(orgtype)->getSize(orgcnt);
    ddt->serialize((char*)orgaddr, sorgaddr.data(), orgcnt, orgsize, PACK);
    thisProxy[rank].winRemotePut(orgtotalsize, sorgaddr.data(), orgcnt, orgtype, targdisp,
                                 targcnt, targtype, win->index);
  }

  return MPI_SUCCESS;
}

void ampi::winRemotePut(int orgtotalsize, char* sorgaddr, int orgcnt, MPI_Datatype orgtype,
                        MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, int winIndex) noexcept {
  win_obj *winobj = winObjects[winIndex];
  CkDDT_DataType *tddt = getDDT()->getType(targtype);
  winobj->checkTarget("MPI_Put", targdisp, targcnt, tddt);
  char* targaddr = ((char*)(winobj->baseAddr)) + winobj->disp_unit*targdisp;
  int targsize = getDDT()->getType(targtype)->getSize(targcnt);
  tddt->serialize(targaddr, (char*)sorgaddr, targcnt, targsize, UNPACK);
}

int ampi::winGet(void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
                 MPI_Aint targdisp, int targcnt, MPI_Datatype targtype,
                 WinStruct *win) noexcept {
  // Send the request for data to remote side
  AMPI_DEBUG("    Rank[%d:%d] invoke Remote get at [%d]\n", thisIndex, myRank, rank);
  CkDDT_DataType *orgddt  = getDDT()->getType(orgtype);
  CkDDT_DataType *targddt = getDDT()->getType(targtype);
  int orgtotalsize  = orgddt->getSize(orgcnt);
  int targtotalsize = targddt->getSize(targcnt);

  // FIXME: DDT has no method to copy directly between two non-contiguous types, so we only handle
  // the case where one but not both of the types are non-contiguous here.
  if (orgddt->isContig() || targddt->isContig()) {
    ampi *destPtr = thisProxy[rank].ckLocal();
    if (destPtr != NULL) {
      char* targdata = destPtr->winLocalGet(orgcnt, orgtype, targdisp, targcnt, targtype, win->index);
      if (orgddt->isContig() && targddt->isContig()) {
        int orgsize = getDDT()->getType(orgtype)->getSize(orgcnt);
        orgddt->serialize((char*)orgaddr, targdata, orgcnt, orgsize, UNPACK);
      } else if (orgddt->isContig()) {
        // The origin is contiguous and the target is not: gather the target
        // elements into the origin buffer (PACK copies from targdata into
        // orgaddr), as winRemoteGet does into its reply.
        int targsize = getDDT()->getType(targtype)->getSize(targcnt);
        targddt->serialize(targdata, (char*)orgaddr, targcnt, targsize, PACK);
      } else {
        // The target is contiguous: scatter its bytes into the non-contiguous
        // origin buffer (UNPACK copies from targdata into orgaddr).
        int targsize = getDDT()->getType(targtype)->getSize(targcnt);
        orgddt->serialize((char*)orgaddr, targdata, orgcnt, targsize, UNPACK);
      }
      return MPI_SUCCESS;
    }
  }

  AmpiMsg* msg = thisProxy[rank].winRemoteGet(orgcnt, orgtype, targdisp, targcnt, targtype, win->index);

  // Process the reply message by serializing the data into the desired memory position
  int orgsize = getDDT()->getType(orgtype)->getSize(orgcnt);
  orgddt->serialize((char*)orgaddr, msg->getData(), orgcnt, orgsize, UNPACK);
  AMPI_DEBUG("    Rank[%d] got win  [%d] \n", thisIndex, *(int*)msg->getData());
  AMPI_DEBUG("    Rank[%d] got win  [%d] , size %d\n", thisIndex, *(int*)orgaddr, orgcnt);

  delete msg;
  return MPI_SUCCESS;
}

char* ampi::winLocalGet(int orgcnt, MPI_Datatype orgtype, MPI_Aint targdisp, int targcnt,
                        MPI_Datatype targtype, int winIndex) noexcept {
  AMPI_DEBUG("    LocalGet invoked at Rank[%d:%d]\n", thisIndex, myRank);

  win_obj *winobj = winObjects[winIndex];
  CkDDT_DataType *tddt = getDDT()->getType(targtype);
  winobj->checkTarget("MPI_Get", targdisp, targcnt, tddt);
  char* targaddr = (char*)(winobj->baseAddr) + winobj->disp_unit*targdisp;

  AMPI_DEBUG("    Rank[%d] local get win  [%d] \n", thisIndex, *(int*)(targaddr));
  return targaddr;
}

AmpiMsg* ampi::winRemoteGet(int orgcnt, MPI_Datatype orgtype, MPI_Aint targdisp, int targcnt,
                            MPI_Datatype targtype, int winIndex) noexcept {
  AMPI_DEBUG("    RemoteGet invoked at Rank[%d:%d]\n", thisIndex, myRank);

  win_obj *winobj = winObjects[winIndex];
  CkDDT_DataType *tddt = getDDT()->getType(targtype);
  int targtotalsize = tddt->getSize(targcnt); // bytes packed into the reply
  winobj->checkTarget("MPI_Get", targdisp, targcnt, tddt);
  char* targaddr = (char*)(winobj->baseAddr) + winobj->disp_unit*targdisp;

  AMPI_DEBUG("    Rank[%d] get win  [%d] \n", thisIndex, *(int*)(targaddr));
  AmpiMsg *msg = new (targtotalsize, 0) AmpiMsg(0, 0, MPI_RMA_TAG, thisIndex, targtotalsize);
  int targsize = getDDT()->getType(targtype)->getSize(targcnt);
  tddt->serialize(targaddr, msg->getData(), targcnt, targsize, PACK);
  return msg;
}

int ampi::winIget(MPI_Aint orgdisp, int orgcnt, MPI_Datatype orgtype, int rank,
                  MPI_Aint targdisp, int targcnt, MPI_Datatype targtype,
                  WinStruct *win, MPI_Request *req) noexcept {
  // Send the request to data and handle of Future to remote side
  AMPI_DEBUG("    Rank[%d:%d] request Remote iget at [%d]\n", thisIndex, myRank, rank);
  *req = thisProxy[rank].winRemoteIget(orgdisp, orgcnt, orgtype, targdisp, targcnt, targtype, win->index);
  return MPI_SUCCESS;
}

AmpiMsg* ampi::winRemoteIget(MPI_Aint orgdisp, int orgcnt, MPI_Datatype orgtype,
                             MPI_Aint targdisp, int targcnt,
                             MPI_Datatype targtype, int winIndex) noexcept {
  AMPI_DEBUG("    RemoteIget invoked at Rank[%d:%d]\n", thisIndex, myRank);
  win_obj *winobj = winObjects[winIndex];
  CkDDT_DataType *tddt = getDDT()->getType(targtype);
  int targtotalsize = tddt->getSize(targcnt); // bytes packed into the reply
  winobj->checkTarget("AMPI_Iget", targdisp, targcnt, tddt);

  AmpiMsg *msg = new (targtotalsize, 0) AmpiMsg(0, 0, MPI_RMA_TAG, thisIndex, targtotalsize);

  char* targaddr = (char*)(winobj->baseAddr) + targdisp*winobj->disp_unit;
  AMPI_DEBUG("    Rank[%d] iget win  [%d] \n", thisIndex, *(int*)(targaddr));
  int targsize = getDDT()->getType(targtype)->getSize(targcnt);
  tddt->serialize(targaddr, msg->getData(), targcnt, targsize, PACK);
  AMPI_DEBUG("    Rank[%d] copy win  [%d] \n", thisIndex, *(int*)msg->getData());
  return msg;
}

int ampi::winIgetWait(MPI_Request *request, MPI_Status *status) noexcept {
  // Wait on the Future object
  AMPI_DEBUG("    [%d] Iget Waiting [%d]\n", thisIndex, *request);
  status->msg = (AmpiMsg*)CkWaitReleaseFuture(*request);
  AMPI_DEBUG("    [%d] Iget Waiting [%d] awaken\n", thisIndex, *request);
  return MPI_SUCCESS;
}

int ampi::winIgetFree(MPI_Request *request, MPI_Status *status) noexcept {
  AMPI_DEBUG("    [%d] : Iget [%d] frees buffer\n", thisIndex, *request);

  void *data = NULL;
  AMPI_Iget_data(&data, *status);
  if(!data) {
    AMPI_DEBUG("    [%d] Iget [%d] attempt to free NULL buffer \n", thisIndex, *request);
    return ampiErrhandler("AMPI_Iget_free", MPI_ERR_BUFFER);
  }
  else {
    delete (status->msg);
    return MPI_SUCCESS;
  }
}

int ampi::winAccumulate(const void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
                        MPI_Aint targdisp, int targcnt, MPI_Datatype targtype,
                        MPI_Op op, WinStruct *win) noexcept {
  CkDDT_DataType *ddt = getDDT()->getType(orgtype);
  int orgtotalsize = ddt->getSize(orgcnt);
  AMPI_DEBUG("    Rank[%d:%d] invoke Remote accumulate at [%d]\n", thisIndex, myRank, rank);

  if (ddt->isContig()) {
    ampi *destPtr = thisProxy[rank].ckLocal();
    if (destPtr != NULL) {
      destPtr->winRemoteAccumulate(orgtotalsize, (char*)orgaddr, orgcnt, orgtype, targdisp,
                                   targcnt, targtype, op, win->index);
    }
#if AMPI_RDMA_IMPL
    else if (ddt->isContig() && orgtotalsize >= AMPI_RDMA_THRESHOLD) {
      AmpiRequestList& reqs = getReqs();
      SendReq* ampiReq = parent->reqPool.newReq<SendReq>(orgtype, myComm->getComm(), getDDT());
      MPI_Request req = reqs.insert(ampiReq);
      CkCallback completedSendCB(CkIndex_ampi::completedRdmaSend(NULL), thisProxy[thisIndex], true/*inline*/);
      completedSendCB.setRefnum(req);
      thisProxy[rank].winRemoteAccumulate(orgtotalsize, CkSendBuffer(orgaddr, completedSendCB), orgcnt,
                                          orgtype, targdisp, targcnt, targtype,  op, win->index);
      parent = ampiReq->wait(parent, MPI_STATUS_IGNORE);
      parent->getReqs().freeNonPersReq(parent, req);
    }
#endif
    else {
      thisProxy[rank].winRemoteAccumulate(orgtotalsize, (char*)orgaddr, orgcnt, orgtype,
                                          targdisp, targcnt, targtype,  op, win->index);
    }
  }
  else {
    std::vector<char> sorgaddr(orgtotalsize);
    int orgsize = getDDT()->getType(orgtype)->getSize(orgcnt);
    ddt->serialize((char*)orgaddr, sorgaddr.data(), orgcnt, orgsize, PACK);
    thisProxy[rank].winRemoteAccumulate(orgtotalsize, sorgaddr.data(), orgcnt, orgtype,
                                        targdisp, targcnt, targtype,  op, win->index);
  }

  return MPI_SUCCESS;
}

// Apply op to targcnt elements of the target (type tddt, handle targtype) at
// displacement targdisp, with operands taken from sorgaddr, which holds
// them packed (contiguous). The reduction functions work on the datatype's
// extent layout, so for a non-contiguous type the operands are first
// unpacked into a buffer of targcnt extents.
void ampi::accumulateIntoTarget(win_obj *winobj, char *sorgaddr, MPI_Aint targdisp, int targcnt,
                                CkDDT_DataType *tddt, MPI_Datatype targtype, MPI_Op op) noexcept {
  if (tddt->isContig()) {
    winobj->accumulate(sorgaddr, targcnt, targdisp, targtype, op, parent);
  }
  else {
    // CkDDT_DataType::serialize places element k at k*extent of userdata.
    // Only predefined target types reach here (MPI_Accumulate and
    // MPI_Get_accumulate reject derived ones), and their bytes lie in [0, extent).
    std::vector<char> getdata((size_t)targcnt * tddt->getExtent());
    int targsize = tddt->getSize(targcnt);
    tddt->serialize(getdata.data(), sorgaddr, targcnt, targsize, UNPACK);
    winobj->accumulate(getdata.data(), targcnt, targdisp, targtype, op, parent);
  }
}

void ampi::winRemoteAccumulate(int orgtotalsize, char* sorgaddr, int orgcnt,
                               MPI_Datatype orgtype, MPI_Aint targdisp,
                               int targcnt, MPI_Datatype targtype, MPI_Op op,
                               int winIndex) noexcept {
  win_obj *winobj = winObjects[winIndex];
  CkDDT_DataType *ddt = getDDT()->getType(targtype);
  winobj->checkTarget("MPI_Accumulate", targdisp, targcnt, ddt);
  accumulateIntoTarget(winobj, sorgaddr, targdisp, targcnt, ddt, targtype, op);
}

int ampi::winGetAccumulate(const void *orgaddr, int orgcnt, MPI_Datatype orgtype,
                           void *resaddr, int rescnt, MPI_Datatype restype, int rank,
                           MPI_Aint targdisp, int targcnt, MPI_Datatype targtype,
                           MPI_Op op, WinStruct *win) noexcept {
  CkDDT_DataType *orgddt = getDDT()->getType(orgtype);
  CkDDT_DataType *resddt = getDDT()->getType(restype);
  int orgtotalsize = orgddt->getSize(orgcnt);
  AMPI_DEBUG("    Rank[%d:%d] invoke Remote get at [%d]\n", thisIndex, myRank, rank);

  AmpiMsg *msg;
  if (orgddt->isContig()) {
    ampi *destPtr = thisProxy[rank].ckLocal();
    if (destPtr != NULL) {
      // winLocalGetAccumulate packs the original target elements contiguously;
      // a non-contiguous result datatype receives them through a buffer.
      if (resddt->isContig()) {
        destPtr->winLocalGetAccumulate(orgtotalsize, (char*)orgaddr, orgcnt, orgtype, targdisp,
                                       targcnt, targtype, op, (char*)resaddr, win->index);
      } else {
        int ressize = resddt->getSize(rescnt);
        std::vector<char> packed(std::max(ressize, getDDT()->getType(targtype)->getSize(targcnt)));
        destPtr->winLocalGetAccumulate(orgtotalsize, (char*)orgaddr, orgcnt, orgtype, targdisp,
                                       targcnt, targtype, op, packed.data(), win->index);
        resddt->serialize((char*)resaddr, packed.data(), rescnt, ressize, UNPACK);
      }
      return MPI_SUCCESS;
    }
#if AMPI_RDMA_IMPL
    else if (orgtotalsize >= AMPI_RDMA_THRESHOLD) {
      AmpiRequestList& reqs = getReqs();
      SendReq* ampiReq = parent->reqPool.newReq<SendReq>(orgtype, myComm->getComm(), getDDT());
      MPI_Request req = reqs.insert(ampiReq);
      CkCallback completedSendCB(CkIndex_ampi::completedRdmaSend(NULL), thisProxy[thisIndex], true/*inline*/);
      completedSendCB.setRefnum(req);
      msg = thisProxy[rank].winRemoteGetAccumulate(orgtotalsize, CkSendBuffer(orgaddr, completedSendCB), orgcnt,
                                                   orgtype, targdisp, targcnt, targtype, op, win->index);
      parent = ampiReq->wait(parent, MPI_STATUS_IGNORE);
      parent->getReqs().freeNonPersReq(parent, req);
    }
#endif
    else {
      msg = thisProxy[rank].winRemoteGetAccumulate(orgtotalsize, CkSendBuffer(orgaddr), orgcnt, orgtype, targdisp,
                                                   targcnt, targtype, op, win->index);
    }
  }
  else {
    std::vector<char> sorgaddr(orgtotalsize);
    int orgsize = getDDT()->getType(orgtype)->getSize(orgcnt);
    orgddt->serialize((char*)orgaddr, sorgaddr.data(), orgcnt, orgsize, PACK);
    msg = thisProxy[rank].winRemoteGetAccumulate(orgtotalsize, sorgaddr.data(), orgcnt, orgtype, targdisp,
                                                 targcnt, targtype, op, win->index);
  }

  int ressize = getDDT()->getType(restype)->getSize(rescnt);
  resddt->serialize((char*)resaddr, msg->getData(), rescnt, ressize, UNPACK);
  delete msg;

  return MPI_SUCCESS;
}

void ampi::winLocalGetAccumulate(int orgtotalsize, char* sorgaddr, int orgcnt, MPI_Datatype orgtype,
                                 MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, MPI_Op op,
                                 char *resaddr, int winIndex) noexcept {
  win_obj *winobj = winObjects[winIndex];
  CkDDT_DataType *tddt = getDDT()->getType(targtype);
  winobj->checkTarget("MPI_Get_accumulate", targdisp, targcnt, tddt);
  char* targaddr = (char*)(winobj->baseAddr) + winobj->disp_unit*targdisp;

  // Pack the original target elements into resaddr (contiguous, the
  // caller unpacks them into the result datatype)
  int targsize = tddt->getSize(targcnt);
  tddt->serialize(targaddr, resaddr, targcnt, targsize, PACK);

  // Accumulate sorgaddr into targaddr
  accumulateIntoTarget(winobj, sorgaddr, targdisp, targcnt, tddt, targtype, op);
}

AmpiMsg* ampi::winRemoteGetAccumulate(int orgtotalsize, char* sorgaddr, int orgcnt, MPI_Datatype orgtype,
                                      MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, MPI_Op op,
                                      int winIndex) noexcept {
  win_obj *winobj = winObjects[winIndex];
  CkDDT_DataType *tddt = getDDT()->getType(targtype);
  int targtotalsize = tddt->getSize(targcnt); // bytes packed into the reply
  winobj->checkTarget("MPI_Get_accumulate", targdisp, targcnt, tddt);
  char* targaddr = (char*)(winobj->baseAddr) + winobj->disp_unit*targdisp;

  // Send back the targaddr buffer before it is accumulated into
  AmpiMsg *msg = new (targtotalsize, 0) AmpiMsg(0, 0, MPI_RMA_TAG, thisIndex, targtotalsize);
  int targsize = getDDT()->getType(targtype)->getSize(targcnt);
  tddt->serialize(targaddr, msg->getData(), targcnt, targsize, PACK);

  // Accumulate sorgaddr into targaddr
  accumulateIntoTarget(winobj, sorgaddr, targdisp, targcnt, tddt, targtype, op);

  return msg;
}

int ampi::winCompareAndSwap(const void *orgaddr, const void *compaddr, void *resaddr, MPI_Datatype type,
                            int rank, MPI_Aint targdisp, WinStruct *win) noexcept {
  // MPI_Compare_and_swap admits only basic contiguous predefined types (see
  // compareAndSwapTypeAllowed), so one element is size plain bytes on both sides.
  int size = getDDT()->getType(type)->getSize(1);

  ampi *destPtr = thisProxy[rank].ckLocal();
  if (destPtr != NULL) {
    destPtr->winLocalCompareAndSwap(size, (char*)orgaddr, (char*)compaddr, (char*)resaddr,
                                    targdisp, win->index);
    return MPI_SUCCESS;
  }

  AmpiMsg* msg = thisProxy[rank].winRemoteCompareAndSwap(size, (char*)orgaddr, (char*)compaddr,
                                                         targdisp, win->index);
  // The reply carries the original target value: copy it out into resaddr.
  memcpy(resaddr, msg->getData(), size);

  delete msg;
  return MPI_SUCCESS;
}

// MPI-3 compare-and-swap on this rank's window: store the original target
// value in resaddr, then, if it equals compaddr (bitwise, all size bytes),
// write sorgaddr into the target. MPI_Compare_and_swap rejects every type
// that is not a basic contiguous predefined type, so each buffer holds one
// element as size contiguous bytes.
void ampi::winLocalCompareAndSwap(int size, char* sorgaddr, char* compaddr, char* resaddr,
                                  MPI_Aint targdisp, int winIndex) noexcept {
  win_obj *winobj = winObjects[winIndex];
  winobj->checkTargetBytes("MPI_Compare_and_swap", targdisp, 0, size);

  char* targaddr = ((char*)(winobj->baseAddr)) + winobj->disp_unit*targdisp;
  bool match = (memcmp(targaddr, compaddr, size) == 0);
  memcpy(resaddr, targaddr, size);
  if (match) {
    memcpy(targaddr, sorgaddr, size);
  }
}

AmpiMsg* ampi::winRemoteCompareAndSwap(int size, char* sorgaddr, char* compaddr, MPI_Aint targdisp,
                                       int winIndex) noexcept {
  AmpiMsg *msg = new (size, 0) AmpiMsg(0, 0, MPI_RMA_TAG, thisIndex, size);
  winLocalCompareAndSwap(size, sorgaddr, compaddr, msg->getData(), targdisp, winIndex);
  return msg;
}

int ampi::winLock(int lock_type, int rank, WinStruct *win) noexcept {
  AMPI_DEBUG("    [%d] Lock: invoke Remote lock at [%d]\n", thisIndex, rank);
  thisProxy[rank].winRemoteLock(lock_type, win->index, thisIndex);
  return MPI_SUCCESS;
}

void ampi::winRemoteLock(int lock_type, int winIndex, int requestRank) noexcept {
  AMPI_DEBUG("    [%d] RemoteLock: invoked \n", thisIndex);
  win_obj *winobj = winObjects[winIndex];

  // check if any one else waiting in the queue
  if(winobj->owner > -1 && !(winobj->emptyQueue()))  {
  // queue it if queue non-empty
    winobj->enqueue(requestRank, lock_type);
    AMPI_DEBUG("    [%d] RemoteLock: queue lock from [%d] \n", thisIndex, requestRank);
  }
  // if queue empty, get semaphore and queue it
  else {
    winobj->lock(requestRank, lock_type);
    winobj->enqueue(requestRank, lock_type);
    AMPI_DEBUG("    [%d] RemoteLock: give lock to [%d] \n", thisIndex, requestRank);
  }
}

int ampi::winUnlock(int rank, WinStruct *win) noexcept {
  AMPI_DEBUG("    [%d] Unlock: invoke Remote lock at [%d]\n", thisIndex, rank);
  thisProxy[rank].winRemoteUnlock(win->index, thisIndex);
  return MPI_SUCCESS;
}

void ampi::winRemoteUnlock(int winIndex, int requestRank) noexcept {
  AMPI_DEBUG("    [%d] RemoteUnlock: invoked \n", thisIndex);
  win_obj *winobj = winObjects[winIndex];
  winobj->unlock(requestRank);
  AMPI_DEBUG("    [%d] RemoteUnlock: [%d] release lock\n", thisIndex, requestRank);

  // if queue non-empty, get lock for the first waiting one and reply
  if(!(winobj->emptyQueue())) {
    AMPI_DEBUG("    [%d] RemoteUnlock: queue non-empty, give lock to \n", thisIndex);
    winobj->lockTopQueue();
  }
}

MPI_Win ampi::createWinInstance(void *base, MPI_Aint size, int disp_unit, MPI_Info info) noexcept {
  AMPI_DEBUG("     Creating win obj {%d, %p}\n ", myComm->getComm(), base);
  win_obj *newobj = new win_obj((char*)(NULL), base, size, disp_unit, myComm->getComm());
  winObjects.push_back(newobj);
  WinStruct *newwin = new WinStruct(myComm->getComm(),winObjects.size()-1);
  AMPI_DEBUG("     Creating MPI_WIN at (%p) with {%d, %ld}\n", &newwin, myComm->getComm(), winObjects.size()-1);
  return (parent->addWinStruct(newwin));
}

int ampi::deleteWinInstance(MPI_Win win) noexcept {
  WinStruct *winStruct = parent->getWinStruct(win);
  win_obj *winobj = winObjects[winStruct->index];
  if (winStruct->ownsMemory) {
    MPI_Free_mem(winobj->baseAddr);
  }
  parent->removeWinStruct(winStruct); // really it does nothing at all
  winobj->free();
  return MPI_SUCCESS;
}

int ampi::winGetGroup(WinStruct *win, MPI_Group *group) const noexcept {
   *group = parent->commGroupHandle(win->comm);
   return MPI_SUCCESS;
}

void ampi::winSetName(WinStruct *win, const char *name) noexcept {
  win_obj *winobj = winObjects[win->index];
  winobj->setName(name);
}

void ampi::winGetName(WinStruct *win, char *name, int *length) const noexcept {
  win_obj *winobj = winObjects[win->index];
  winobj->getName(name, length);
}

win_obj* ampi::getWinObjInstance(WinStruct *win) const noexcept {
  return winObjects[win->index];
}

/*
 * int AMPI_Win_create(void *base, MPI_Aint size, int disp_unit,
 *	       MPI_Info info, MPI_Comm comm, MPI_Win *newwin)
 *   Creates the window object and returns the pointer for *win
 *
 *   ---Assumption: memory location at *base is pre-allocated
 *   ---by a MPI_Alloc_mem call
 *
 *   Inputs:
 *     void *base : pointer specifying the memory area to create the window
 *     MPI_Aint size : size of target memory area (in bytes)
 *     int disp_unit : number of bytes for one datatype
 *     MPI_Info info : MPI_Info object, provides hints for optimization
 *     MPI_Comm comm : communicator
 *     MPI_Win *newwin : stores the handle to the created MPI_Win object on return
 *
 *   Returns int: MPI_SUCCESS or MPI_ERR_WIN
 */
// A collective call over all processes in the communicator
// MPI_Win object created LOCALLY on all processes when the call returns
AMPI_API_IMPL(int, MPI_Win_create, void *base, MPI_Aint size, int disp_unit,
                                   MPI_Info info, MPI_Comm comm, MPI_Win *newwin)
{
  AMPI_API("AMPI_Win_create", base, size, disp_unit, info, comm, newwin);
  ampiParent *parent = getAmpiParent();
  ampi *ptr = getAmpiInstance(comm);
  *newwin = ptr->createWinInstance(base, size, disp_unit, info);
  /* set the builtin attributes on the window */
  WinStruct *winStruct = parent->getWinStruct(*newwin);
  winStruct->base = base;
  winStruct->size = size;
  winStruct->disp_unit = disp_unit;
  ptr = ptr->barrier(); // synchronize all participating virtual processes
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Win_allocate, MPI_Aint size, int disp_unit, MPI_Info info, MPI_Comm comm, void *baseptr, MPI_Win *win)
{
  AMPI_API("AMPI_Win_allocate", size, disp_unit, info, comm, baseptr, win);

  int res = MPI_Alloc_mem(size, info, (void**)baseptr);
  if(res != MPI_SUCCESS)
    return ampiErrhandler("AMPI_Win_allocate", res);

  ampiParent *parent = getAmpiParent();
  ampi *ptr = getAmpiInstance(comm);
  *win = ptr->createWinInstance(*((void**)baseptr), size, disp_unit, info);
  /* set the builtin attributes on the window */
  WinStruct *winStruct = parent->getWinStruct(*win);
  winStruct->base = baseptr;
  winStruct->size = size;
  winStruct->disp_unit = disp_unit;
  winStruct->ownsMemory = true;
  ptr = ptr->barrier(); // synchronize all participating virtual processes
  return MPI_SUCCESS;
}

/*
 * int AMPI_Win_free(MPI_Win *win):
 *   Frees the window object and returns a null pointer for *win
 */
// A collective call over all processes in the communicator
// MPI_Win object deleted LOCALLY on all processes when the call returns
AMPI_API_IMPL(int, MPI_Win_free, MPI_Win *win)
{
  if (win == nullptr)
  {
    AMPI_API("AMPI_Win_free", win);
    return ampiErrhandler("AMPI_Win_free", MPI_ERR_WIN);
  }

  AMPI_API("AMPI_Win_free", win, *win);

  ampiParent *parent = getAmpiParent();
  WinStruct *winStruct = parent->getWinStruct(*win);
  ampi *ptr = getAmpiInstance(winStruct->comm);

  int ret = parent->freeUserAttributes(*win, ptr->getWinObjInstance(winStruct)->getAttributes());
  if (ret != MPI_SUCCESS)
    return ret;

  ptr->deleteWinInstance(*win);
  /* Need a barrier here: to ensure that every process participates */
  ptr = ptr->barrier();
  *win = MPI_WIN_NULL;
  return MPI_SUCCESS;
}

/*
 * ---Note : No atomicity for overlapping Puts.
 * ---sync calls should be made on this window to ensure the
 * ---correctness of the operation
 */
AMPI_API_IMPL(int, MPI_Put, const void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
                            MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, MPI_Win win)
{
  AMPI_API("AMPI_Put", orgaddr, orgcnt, orgtype, rank, targdisp, targcnt, targtype, win);
  if (rank == MPI_PROC_NULL) return MPI_SUCCESS; // MPI-3.1 section 11.3: a no-op
  if (targtype > AMPI_MAX_PREDEFINED_TYPE) {CkAbort("AMPI does not currently support RMA with derived datatypes.");}
  handle_MPI_BOTTOM((void*&)orgaddr, orgtype);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  return ptr->winPut(orgaddr, orgcnt, orgtype, rank, targdisp, targcnt, targtype, winStruct);
}

/*
 * ---Note : No atomicity for overlapping Gets.
 * ---sync calls should be made on this window to ensure the
 * ---correctness of the operation
 */
AMPI_API_IMPL(int, MPI_Get, void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
                            MPI_Aint targdisp, int targcnt, MPI_Datatype targtype,
                            MPI_Win win)
{
  AMPI_API("AMPI_Get", orgaddr, orgcnt, orgtype, rank, targdisp, targcnt, targtype, win);
  if (rank == MPI_PROC_NULL) return MPI_SUCCESS; // MPI-3.1 section 11.3: a no-op
  if (targtype > AMPI_MAX_PREDEFINED_TYPE) {CkAbort("AMPI does not currently support RMA with derived datatypes.");}
  handle_MPI_BOTTOM(orgaddr, orgtype);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  // winGet is a local function which will call the remote method on #rank processor
  return  ptr->winGet(orgaddr, orgcnt, orgtype, rank, targdisp, targcnt, targtype, winStruct);
}

/*
 * int AMPI_Accumulate(void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
 *		   MPI_Aint targdisp, int targcnt, MPI_Datatype targtype,
 *		   MPI_Op op, MPI_Win win)
 *   Accumulates the contents from the origin buffer to the target area using
 *   the predefined op operation.
 *
 * ---Accumulate call is ATOMIC: no sync is needed
 * ---Many accumulate can be made from many origins to one target
 */
AMPI_API_IMPL(int, MPI_Accumulate, const void *orgaddr, int orgcnt, MPI_Datatype orgtype,
                                   int rank, MPI_Aint targdisp, int targcnt,
                                   MPI_Datatype targtype, MPI_Op op, MPI_Win win)
{
  AMPI_API("AMPI_Accumulate", orgaddr, orgcnt, orgtype, rank, targdisp, targcnt, targtype, op, win);
  if (rank == MPI_PROC_NULL) return MPI_SUCCESS; // MPI-3.1 section 11.3: a no-op
  if (targtype > AMPI_MAX_PREDEFINED_TYPE) {CkAbort("AMPI does not currently support RMA with derived datatypes.");}
  handle_MPI_BOTTOM((void*&)orgaddr, orgtype);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  return ptr->winAccumulate(orgaddr, orgcnt, orgtype, rank,
                            targdisp, targcnt, targtype, op, winStruct);
}

/*
 * int AMPI_Get_accumulate(void *orgaddr, int orgcnt, MPI_Datatype orgtype,
 *         void *resaddr, int rescnt, MPI_Datatype restype,
 *         int rank, MPI_Aint targdisp, int targcnt,
 *         MPI_Datatype targtype, MPI_Op op, MPI_Win win)
 *   Perform an atomic, one-sided read-and-accumulate operation.
 */
AMPI_API_IMPL(int, MPI_Get_accumulate, const void *orgaddr, int orgcnt, MPI_Datatype orgtype,
                                       void *resaddr, int rescnt, MPI_Datatype restype,
                                       int rank, MPI_Aint targdisp, int targcnt,
                                       MPI_Datatype targtype, MPI_Op op, MPI_Win win)
{
  AMPI_API("AMPI_Get_accumulate", orgaddr, orgcnt, orgtype, rank, targdisp, targcnt, targtype, op, win);
  if (rank == MPI_PROC_NULL) return MPI_SUCCESS; // MPI-3.1 section 11.3: a no-op
  if (targtype > AMPI_MAX_PREDEFINED_TYPE) {CkAbort("AMPI does not currently support RMA with derived datatypes.");}
  handle_MPI_BOTTOM((void*&)orgaddr, orgtype);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  return ptr->winGetAccumulate(orgaddr, orgcnt, orgtype, resaddr, rescnt, restype,
                               rank, targdisp, targcnt, targtype, op, winStruct);
}

/*
 * int AMPI_Rput(const void *origin_addr, int origin_count, MPI_Datatype
 *         origin_datatype, int target_rank, MPI_Aint target_disp,
 *         int target_count, MPI_Datatype target_datatype, MPI_Win
 *         win, MPI_Request *request)
 *   Put data into a memory window on a remote process and
 *   return a request handle for the operation.
 */
AMPI_API_IMPL(int, MPI_Rput, const void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
                             MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, MPI_Win win,
                             MPI_Request *request)
{
  AMPI_API("AMPI_Rput", orgaddr, orgcnt, orgtype, rank, targdisp, targcnt, targtype, win, request);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  *request = ptr->postReq(getAmpiParent()->reqPool.newReq<SendReq>(orgtype, winStruct->comm, ptr->getDDT(), AMPI_REQ_COMPLETED));
  // The request is complete already: MPI_PROC_NULL makes the call a no-op.
  if (rank == MPI_PROC_NULL) return MPI_SUCCESS; // MPI-3.1 section 11.3: a no-op
  if (targtype > AMPI_MAX_PREDEFINED_TYPE) {CkAbort("AMPI does not currently support RMA with derived datatypes.");}
  return ptr->winPut(orgaddr, orgcnt, orgtype, rank, targdisp, targcnt, targtype, winStruct);
}
/*
 * int AMPI_Rget(void *origin_addr, int origin_count, MPI_Datatype
 *         origin_datatype, int target_rank, MPI_Aint target_disp,
 *         int target_count, MPI_Datatype target_datatype, MPI_Win
 *         win, MPI_Request *request)
 *   Get data from a memory window on a remote process and
 *   return a request handle for the operation.
 */
AMPI_API_IMPL(int, MPI_Rget, void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
                             MPI_Aint targdisp, int targcnt, MPI_Datatype targtype,
                             MPI_Win win, MPI_Request *request)
{
  AMPI_API("AMPI_Rget", orgaddr, orgcnt, orgtype, rank, targdisp, targcnt, targtype, win, request);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  *request = ptr->postReq(getAmpiParent()->reqPool.newReq<SendReq>(orgtype, winStruct->comm, ptr->getDDT(), AMPI_REQ_COMPLETED));
  // The request is complete already: MPI_PROC_NULL makes the call a no-op.
  if (rank == MPI_PROC_NULL) return MPI_SUCCESS; // MPI-3.1 section 11.3: a no-op
  if (targtype > AMPI_MAX_PREDEFINED_TYPE) {CkAbort("AMPI does not currently support RMA with derived datatypes.");}
  return ptr->winGet(orgaddr, orgcnt, orgtype, rank, targdisp, targcnt, targtype, winStruct);
}

/*
 * int AMPI_Raccumulate(const void *origin_addr, int origin_count, MPI_Datatype
 *         origin_datatype, int target_rank, MPI_Aint target_disp,
 *         int target_count, MPI_Datatype target_datatype, MPI_Op op,
 *         MPI_Win win, MPI_Request *request)
 *   Accumulate data into the target process using remote memory access and
 *   return a request handle for the operation.
 */
AMPI_API_IMPL(int, MPI_Raccumulate, const void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
                                    MPI_Aint targdisp, int targcnt, MPI_Datatype targtype,
                                    MPI_Op op, MPI_Win win, MPI_Request *request)
{
  AMPI_API("AMPI_Raccumulate", orgaddr, orgcnt, orgtype, rank, targdisp, targcnt, targtype, op, win, request);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  *request = ptr->postReq(getAmpiParent()->reqPool.newReq<SendReq>(orgtype, winStruct->comm, ptr->getDDT(), AMPI_REQ_COMPLETED));
  // The request is complete already: MPI_PROC_NULL makes the call a no-op.
  if (rank == MPI_PROC_NULL) return MPI_SUCCESS; // MPI-3.1 section 11.3: a no-op
  if (targtype > AMPI_MAX_PREDEFINED_TYPE) {CkAbort("AMPI does not currently support RMA with derived datatypes.");}
  return ptr->winAccumulate(orgaddr, orgcnt, orgtype, rank,
                            targdisp, targcnt, targtype, op, winStruct);
}

/*
 * int AMPI_Rget_accumulate(void *orgaddr, int orgcnt, MPI_Datatype orgtype,
 *         void *resaddr, int rescnt, MPI_Datatype restype,
 *         int rank, MPI_Aint targdisp, int targcnt,
 *         MPI_Datatype targtype, MPI_Op op, MPI_Win win)
 *   Perform an atomic, one-sided read-and-accumulate operation and
 *   return a request handle for the operation.
 */
AMPI_API_IMPL(int, MPI_Rget_accumulate, const void *orgaddr, int orgcnt, MPI_Datatype orgtype,
                                        void *resaddr, int rescnt, MPI_Datatype restype,
                                        int rank, MPI_Aint targdisp, int targcnt,
                                        MPI_Datatype targtype, MPI_Op op, MPI_Win win,
                                        MPI_Request *request)
{
  AMPI_API("AMPI_Rget_accumulate", orgaddr, orgcnt, orgtype, rank, targdisp, targcnt, targtype, op, win, request);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  *request = ptr->postReq(getAmpiParent()->reqPool.newReq<SendReq>(orgtype, winStruct->comm, ptr->getDDT(), AMPI_REQ_COMPLETED));
  // The request is complete already: MPI_PROC_NULL makes the call a no-op.
  if (rank == MPI_PROC_NULL) return MPI_SUCCESS; // MPI-3.1 section 11.3: a no-op
  if (targtype > AMPI_MAX_PREDEFINED_TYPE) {CkAbort("AMPI does not currently support RMA with derived datatypes.");}
  return ptr->winGetAccumulate(orgaddr, orgcnt, orgtype, resaddr, rescnt, restype,
                               rank, targdisp, targcnt, targtype, op, winStruct);
}

// MPI-3.1 section 11.3.4: MPI_Fetch_and_op takes one element of a predefined
// datatype. MPI_LB and MPI_UB are markers, not element types. Whether the
// type is valid for op is left to the reduction code, as for MPI_Accumulate.
static bool fetchAndOpTypeAllowed(MPI_Datatype type) noexcept {
  return type >= 0 && type <= AMPI_MAX_PREDEFINED_TYPE && type != MPI_LB && type != MPI_UB;
}

// MPI-3.1 section 11.3.4: MPI_Compare_and_swap takes one element of a C
// integer, Fortran integer, logical, multi-language or byte datatype. Every
// one of these is a basic, contiguous predefined type in AMPI.
static bool compareAndSwapTypeAllowed(MPI_Datatype type) noexcept {
  switch (type) {
    case MPI_INT: // also MPI_INTEGER
    case MPI_SHORT:
    case MPI_LONG:
    case MPI_LONG_LONG_INT: // also MPI_LONG_LONG, MPI_OFFSET
    case MPI_CHAR: // also MPI_INTEGER1 (Fortran integer)
    case MPI_SIGNED_CHAR:
    case MPI_UNSIGNED_CHAR:
    case MPI_UNSIGNED_SHORT:
    case MPI_UNSIGNED:
    case MPI_UNSIGNED_LONG:
    case MPI_UNSIGNED_LONG_LONG:
    case MPI_INT8_T:
    case MPI_INT16_T:
    case MPI_INT32_T:
    case MPI_INT64_T:
    case MPI_UINT8_T:
    case MPI_UINT16_T:
    case MPI_UINT32_T:
    case MPI_UINT64_T:
    case MPI_LOGICAL:
    case MPI_C_BOOL:
    case MPI_AINT: // also MPI_COUNT
    case MPI_BYTE:
      return true;
    default:
      return false;
  }
}

/*
 * int AMPI_Fetch_and_op(void *orgaddr, void *resaddr, MPI_Datatype type,
 *         int rank, MPI_Aint targdisp, MPI_Op op, MPI_Win win)
 *   Perform one-sided read-modify-write.
 */
AMPI_API_IMPL(int, MPI_Fetch_and_op, const void *orgaddr, void *resaddr, MPI_Datatype type,
                                     int rank, MPI_Aint targdisp, MPI_Op op, MPI_Win win)
{
  AMPI_API("AMPI_Fetch_and_op", orgaddr, resaddr, type, rank, targdisp, op, win);
  if (!fetchAndOpTypeAllowed(type)) {
    return ampiErrhandler("AMPI_Fetch_and_op", MPI_ERR_TYPE);
  }
  if (rank == MPI_PROC_NULL) return MPI_SUCCESS; // MPI-3.1 section 11.3: a no-op
  handle_MPI_BOTTOM((void*&)orgaddr, type);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  // HACK: use GetAccumulate for FetchAndOp
  return ptr->winGetAccumulate(orgaddr, 1, type, resaddr, 1, type,
                               rank, targdisp, 1, type, op, winStruct);
}

/*
 * int AMPI_Compare_and_swap(void *orgaddr, void *compaddr, void *resaddr,
 *         MPI_Datatype type, int rank, MPI_Aint targdisp, MPI_Win win)
 *   Perform one-sided atomic compare-and-swap.
 */
AMPI_API_IMPL(int, MPI_Compare_and_swap, const void *orgaddr, const void *compaddr, void *resaddr,
                                         MPI_Datatype type, int rank, MPI_Aint targdisp, MPI_Win win)
{
  AMPI_API("AMPI_Compare_and_swap", orgaddr, compaddr, resaddr, type, rank, targdisp, win);
  if (!compareAndSwapTypeAllowed(type)) {
    return ampiErrhandler("AMPI_Compare_and_swap", MPI_ERR_TYPE);
  }
  if (rank == MPI_PROC_NULL) return MPI_SUCCESS; // MPI-3.1 section 11.3: a no-op
  handle_MPI_BOTTOM((void*&)orgaddr, type);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  return ptr->winCompareAndSwap(orgaddr, compaddr, resaddr, type, rank, targdisp, winStruct);
}

/*
 * int AMPI_Win_fence(int assertion, MPI_Win win)
 *   Synchronizes all one-sided communication calls on this MPI_Win.
 *   (Synchronized RMA operations on the specified window)
 *
 *   Inputs:
 *     int assertion : program assertion, used to provide optimization hints
 *   Returns int : MPI_SUCCESS or MPI_ERR_WIN
 */
AMPI_API_IMPL(int, MPI_Win_fence, int assertion, MPI_Win win)
{
  AMPI_API("AMPI_Win_fence", assertion, win);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  MPI_Comm comm = winStruct->comm;

  // Wait until everyone reaches the fence
  ampi *unused = getAmpiInstance(comm)->barrier();

  // Complete all outstanding one-sided comm requests
  // no need to do this for the pseudo-implementation
  return MPI_SUCCESS;
}

/*
 * int AMPI_Win_lock(int lock_type, int rank, int assertion, MPI_Win win)
 *   Locks access to this MPI_Win object.
 *   Input:
 *     int lock_type : MPI_LOCK_EXCLUSIVE or MPI_LOCK_SHARED
 *     int rank : rank of locked window
 *     int assertion : program assertion, used to provide optimization hints
 *   Returns int : MPI_SUCCESS or MPI_ERR_WIN
 */
AMPI_API_IMPL(int, MPI_Win_lock, int lock_type, int rank, int assertion, MPI_Win win)
{
  AMPI_API("AMPI_Win_lock", lock_type, rank, assertion, win);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);

  // process assertion here:
  // end of assertion
  ptr->winLock(lock_type, rank, winStruct);
  return MPI_SUCCESS;
}

/*
 * int AMPI_Win_unlock(int rank, MPI_Win win)
 *   Unlocks access to this MPI_Win object.
 *   Input:
 *     int rank : rank of locked window
 *   Returns int : MPI_SUCCESS or MPI_ERR_WIN
 */
// The RMA call is completed both locally and remotely after unlock.
  // process assertion here: HOW???
AMPI_API_IMPL(int, MPI_Win_unlock, int rank, MPI_Win win)
{
  AMPI_API("AMPI_Win_unlock", rank, win);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);

  // process assertion here: HOW???
  // end of assertion
  ptr->winUnlock(rank, winStruct);
  return MPI_SUCCESS;
}

/*
 * int AMPI_Win_lock_all(int assert, MPI_Win win)
 *   Locks access to this MPI_Win object for all ranks.
 *   Input:
 *     int assertion : program assertion, used to provide optimization hints
 *   Returns int : MPI_SUCCESS or MPI_ERR_WIN
 */
AMPI_API_IMPL(int, MPI_Win_lock_all, int assert, MPI_Win win)
{
  AMPI_API("AMPI_Win_lock_all", assert, win);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  int size = ptr->getSize();

  // TODO: optimize for assertions here

  for(int i=0; i<size; i++) {
    ptr->winLock(MPI_LOCK_SHARED, i, winStruct);
  }
  return MPI_SUCCESS;
}

/*
 * int AMPI_Win_unlock_all(MPI_Win win)
 *   Unlocks access to this MPI_Win object for all ranks.
 *   Input:
 *   Returns int : MPI_SUCCESS or MPI_ERR_WIN
 */
// The RMA call is completed both locally and remotely after unlock.
AMPI_API_IMPL(int, MPI_Win_unlock_all, MPI_Win win)
{
  AMPI_API("AMPI_Win_unlock_all", win);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  int size = ptr->getSize();

  for(int i=0; i<size; i++) {
    ptr->winUnlock(i, winStruct);
  }
  return MPI_SUCCESS;
}

/* the following four functions are yet to implement */
/*
 * int AMPI_Win_post(MPI_Group group, int assertion, MPI_Win win)
 *   Opens a RMA access epoch for local window win.
 *   Only processes in group can access this window with RMA calls.
 *   Each process must issue a matching MPI_Win_start to start the
 *     access epoch.
 *   Post is non-blocking while start could be blocking.
 *   Input:
 *     MPI_Group group : a group of processes
 *   Returns int : MPI_SUCCESS or MPI_ERR_WIN
 */
AMPI_API_IMPL(int, MPI_Win_post, MPI_Group group, int assertion, MPI_Win win)
{
  AMPI_API("AMPI_Win_post", group, assertion, win);

  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  if (winStruct->isInEpoch()) {
    return ampiErrhandler("AMPI_Win_post", MPI_ERR_RMA_SYNC);
  } else {
    winStruct->setInEpoch(true);
  }

  int parentGroupSize;
  MPI_Group parentGroup;
  parentGroup = getAmpiParent()->comm2group(winStruct->comm); // internal: no reference to free
  MPI_Group_size(parentGroup, &parentGroupSize);

  std::vector<int> parentGroupRanks(parentGroupSize);
  std::vector<int> subsetGroupRanks(parentGroupSize);
  for (int i=0; i<parentGroupSize; i++) {
    parentGroupRanks[i] = i;
  }
  MPI_Group_translate_ranks(parentGroup, parentGroupSize, parentGroupRanks.data(), group, subsetGroupRanks.data());

  ampi *ptr = getAmpiInstance(winStruct->comm);
  int actualRanks = 0;
  for (int i=0; i<subsetGroupRanks.size(); i++) { // If subsetGroupRanks is large, multicast would be more efficient
    if (subsetGroupRanks[i] != MPI_UNDEFINED) {
      subsetGroupRanks[actualRanks++] = i;
      ptr->send(MPI_EPOCH_START_TAG, ptr->getRank(), NULL, 0, MPI_INT, subsetGroupRanks[actualRanks-1], winStruct->comm);
    }
  }

  for (int i=actualRanks; i<subsetGroupRanks.size(); i++) {
    subsetGroupRanks.pop_back();
  }
  winStruct->setExposureRankList(subsetGroupRanks);
  std::vector<MPI_Request> &reqList = winStruct->getRequestList();
  reqList.resize(subsetGroupRanks.size()); // for use in AMPI_Win_{test,wait}

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Win_wait, MPI_Win win)
{
  AMPI_API("AMPI_Win_wait", win);

  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  if (!winStruct->isInEpoch()) {
    return ampiErrhandler("AMPI_Win_wait", MPI_ERR_RMA_SYNC);
  }

  ampi* ptr = getAmpiInstance(winStruct->comm);
  const std::vector<int> &exposureRankList = winStruct->getExposureRankList();
  std::vector<MPI_Request> &requestList = winStruct->getRequestList();

  // If nonblocking recvs have been posted already, there is no reason to post duplicates
  if (!winStruct->AreRecvsPosted()) {
    for (int i=0; i<exposureRankList.size(); i++) {
      ptr->irecv(NULL, 0, MPI_INT, exposureRankList[i], MPI_EPOCH_END_TAG, winStruct->comm, &requestList[i]);
    }
  }

  MPI_Waitall(requestList.size(), requestList.data(), MPI_STATUSES_IGNORE);
  winStruct->clearEpochExposure();

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Win_start, MPI_Group group, int assertion, MPI_Win win)
{
  AMPI_API("AMPI_Win_start", group, assertion, win);

  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  if (winStruct->isInEpoch()) {
    return ampiErrhandler("AMPI_Win_start", MPI_ERR_RMA_SYNC);
  } else {
    winStruct->setInEpoch(true);
  }

  int parentGroupSize;
  MPI_Group parentGroup;
  parentGroup = getAmpiParent()->comm2group(winStruct->comm); // internal: no reference to free
  MPI_Group_size(parentGroup, &parentGroupSize);

  std::vector<int> subsetGroupRanks(parentGroupSize);
  std::vector<int> parentGroupRanks(parentGroupSize);
  for (int i=0; i<parentGroupSize; i++) {
    parentGroupRanks[i] = i;
  }

  MPI_Group_translate_ranks(parentGroup, parentGroupSize, parentGroupRanks.data(), group, subsetGroupRanks.data());

  ampi *ptr = getAmpiInstance(winStruct->comm);
  int actualRanks = 0;
  for (int i=0; i<subsetGroupRanks.size(); i++) {
    if (subsetGroupRanks[i] != MPI_UNDEFINED) {
      subsetGroupRanks[actualRanks++] = i;
      ptr->recv(MPI_EPOCH_START_TAG, subsetGroupRanks[actualRanks-1], NULL, 0, MPI_INT, winStruct->comm, MPI_STATUS_IGNORE);
    }
  }

  for (int i=actualRanks; i<subsetGroupRanks.size(); i++) {
    subsetGroupRanks.pop_back();
  }
  winStruct->setAccessRankList(subsetGroupRanks);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Win_complete, MPI_Win win)
{
  AMPI_API("AMPI_Win_complete", win);

  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  if (!winStruct->isInEpoch()) {
    return ampiErrhandler("AMPI_Win_complete", MPI_ERR_RMA_SYNC);
  } else {
    winStruct->setInEpoch(true);
  }

  std::vector<int> &accessGroupRanks = winStruct->getAccessRankList();
  ampi *ptr = getAmpiInstance(winStruct->comm);

  for (int i=0; i<accessGroupRanks.size(); i++) {
    ptr->send(MPI_EPOCH_END_TAG, ptr->getRank(), NULL, 0, MPI_INT, accessGroupRanks[i], winStruct->comm);
  }
  winStruct->clearEpochAccess();

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Win_test, MPI_Win win, int *flag)
{
  AMPI_API("AMPI_Win_test", win, flag);

  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  if (!winStruct->isInEpoch()) {
    return ampiErrhandler("AMPI_Win_test", MPI_ERR_RMA_SYNC);
  }

  std::vector<MPI_Request> &reqList = winStruct->getRequestList();
  const std::vector<int> &exposureRankList = winStruct->getExposureRankList();
  ampi* ptr = getAmpiInstance(winStruct->comm);

  // If nonblocking recvs have been posted already, there is no reason to post duplicates
  if (!winStruct->AreRecvsPosted()) {
    for (int i=0; i<reqList.size(); i++) {
      ptr->irecv(NULL, 0, MPI_INT, exposureRankList[i], MPI_EPOCH_END_TAG, winStruct->comm, reqList.data());
    }
    winStruct->setAreRecvsPosted(true);
  }

  MPI_Testall(reqList.size(), reqList.data(), flag, MPI_STATUSES_IGNORE);
  if (*flag) {
    winStruct->clearEpochExposure();
  }

  return MPI_SUCCESS;
}

// FIX PLACE II
CLINKAGE
int AMPI_Iget(MPI_Aint orgdisp, int orgcnt, MPI_Datatype orgtype, int rank,
              MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, MPI_Win win,
              MPI_Request *request) {
  AMPI_API("AMPI_Iget", orgdisp, orgcnt, orgtype, rank, targdisp, targcnt, targtype, win, request);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  // winGet is a local function which will call the remote method on #rank processor
  return  ptr->winIget(orgdisp, orgcnt, orgtype, rank, targdisp, targcnt, targtype, winStruct,
		       request);
}

CLINKAGE
int AMPI_Iget_wait(MPI_Request *request, MPI_Status *status, MPI_Win win) {
  AMPI_API("AMPI_Iget_wait", request, status, win);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  // winGet is a local function which will call the remote method on #rank processor
  return  ptr->winIgetWait(request,status);
}

CLINKAGE
int AMPI_Iget_free(MPI_Request *request, MPI_Status *status, MPI_Win win) {
  AMPI_API("AMPI_Iget_free", request, status, win);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  // winGet is a local function which will call the remote method on #rank processor
  return  ptr->winIgetFree(request, status);
}

CLINKAGE
int AMPI_Iget_data(void *data, MPI_Status status) {
  *((char**)data) = /*(void*)*/((AmpiMsg*)status.msg)->data;
  return MPI_SUCCESS;
}

/*
 * int AMPI_Alloc_mem(MPI_Aint size, MPI_Info info, void *baseptr)
 *   A simple wrapper around 'malloc' call. Used to allocate memory
 *   for MPI functions
 *
 *   Inputs:
 *     MPI_Aint size : size of target memory area
 *     MPI_Info info :
 *     void *base : stores the base pointer of target memory area on return
 *   Return:
 *     void* : address of the allocated memory
 */
AMPI_API_IMPL(int, MPI_Alloc_mem, MPI_Aint size, MPI_Info info, void *baseptr)
{
  //NOTE: do not use AMPI_API() here, so that the memory allocated is migratable!
  *(void **) baseptr = malloc(size);
  if(*(void **) baseptr == nullptr)
    return ampiErrhandler("AMPI_Alloc_mem", MPI_ERR_NO_MEM);
  return MPI_SUCCESS;
}

/*
 * int AMPI_Free_mem(void *base)
 *   Frees memory that was previous allocated by MPI_Alloc_mem call
 */
AMPI_API_IMPL(int, MPI_Free_mem, void *baseptr)
{
  //NOTE: do not use AMPI_API() here, since the memory being freed is migratable!
  free(baseptr);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Win_get_group, MPI_Win win, MPI_Group *group)
{
  AMPI_API("AMPI_Win_get_group", win, group);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  ptr->winGetGroup(winStruct, group);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Win_delete_attr, MPI_Win win, int key)
{
  AMPI_API("AMPI_Win_delete_attr", win, key);
  ampiParent *parent = getAmpiParent();
  WinStruct *winStruct = parent->getWinStruct(win);
  auto & attributes = getAmpiInstance(winStruct->comm)->getWinObjInstance(winStruct)->getAttributes();
  return parent->deleteAttr(win, attributes, key);
}

AMPI_API_IMPL(int, MPI_Win_get_attr, MPI_Win win, int key, void* value, int* flag)
{
  AMPI_API("AMPI_Win_get_attr", win, key, value, flag);
  ampiParent *parent = getAmpiParent();
  WinStruct *winStruct = parent->getWinStruct(win);
  auto & attributes = getAmpiInstance(winStruct->comm)->getWinObjInstance(winStruct)->getAttributes();
  return parent->getAttrWin(win, attributes, key, value, flag, winStruct);
}

AMPI_API_IMPL(int, MPI_Win_set_attr, MPI_Win win, int key, void* value)
{
  AMPI_API("AMPI_Win_set_attr", win, key, value);
  ampiParent *parent = getAmpiParent();
  WinStruct *winStruct = parent->getWinStruct(win);
  auto & attributes = getAmpiInstance(winStruct->comm)->getWinObjInstance(winStruct)->getAttributes();
  return parent->setAttrWin(win, attributes, key, value);
}

AMPI_API_IMPL(int, MPI_Win_set_name, MPI_Win win, const char *name)
{
  AMPI_API("AMPI_Win_set_name", win, name);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  ptr->winSetName(winStruct, name);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Win_set_info, MPI_Win win, MPI_Info info)
{
  AMPI_API("AMPI_Win_set_info", win, info);
  /* FIXME: no-op implementation */
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Win_get_info, MPI_Win win, MPI_Info *info)
{
  AMPI_API("AMPI_Win_get_info", win, info);
  /* FIXME: no-op implementation */
  *info = MPI_INFO_NULL;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Win_create_errhandler, MPI_Win_errhandler_function *win_errhandler_fn,
                                              MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_Win_create_errhandler", win_errhandler_fn, errhandler);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Win_call_errhandler, MPI_Win win, int errorcode)
{
  AMPI_API("AMPI_Win_call_errhandler", win, errorcode);
  CkPrintf("WARNING: AMPI does not support MPI_Win_call_errhandler (errorcode = %d)\n", errorcode);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Win_get_errhandler, MPI_Win win, MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_Win_get_errhandler", win, errhandler);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Win_set_errhandler, MPI_Win win, MPI_Errhandler errhandler)
{
  AMPI_API("AMPI_Win_set_errhandler", win, errhandler);
  return MPI_SUCCESS;
}

int MPI_win_null_copy_fn(MPI_Win win, int keyval, void *extra_state,
                         void *attr_in, void *attr_out, int *flag){
  (*flag) = 0;
  return MPI_SUCCESS;
}

int MPI_win_dup_fn(MPI_Win win, int keyval, void *extra_state,
                   void *attr_in, void *attr_out, int *flag){
  (*(void **)attr_out) = attr_in;
  (*flag) = 1;
  return MPI_SUCCESS;
}

int MPI_win_null_delete_fn(MPI_Win win, int keyval, void *attr, void *extra_state){
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Win_create_keyval, MPI_Win_copy_attr_function *copy_fn,
                                          MPI_Win_delete_attr_function *delete_fn,
                                          int *keyval, void *extra_state)
{
  AMPI_API("AMPI_Win_create_keyval", copy_fn, delete_fn, keyval, extra_state);
  return MPI_Comm_create_keyval(copy_fn, delete_fn, keyval, extra_state);
}

AMPI_API_IMPL(int, MPI_Win_free_keyval, int *keyval)
{
  AMPI_API("AMPI_Win_free_keyval", keyval, *keyval);
  return MPI_Comm_free_keyval(keyval);
}

AMPI_API_IMPL(int, MPI_Win_get_name, MPI_Win win, char *name, int *length)
{
  AMPI_API("AMPI_Win_get_name", win, name, length);
  WinStruct *winStruct = getAmpiParent()->getWinStruct(win);
  ampi *ptr = getAmpiInstance(winStruct->comm);
  ptr->winGetName(winStruct, name, length);
  return MPI_SUCCESS;
}
