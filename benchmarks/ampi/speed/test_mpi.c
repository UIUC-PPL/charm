/*
   msg_ comm program to test out speed of MPI.
   */
#include "mpi.h"
#include "msgspeed.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct mpiComm mpiComm;
struct mpiComm
{
  msg_comm b;
  MPI_Comm comm;
  int myRank;
  int doneFlag;

/* Only used during Isend: */
#define maxIsend 3
  int nIsend;
  struct isendReq
  {
    void* data;
    size_t len;
    MPI_Request req;
  } isend[maxIsend];
};

void mpi_bsend_fn(void* data, size_t len, int dest, msg_comm* comm)
{
  mpiComm* c = (mpiComm*)comm;
  MPI_Bsend(data, len, MPI_BYTE, dest, 0, c->comm);
  msg_send_complete(comm, data, len);
}

/* Finish up previously initiated Isends */
void mpi_isend_poll(msg_comm* comm)
{
  mpiComm* c = (mpiComm*)comm;
  int i;
  for (i = 0; i < c->nIsend; i++)
  {
    int flg = 0;
    MPI_Status sts;
    MPI_Test(&c->isend[i].req, &flg, &sts);
    if (flg)
    { /* this send is now done */
      msg_send_complete(comm, c->isend[i].data, c->isend[i].len);
      c->isend[i--] = c->isend[--c->nIsend];
    }
  }
}

void mpi_isend_fn(void* data, size_t len, int dest, msg_comm* comm)
{
  int n;
  mpiComm* c = (mpiComm*)comm;
  while (c->nIsend >= maxIsend)
  {
    // Outgoing queue full-- clean out messages
    mpi_isend_poll(comm);
  }
  n = c->nIsend++;
  c->isend[n].data = data;
  c->isend[n].len = len;
  MPI_Isend(data, len, MPI_BYTE, dest, 0, c->comm, &c->isend[n].req);
}

void mpi_recv_fn(void* data, size_t len, int src, msg_comm* comm)
{
  mpiComm* c = (mpiComm*)comm;
  MPI_Status sts;
  MPI_Recv(data, len, MPI_BYTE, src, 0, c->comm, &sts);
  msg_recv_complete(comm, data, len);
}

void mpi_finish_fn(msg_comm* comm)
{
  mpiComm* c = (mpiComm*)comm;
  c->doneFlag = 1;
}

void startMPItest(MPI_Comm comm, int verbose)
{
  int bufSize = 2 * 1024 * 1024;
  char* buf = malloc(bufSize);

  mpiComm msg;
  mpiComm* c = &msg;
  c->b.recv_fn = mpi_recv_fn;
  c->b.finish_fn = mpi_finish_fn;
  c->comm = comm;
  MPI_Comm_rank(c->comm, &c->myRank);

  MPI_Buffer_attach(buf, bufSize);

  // Run the Bsend test, which is non-blocking:
  c->b.send_fn = mpi_bsend_fn;
  c->doneFlag = 0;
  msg_comm_test(&c->b, "MPI Bsend", c->myRank, verbose);

  // Run the Isend test, which may block:
  c->b.send_fn = mpi_isend_fn;
  c->doneFlag = 0;
  c->nIsend = 0;
  msg_comm_test(&c->b, "MPI Isend", c->myRank, verbose);
  while (!c->doneFlag) mpi_isend_poll(&c->b);

  MPI_Buffer_detach(buf, &bufSize);
  free(buf);
}
