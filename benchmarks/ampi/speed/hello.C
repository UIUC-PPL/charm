/*
   msg_ comm program to test out speed of Charm++
   messaging layers.
   */
#include "hello.decl.h"
#include "mpi.h"
#include "msgspeed.h"
#include "tcharmc.h"
#include <stdio.h>

/*readonly*/ CProxy_Main mainProxy;
/*readonly*/ int verbose = 0;

void emptyCommTest(void);
void convCommTest(int isLocal);
void startMPItests(int isLocal);
void startMPItestsLocal(void);
void startMPItestsRemote(void);

int startMPItestsLocal_idx = -1;
int startMPItestsRemote_idx = -1;

/*mainchare*/
class Main : public Chare
{
  int state;
  CProxy_Hello arr;

public:
  Main(CkArgMsg* m)
  {
    if (m->argc > 1) verbose = atoi(m->argv[1]);
    delete m;

    mainProxy = thishandle;

    state = 1;
    mainProxy.done();
  };

  void done(void)
  {
    // CkPrintf("Beginning test %d\n",state);
    switch (state++)
    {
      case 1:
        emptyCommTest();
        done();
        break;

      case 2:
        CkPrintf("----- Local machine (everything on 1 processor) -----\n");
        convCommTest(1);
        break;

      case 3:
      {
        /* make two array elements on the same processor */
        arr = CProxy_Hello::ckNew();
        int onPe = CkMyPe();
        arr[0].insert(onPe);
        arr[1].insert(onPe);
        arr.doneInserting();
        arr.start(0);
      }
      break;

      case 4:
        arr.start(1);
        break;
      case 5:
        TCHARM_Create(2, startMPItestsLocal_idx);
        break;
      case 6:
        if (CkNumPes() < 2)
        {
          CkPrintf("Tests Complete (run with +p2 for remote tests)\n");
          CkExit();
        }
        CkPrintf("----- Remote machine (between 2 processors) -----\n");
        convCommTest(0);
        break;
      case 7: /* make two array elements on *different* processors */
        arr = CProxy_Hello::ckNew(2);
        arr.start(0);
        break;
      case 8:
        arr.start(1);
        break;
      case 9:
        TCHARM_Create(2, startMPItestsRemote_idx);
        break;
      case 10:
        CkPrintf("Tests Complete\n");
        CkExit();
    }
  };

  static void conv_init(void);
};

/*********************** Empty comm *********************/
static void empty_send_fn(void* data, size_t len, int dest, msg_comm* comm)
{
  msg_send_complete(comm, data, len);
}
static void empty_recv_fn(void* data, size_t len, int src, msg_comm* comm)
{
  msg_recv_complete(comm, data, len);
}
static void empty_finish_fn(msg_comm* comm) {}

void emptyCommTest(void)
{
  msg_comm m;
  m.send_fn = empty_send_fn;
  m.recv_fn = empty_recv_fn;
  m.finish_fn = empty_finish_fn;
  msg_comm_test(&m, "Empty", 0, verbose);
}

/*********************** Converse *********************/
static void conv_kicker(void* startMsg);
CpvDeclare(int, conv_kicker_idx);

/** Called on every processor at startup time */
void Main::conv_init(void)
{
  CpvInitialize(int, conv_kicker_idx);
  CpvAccess(conv_kicker_idx) = CmiRegisterHandler((CmiHandler)conv_kicker);
}

struct conv_start_msg
{
  char hdr[CmiMsgHeaderSizeBytes];
  int isLocal;
};

/** Called on processor 0 to initiate the test */
void convCommTest(int isLocal)
{
  conv_start_msg m;
  CmiInitMsgHeader(m.hdr, sizeof(struct conv_start_msg));
  CmiSetHandler(&m, CpvAccess(conv_kicker_idx));
  m.isLocal = isLocal;
  CmiSyncBroadcastAllFn(sizeof(m), (char*)&m);
}

/* Basic communications: */
struct conv_msg_header
{
  char conv_hdr[CmiMsgHeaderSizeBytes];
  size_t len;
  double data;
  /* user data goes here */
};
struct conv_msg_comm : public msg_comm
{
  /* Converse handler index that will receive our messages */
  int send_idx;
  /* Converse PE number that will receive our messages */
  int send_pe;
  int master; /* marker: I'm responsible for saying it's over */
};

static void conv_send_fn(void* data, size_t len, int dest, conv_msg_comm* comm)
{
  if (verbose >= 8) CmiPrintf("Processor %d send\n", CmiMyPe());
  int mlen = sizeof(conv_msg_header) + len;
  conv_msg_header* m = (conv_msg_header*)CmiAlloc(mlen);
  m->len = len;
  memcpy(&m->data, data, len);
  CmiSetHandler(m, comm->send_idx);
  CmiSyncSendAndFree(comm->send_pe, mlen, (char*)m);
  msg_send_complete(comm, data, len);
}
static void conv_recv_fn(void* data, size_t len, int src, msg_comm* comm) { /* ignored */ }
static void conv_recv(conv_msg_header* m, conv_msg_comm* comm)
{
  if (verbose >= 8) CmiPrintf("Processor %d recv\n", CmiMyPe());
  msg_recv_complete(comm, &m->data, m->len);
  CmiFree(m);
}

static void conv_finish_fn(conv_msg_comm* comm)
{
  if (comm->master) mainProxy.done();
}

conv_msg_comm* makeComm(void)
{
  conv_msg_comm* comm = (conv_msg_comm*)malloc(sizeof(conv_msg_comm));
  comm->send_fn = (msg_send_fn)conv_send_fn;
  comm->recv_fn = conv_recv_fn;
  comm->finish_fn = (msg_finish_fn)conv_finish_fn;
  return comm;
}

/** Called on every processor to initiate the test */
static void conv_kicker(void* startMsg)
{
  int isLocal = ((conv_start_msg*)startMsg)->isLocal;
  CmiFree(startMsg);
  conv_msg_comm* comm = makeComm();
  conv_msg_comm* comm2 = NULL;
  if (isLocal)
  { /* Spawn off a separate copy locally */
    comm2 = makeComm();
    comm2->send_idx = CmiRegisterHandlerEx((CmiHandlerEx)conv_recv, comm);
    comm2->send_pe = 0;
    comm2->master = 0;
    comm->send_idx = CmiRegisterHandlerEx((CmiHandlerEx)conv_recv, comm2);
    comm->send_pe = 0;
    comm->master = 1;
    if (CmiMyPe() != 0) return; /* register only-- don't start test */
  }
  else
  { /* Really talking over the network: */
    comm->send_idx = CmiRegisterHandlerEx((CmiHandlerEx)conv_recv, comm);
    comm->send_pe = !CmiMyPe();
    comm->master = CmiMyPe() == 0;
  }
  if (verbose >= 8)
  {
    CmiPrintf("Processor %d ready (hdlr %d, pe %d)\n", CmiMyPe(), comm->send_idx,
              comm->send_pe);
  }
  msg_comm_test(comm, "Converse", CmiMyPe(), verbose);
  if (comm2) msg_comm_test(comm2, "Converse", 1, verbose);
}

/*********************** Array *********************/
class myMsg : public CMessage_myMsg
{
public:
  size_t len;
  char* data;
};

class helloComm : public msg_comm
{
public:
  Hello* h;
  CProxy_Hello hp;
};

/*array [1D]*/
class Hello : public CBase_Hello
{
  helloComm marshal;
  helloComm message;

public:
  Hello();
  Hello(CkMigrateMessage* m) {}

  void start(int testNo)
  {
    if (testNo == 0)
      msg_comm_test(&message, "Message  1D", thisIndex, verbose);
    else
      msg_comm_test(&marshal, "Marshall 1D", thisIndex, verbose);
  }

  void recvMarshal(int n, const char* data)
  {
    msg_recv_complete(&marshal, (void*)data, n);
  }
  void recvMessage(myMsg* m)
  {
    msg_recv_complete(&message, (void*)m->data, m->len);
    delete m;
  }
};

static void marshal_send_fn(void* data, size_t len, int dest, msg_comm* comm)
{
  ((helloComm*)comm)->hp[dest].recvMarshal(len, (char*)data);
  msg_send_complete(comm, data, len);
}

static void message_send_fn(void* data, size_t len, int dest, msg_comm* comm)
{
  myMsg* m = new (&len, 0) myMsg;
  m->len = len;
  memcpy(m->data, data, len);
  ((helloComm*)comm)->hp[dest].recvMessage(m);
  msg_send_complete(comm, data, len);
}

static void ignore_recv_fn(void* data, size_t len, int dest, msg_comm* comm)
{
  /* Charm decides when *it* wants you to recv, so this is useless */
}

static void array_finish_fn(msg_comm* comm)
{
  ((helloComm*)comm)
      ->h->contribute(0, 0, CkReduction::sum_int,
                      CkCallback(CkIndex_Main::done(), mainProxy));
}

Hello::Hello()
{
  marshal.h = this;
  marshal.hp = thisProxy;
  marshal.send_fn = marshal_send_fn;
  marshal.recv_fn = ignore_recv_fn;
  marshal.finish_fn = array_finish_fn;

  message.h = this;
  message.hp = thisProxy;
  message.send_fn = message_send_fn;
  message.recv_fn = ignore_recv_fn;
  message.finish_fn = array_finish_fn;
}

/*********************** MPI *********************/

extern "C" void startMPItest(MPI_Comm comm, int verbose);

void startMPItests(int isLocal)
{
  int argc = 0;
  char* argv0 = NULL;
  char** argv = &argv0;
  MPI_Init(&argc, &argv);
  MPI_Comm comm = MPI_COMM_WORLD;
  int myRank;
  MPI_Comm_rank(comm, &myRank);

  if (isLocal)
  { /* Migrate both threads to physical processor 0 */
    TCHARM_Migrate_to(0);
  }
  else
  {
    /* don't do any migration-- leave threads where they are */
  }

  MPI_Barrier(comm);
  startMPItest(comm, verbose);
  MPI_Barrier(comm);

  if (myRank == 0)
  {
    // CkPrintf("MPI Test Complete\n");
    mainProxy.done();
  }
}
void startMPItestsLocal(void) { startMPItests(1); }
void startMPItestsRemote(void) { startMPItests(0); }

static void nodeInit()
{
  startMPItestsLocal_idx =
      TCHARM_Register_thread_function((TCHARM_Thread_data_start_fn)startMPItestsLocal);
  startMPItestsRemote_idx =
      TCHARM_Register_thread_function((TCHARM_Thread_data_start_fn)startMPItestsRemote);
}

#include "hello.def.h"
