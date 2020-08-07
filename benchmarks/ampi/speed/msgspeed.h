/**
  Framework for measuring message-passing speed.
  Orion Sky Lawlor, olawlor@uiuc.edu, 2003/8/21

  This is needed to accurately, uniformly compare
  vertically, across all our API's:
  - Converse's netlrts-linux
  - Charm++ messaging
  - AMPI messaging

  And horizontally, across different interconnects:
  - UDP packets
  - TCP sockets
  - Myrinet
  - Infiniband
  - Shared memory
  - Various MPI's
  - IBM's LAPI

  The basic idea is that an interconnect writes
  a small, simple set of routines-- basically send
  and recv-- and this code will try out a bunch of
  different message passing styles to measure speed,
  bandwidth, and congestion.

  This header should be usable outside of Charm++.
  */
#ifndef __CHARM_MSG_SPEED_H
#define __CHARM_MSG_SPEED_H

/* for size_t */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

  typedef struct msg_comm msg_comm;
  typedef struct msg_driver msg_driver;

  /**
    Send len bytes of data to dest.
    Once the data is sent, you must call msg_send_complete.
    */
  typedef void (*msg_send_fn)(void *data,size_t len, int dest,msg_comm *comm);

  /**
    This message was actually sent.  This call can
    be made from a msg_send_fn (for a blocking send)
    or from outside (for a non-blocking send).
    */
  void msg_send_complete(msg_comm *comm,void *data,size_t len);


  /**
Hint: you're about to recv len bytes of data from src.
Once the data arrives, you must call msg_recv_complete.
You only own the data pointer from this call until
you make the call to msg_recv_complete.
*/
  typedef void (*msg_recv_fn)(void *data,size_t len, int src,msg_comm *comm);

  /**
    This message just arrived.  This call can
    be made from a msg_recv_fn (for a blocking API
    like MPI), or can be made from outside (for an
    asynchronous API like converse or Charm++).

    The data pointer need not be the same one passed
    to msg_recv, and this function does not transfer
    ownership of the data pointer.
    */
  void msg_recv_complete(msg_comm *comm,void *data,size_t len);


  /**
    We're ready to stop the test now. (collective)
    */
  typedef void (*msg_finish_fn)(msg_comm *comm);

  /**
    Interconnect writers will fill this struct out with
    their send and recv functions, possibly extending the struct
    with their own data.
    */
  struct msg_comm {
    msg_driver *driver; /* controller routine private data */

    msg_send_fn send_fn;
    msg_recv_fn recv_fn;
    msg_finish_fn finish_fn;
  };

  /**
    Begin a test using this msg_comm state.
    The "driver" portion can be left uninitalized,
    but all other fields must be filled out.

    This call must be made from exactly two processors,
    0 and 1, at the same time.

    This call will result in calls to the comm
    send and recv functions.  After a number of calls,
    the finish function will be executed, at which
    point the test is over.
    */
  void msg_comm_test(msg_comm *comm,const char *desc,int myPe,int verbose);


  /* Returns wall clock time in seconds */
  double msg_timer(void);

#ifdef __cplusplus
};
#endif

#endif

