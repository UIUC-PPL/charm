/**
  Framework for measuring message-passing speed.
  Orion Sky Lawlor, olawlor@uiuc.edu, 2003/8/21
  */
#include "msgspeed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CD comm->driver

/***** Timing Routine (gettimeofday) ******/
#ifdef __CHARMC__
#include "converse.h"

/* Use fast, accurate charm clock */
double msg_timer(void) { return CmiWallTimer(); }

#else
/* Portable, but microsecond resolution "gettimeofday" */

#include <sys/resource.h>
#include <sys/time.h>

double msg_timer(void)
{
  struct timeval tv;
  gettimeofday(&tv, 0);
  return (tv.tv_sec * 1.0) + (tv.tv_usec * 0.000001);
}

#endif

/********************* Driver Utilities ***************************/
#define MAX_RUNS 100
typedef struct msg_test_runs msg_test_runs;
struct msg_test_runs
{
  int current, last;               /* run counter */
  double start;                    /* start of this run */
  double times[MAX_RUNS];          /* elapsed times for each run */
  double minTime, median, maxTime; /* Time bounds for run */
};

/* Routine to start a test running */
typedef void (*msg_test_fn)(msg_comm* comm);

typedef struct msg_test msg_test;
struct msg_test
{
  msg_test_fn fn;   /* Current test function */
  const char* name; /* Human-readable name of test (NULL to hide) */
  double overhead;  /* Timing correction to subtract off each run. */
  int msgs, bytes;  /* Number of messages, bytes (used for printouts) */
};

struct msg_driver
{
  const char* desc;
  int myPe;
  int verbose;

  /* Testing runtime: */
  /** Current test we're running */
  int testNo;
  msg_test test;

  /** Data for each run (repetition) of the test */
  msg_test_runs run;

  /** Number of outstanding sends/recvs
    (only nonzero during a run). */
  int nSends, nRecvs;

  /** Routine to execute next (instead of next run) */
  msg_test_fn continuation;

  /** Counter used inside test (somehow) */
  int counter;

  /** Flags to prevent re-entrant driver calls */
  int inComm;

  /** Big buffer of data to send off as messages */
  void* msgBuf; /* FIXME: actually keep track of used bytes here */

  /** To frighten compiler's optimizer away from the send/recv copy loops. */
  double dont_optimize_away;

  /* Derived Quantities: */
  /** Time for empty task (seconds) */
  double timerOverhead;

  /** One-way total time per short message (seconds) */
  double oneWay;

  /** Transmission time per byte, with idle and two-way link  */
  double perByte, perByteBusy;
};

/*** State tracking ****/

/** Print out this test's time */
static void print_time(msg_test* test, double elapsed)
{
  if (test->bytes > 0) /* Test measures bytes-- estimate bandwidth */
    printf("%.1f MB/s", 1.0e-6 * test->bytes / elapsed);
  else /* taskBytes invalid-- try messages */
  {
    if (test->msgs > 0) /* Test measures messages-- estimate time/msg */
      printf("%.2f us/msg", 1.0e6 * elapsed / test->msgs);
    else /* no bytes or messages-- just print time */
      printf("%.2f us", 1.0e6 * elapsed);
  }
}

static int compare_double(const void* vl, const void* vr)
{
  double l = *(double*)vl, r = *(double*)vr;
  if (l < r) return -1;
  if (l > r) return 1;
  return 0;
}

/** Print out the results of the last-run test */
static void msg_test_finish(msg_comm* comm)
{
  /* Sort times as ascending. */
  qsort(CD->run.times, CD->run.last, sizeof(CD->run.times[0]), compare_double);
  CD->run.minTime = CD->run.times[CD->run.last / 10];
  CD->run.median = CD->run.times[CD->run.last / 2];
  CD->run.maxTime = CD->run.times[CD->run.last - 1 - CD->run.last / 10];

  if (CD->verbose >= 1 && CD->test.name && CD->myPe == 0)
  {
    printf("    %s %s: ", CD->desc, CD->test.name);
    print_time(&CD->test, CD->run.median);
    if (CD->run.minTime < 0.8 * CD->run.median)
    {
      printf(" [ Best: ");
      print_time(&CD->test, CD->run.minTime);
      printf(" ]");
    }
    if (CD->run.maxTime > 2.0 * CD->run.median)
    {
      printf(" [ Worst: ");
      print_time(&CD->test, CD->run.maxTime);
      printf(" ]");
    }
    printf("\n");
  }
}

/** Begin the next run of the current test. */
static void msg_run_start(msg_comm* comm)
{
  if (CD->verbose >= 5)
    printf("msg_comm_test(%d): starting run %d of test %s\n", CD->myPe, CD->run.current,
           CD->test.name);
  CD->run.start = msg_timer();
  (CD->test.fn)(comm);
}

/** Finish the current run. */
static void msg_run_finish(msg_comm* comm)
{
  double elapsed = msg_timer() - CD->run.start - CD->test.overhead;
  CD->run.times[CD->run.current] = elapsed;
  CD->run.current++;
}

static void msg_test_next(msg_comm* comm);

/** Called repeatedly whenever the previous run finishes. */
static void msg_driver_next_state(msg_comm* comm)
{
  if (CD->continuation)
  {
    msg_test_fn fn = CD->continuation;
    CD->continuation = NULL;
    (fn)(comm);
  }
  else if (CD->run.current < CD->run.last)
  { /* start the next run of the current test */
    msg_run_finish(comm);
    msg_run_start(comm);
  }
  else if (CD->run.current >= CD->run.last)
  { /* this test is over-- move on to the next test */
    msg_run_finish(comm);
    msg_test_finish(comm);
    msg_test_next(comm);
  }
}

/**
  Run the message driver state machine.
  */
static void msg_driver_state(msg_comm* comm)
{
  /* don't re-enter the driver */
  if (CD->inComm != 0) return;
  CD->inComm = 1;

  /* don't start the next state with messages in flight.
   */
  while (CD->nSends == 0 && CD->nRecvs == 0) msg_driver_next_state(comm);

  if (CD->verbose >= 7)
    printf("msg_comm_test(%d): blocking during test %s (%d send, %d recv)\n", CD->myPe,
           CD->test.name, CD->nSends, CD->nRecvs);
  CD->inComm = 0;
}

/**** Communication ****/
#define MSG_LEN_MAX (1024 * 1024)

/** Send len (random) bytes to "the other guy" */
static void msg_driver_send(msg_comm* comm, size_t len)
{
  int otherPe = !CD->myPe;
  void* data = CD->msgBuf;
  CD->nSends++;
  if (len)
  {
    /* Put in some data, just to get the timing right */
    double v = CD->dont_optimize_away;
    int i, n = len / sizeof(double);
    for (i = 0; i < n; i++) ((double*)data)[i] = v;
  }
  (comm->send_fn)(data, len, otherPe, comm);
}

/**
  This message was actually sent.  This call can
  be made from a msg_send_fn (for a blocking send)
  or from outside (for a non-blocking send).
  */
void msg_send_complete(msg_comm* comm, void* data, size_t len)
{
  CD->nSends--;
  msg_driver_state(comm);
}

/** Receive len (random) bytes from "the other guy" */
static void msg_driver_recv(msg_comm* comm, size_t len)
{
  int otherPe = !comm->driver->myPe;
  CD->nRecvs++;
  (comm->recv_fn)(CD->msgBuf, len, otherPe, comm);
}

/**
  This message just arrived.  This call can
  be made from a msg_recv_fn (for a blocking API
  like MPI), or can be made from outside (for an
  asynchronous API like converse or Charm++).
  */
void msg_recv_complete(msg_comm* comm, void* data, size_t len)
{
  CD->nRecvs--;
  if (len)
  {
    /* Read out the data, just to get the timing right */
    double sum = 0.0;
    int i, n = len / sizeof(double);
    for (i = 0; i < n; i++) sum += ((double*)data)[i];
    CD->dont_optimize_away = sum * (1.0 / MSG_LEN_MAX);
  }
  msg_driver_state(comm);
}

/**** Startup and shutdown ****/

/**
  Begin testing this msg_comm state.
  The "driver" portion can be left uninitalized,
  but all other fields must be filled out.

  This call must be made from exactly two processors,
  0 and 1, at the same time.

  This call will result in calls to the comm
  send and recv functions.  After a number of calls,
  the finish function will be executed, at which
  point the test is over.
  */
void msg_comm_test(msg_comm* comm, const char* desc, int myPe, int verbose)
{
  double startTime;
  CD = (msg_driver*)malloc(sizeof(msg_driver));
  CD->desc = desc;
  CD->myPe = myPe;
  CD->verbose = verbose;
  CD->testNo = 0;
  CD->test.fn = NULL;
  CD->test.name = NULL;
  CD->test.overhead = 0.0;
  CD->test.msgs = -1;
  CD->test.bytes = -1;
  CD->run.current = 0;
  CD->run.last = 0;
  CD->run.start = 0.0;
  CD->nSends = 0;
  CD->nRecvs = 0;
  CD->continuation = NULL;
  CD->counter = 0;
  CD->inComm = 0;
  CD->msgBuf = malloc(MSG_LEN_MAX);
  memset(CD->msgBuf, 0, MSG_LEN_MAX);
  CD->dont_optimize_away = 0.1;
  CD->timerOverhead = 0.0;
  CD->oneWay = 0.0;
  CD->perByte = 0.0;
  CD->perByteBusy = 0.0;
  if (CD->verbose >= 2) printf("msg_comm_test(%d): starting tests\n", CD->myPe);
  msg_driver_state(comm);
}

/** All tests are complete. */
static void msg_comm_test_over(msg_comm* comm)
{
  CD->nRecvs = 123456; /* Last "test" will never finish... */
  free(CD->msgBuf);
  // FIXME: figure out how to delete CD without
  //   reading from uninitialized data on our way out.
  // For now, I'm leaking a few hundred bytes per comm!
  //   free(CD);
  (comm->finish_fn)(comm);
}

/********* Actual Communication Tests **************/

const int nMsgsShort = 1000;
const int nMsgsLong = 1;
const int msgSizeLong = 1024 * 1024;

/** Empty test: for estimating timer overhead */
static void test_nop(msg_comm* comm) {}

/** Barrier: Do a short synchronizing message exchange.
  Communication Pattern:
  0      1
  ->  <-
  */
static void test_barrier(msg_comm* comm)
{
  msg_driver_send(comm, 0);
  msg_driver_recv(comm, 0);
}

/** Jacobi: Do a short pairwise exchange.
  Communication Pattern is just like barrier.
  */
const int K = 1024;
static void test_jacobi_100(msg_comm* comm)
{
  msg_driver_send(comm, 100);
  msg_driver_recv(comm, 100);
}
static void test_jacobi_1K(msg_comm* comm)
{
  msg_driver_send(comm, 1 * K);
  msg_driver_recv(comm, 1 * K);
}
static void test_jacobi_10K(msg_comm* comm)
{
  msg_driver_send(comm, 10 * K);
  msg_driver_recv(comm, 10 * K);
}

static void test_jacobiLong(msg_comm* comm)
{
  msg_driver_send(comm, msgSizeLong);
  msg_driver_recv(comm, msgSizeLong);
}

/** Pingpong: Send a short message back and forth.
  Communication Pattern:
  0        1
  ->
  <-
  ->
  <-
  ...
  */
static void call_pingpong_next(msg_comm* comm)
{
  CD->counter--;
  if (CD->counter > 0)
  { /* recursive case */
    msg_driver_send(comm, 0);
    msg_driver_recv(comm, 0);
    CD->continuation = call_pingpong_next;
  }
  if (CD->counter == 0)
  { /* last time: */
    msg_driver_send(comm, 0);
    if (CD->myPe == 0) msg_driver_recv(comm, 0);
  }
}

const int nMsgsPingpong = 1000; /* must be even */
static void test_pingpong(msg_comm* comm)
{
  CD->counter = nMsgsPingpong / 2;
  if (CD->myPe == 0)
  { /* send first */
    call_pingpong_next(comm);
  }
  else /* myPe==1-- begin with a receive */
  {
    msg_driver_recv(comm, 0);
    CD->continuation = call_pingpong_next;
  }
}

/** Generic repeated message latency/bandwidth test.
  Communication Pattern:
  0        1
  ------>  len bytes per message;
  ------>  n total messages
  ------>
  ...
  <-
  */

static void call_short_send(msg_comm* comm) { msg_driver_send(comm, 0); }

static void test_oneway_inner(msg_comm* comm, size_t len, int n)
{
  int i;
  if (CD->myPe == 0)
  {
    for (i = 0; i < n; i++) msg_driver_send(comm, len);
    msg_driver_recv(comm, 0);
  }
  else /* myPe==1 */
  {
    for (i = 0; i < n; i++) msg_driver_recv(comm, len);
    CD->continuation = call_short_send;
  }
}

static void test_oneway_short(msg_comm* comm) { test_oneway_inner(comm, 0, nMsgsShort); }
static void test_oneway_long(msg_comm* comm)
{
  test_oneway_inner(comm, msgSizeLong, nMsgsLong);
}

/** Testing setup */
static void msg_test_setup(msg_comm* comm, msg_test_fn fn, const char* name)
{
  if (CD->verbose >= 3) printf("msg_comm_test(%d): starting test %s\n", CD->myPe, name);
  CD->test.fn = fn;
  CD->test.name = name;
  msg_run_start(comm);
}

/**
  Begin the next testing task.
  */
static void msg_test_next(msg_comm* comm)
{
  CD->testNo++;
  CD->run.current = 0;
  if (CD->testNo >= 8)
    CD->run.last = 4; /* run slow bandwidth tests only a few times */
  else
    CD->run.last = 20;
  CD->test.overhead = CD->timerOverhead;
  CD->test.msgs = -1;
  CD->test.bytes = -1;
  switch (CD->testNo)
  {
    /* First, measure the timer overhead */
    case 1:
      msg_test_setup(comm, test_nop, "timer only");
      break;
    case 2:
      CD->timerOverhead = CD->run.minTime;
      CD->test.overhead = CD->timerOverhead;
      /* Next, measure the simultanious short-message synch. overhead */
      msg_test_setup(comm, test_barrier, "barrier");
      break;
    case 3:
      msg_test_setup(comm, test_jacobi_100, "jacobi 100");
      break;
    case 4:
      msg_test_setup(comm, test_jacobi_1K, "jacobi  1K");
      break;
    case 5:
      msg_test_setup(comm, test_jacobi_10K, "jacobi 10K");
      break;
    /* Next, measure the sequential short-message synch. overhead */
    case 6:
      CD->test.msgs = nMsgsPingpong;
      msg_test_setup(comm, test_pingpong, "pingpong");
      break;
    case 7:
      CD->oneWay = CD->run.median / nMsgsPingpong;
      /* Now the repeated-short-message overhead */
      CD->test.msgs = nMsgsShort;
      msg_test_setup(comm, test_oneway_short, "one-way short");
      break;

    /* Now the repeated-long-message overhead */
    case 8:
      CD->test.overhead += CD->oneWay; /* subtract off last message */
      CD->test.bytes = nMsgsLong * msgSizeLong;
      msg_test_setup(comm, test_oneway_long, "one-way long");
      break;
    case 9:
      CD->perByte = CD->run.median / msgSizeLong;

      /* Various two-way message tests */
      CD->test.overhead += CD->oneWay; /* subtract off last message */
      CD->test.bytes = msgSizeLong;
      msg_test_setup(comm, test_jacobiLong, "jacobi long");
      break;
    case 10:
      CD->perByteBusy = CD->run.median / msgSizeLong;

      /* That's it-- we're done. */
      if (CD->myPe == 0)
        printf("%s model: %.2f us/msg, %.1f MB/s, %.0f%% contention\n", CD->desc,
               1.0e6 * CD->oneWay, 1.0e-6 / CD->perByte,
               100.0 * (CD->perByteBusy - CD->perByte) / CD->perByteBusy);
      msg_comm_test_over(comm);
      break;
    default:
      fprintf(stderr, "Unrecognized driver test %d!\n", CD->testNo);
      abort();
      break;
  };
}
