#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "mpi.h"

// change this verbosity level to get more detailed prints
int verboseLevel = 1;

void intercomm_bcast_test(MPI_Comm myFirstComm, int global_rank, int root, bool non_blocking) {
  int data = 0;
  char msg[20] = {0};
  MPI_Request req = MPI_REQUEST_NULL;

  if (global_rank == 0) {//root
    if(non_blocking) {
      strcpy(msg, "Hello World");
      MPI_Ibcast(msg, 12, MPI_CHAR, MPI_ROOT, myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
      assert(strcmp(msg,"Hello World") == 0); //verify that buffer in root is not modified
      strcpy(msg, "Root - new message");
    }
    else {
      data = 42;
      MPI_Bcast(&data, 1, MPI_INT, MPI_ROOT, myFirstComm);
      assert(data == 42);
    }
  }
  else if (global_rank%2 == 0) {//local group
    if (non_blocking) {
      MPI_Ibcast(msg, 12, MPI_CHAR, MPI_PROC_NULL, myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
      assert(strcmp(msg,"") == 0); //local ranks should not receive broadcasted msg
    }
    else {
      MPI_Bcast(&data, 1, MPI_INT, MPI_PROC_NULL, myFirstComm);
      assert(data == 0); //local ranks should not receive broadcasted data
    }
  }
  else {//remote group
    if (non_blocking) {
      MPI_Ibcast(msg, 12, MPI_CHAR, root, myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
      assert(strcmp(msg,"Hello World") == 0);
    }
    else {
      MPI_Bcast(&data, 1, MPI_INT, root, myFirstComm);
      assert(data == 42);
    }
  }

  if (verboseLevel > 10) {
    if (non_blocking)
      printf("[%d] msg: %s\n", global_rank, msg);
    else
      printf("[%d] data : %d\n", global_rank, data);
  }
}

void intercomm_barrier_test(MPI_Comm myFirstComm, int global_rank, bool non_blocking) {
  MPI_Request req = MPI_REQUEST_NULL;

  if (global_rank % 2 == 0) {
    if (non_blocking) {
      MPI_Ibarrier(myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
    }
    else {
      MPI_Barrier(myFirstComm);
    }

    if (verboseLevel > 10)
      printf("[%d]Local group resumed\n", global_rank);
  }
  else {
    if (verboseLevel > 10)
      printf("[%d]Remote group ranks doing work before barrier\n", global_rank);
    if (non_blocking) {
      MPI_Ibarrier(myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
    }
    else {
      MPI_Barrier(myFirstComm);
    }
  }
}

void intercomm_gather_test(MPI_Comm myFirstComm, int global_rank, int root, bool non_blocking) {
  int sendarray[5];
  int *rbuf, remoteSize;
  MPI_Request req = MPI_REQUEST_NULL;
  MPI_Comm_remote_size(myFirstComm, &remoteSize);
  rbuf = (int*) malloc(remoteSize*5*sizeof(int));

  if (global_rank == 0) {//root
    if (non_blocking) {
      MPI_Igather(sendarray, 5, MPI_INT, rbuf, 5, MPI_INT, MPI_ROOT, myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
    }
    else {
      MPI_Gather(sendarray, 5, MPI_INT, rbuf, 5, MPI_INT, MPI_ROOT, myFirstComm);
    }
    // assertions and print here
    int idx = 0;
    for (int i = 0; i < remoteSize; i++) {
      for (int j = 0; j < 5; j++) {
        //each remote rank sends its local_rank in an array of size 5
        assert(rbuf[idx] == i);
        if (verboseLevel > 10) printf(" %d", rbuf[idx]);
        idx++;
      }
    }
    if (verboseLevel > 10) printf("\n");
  }
  else if (global_rank%2 == 0) {//local group
    if (non_blocking) {
      MPI_Igather(sendarray, 5, MPI_INT, rbuf, 5, MPI_INT, MPI_PROC_NULL, myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
    }
    else {
      MPI_Gather(sendarray, 5, MPI_INT, rbuf, 5, MPI_INT, MPI_PROC_NULL, myFirstComm);
    }
  }
  else if (global_rank%2 == 1) {//remote group
    int local_rank;
    MPI_Comm_rank(myFirstComm, &local_rank);
    for (int i = 0; i < 5; i++)
      sendarray[i] = local_rank;

    if (non_blocking) {
      MPI_Igather(sendarray, 5, MPI_INT, rbuf, 5, MPI_INT, root, myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
    }
    else {
      MPI_Gather(sendarray, 5, MPI_INT, rbuf, 5, MPI_INT, root, myFirstComm);
    }
  }
}

void intercomm_gatherv_test(MPI_Comm myFirstComm, int global_rank, int root, bool non_blocking) {
  int local_rank, remote_size;
  MPI_Comm_rank(myFirstComm, &local_rank);
  // each remote rank sends its local_rank number of elements
  int *sendbuf = (int*)malloc(local_rank*sizeof(int));
  MPI_Comm_remote_size(myFirstComm, &remote_size);
  MPI_Request req = MPI_REQUEST_NULL;

  int recvbuf_size = 0;
  int *recv_counts = (int*)malloc(remote_size*sizeof(int));
  int *displacements = (int*)malloc(remote_size*sizeof(int));
  for (int i = 0; i < remote_size; i++) {
    recvbuf_size += i;
    recv_counts[i] = i;
    if (i == 0) {
      displacements[i] = 0;
    }
    else {
      displacements[i] = displacements[i-1] + recv_counts[i-1];
    }
  }
  int *recvbuf = (int*)malloc(recvbuf_size*sizeof(int));

  if (global_rank == 0) {//root for gatherv
    if (non_blocking) {
      MPI_Igatherv(sendbuf, local_rank, MPI_INT, recvbuf, recv_counts, displacements, MPI_INT, MPI_ROOT, myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
    }
    else {
      MPI_Gatherv(sendbuf, local_rank, MPI_INT, recvbuf, recv_counts, displacements, MPI_INT, MPI_ROOT, myFirstComm);
    }

    // assert and print recvbuf here
    int idx = 0;
    for (int i = 0; i < remote_size; i++) {
      for (int j = 0; j < i; j++) {
        //each remote rank sends its local_rank in an array of local_rank size
        assert(recvbuf[idx] == i);
        if (verboseLevel > 10) printf(" %d", recvbuf[idx]);
        idx++;
      }
    }
    if (verboseLevel > 10) printf("\n");
  }
  else if (global_rank%2 == 0) {//local group
    if (non_blocking) {
      MPI_Igatherv(sendbuf, local_rank, MPI_INT, recvbuf, recv_counts, displacements, MPI_INT, MPI_PROC_NULL, myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
    }
    else {
      MPI_Gatherv(sendbuf, local_rank, MPI_INT, recvbuf, recv_counts, displacements, MPI_INT, MPI_PROC_NULL, myFirstComm);
    }
  }
  else if (global_rank%2 == 1) {//remote group
    for (int i = 0; i < local_rank; i++) {
      sendbuf[i] = local_rank;
    }
    if (non_blocking) {
      MPI_Igatherv(sendbuf, local_rank, MPI_INT, recvbuf, recv_counts, displacements, MPI_INT, root, myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
    }
    else {
      MPI_Gatherv(sendbuf, local_rank, MPI_INT, recvbuf, recv_counts, displacements, MPI_INT, root, myFirstComm);
    }
  }

}

void intercomm_scatter_test(MPI_Comm myFirstComm, int global_rank, int root, bool non_blocking) {
  int remote_size;
  MPI_Comm_remote_size(myFirstComm, &remote_size);
  int sendcount = 5;
  int *sendbuf = (int*)malloc(sendcount*remote_size*sizeof(int));
  int *recvbuf = (int*)malloc(sendcount*sizeof(int));
  MPI_Request req = MPI_REQUEST_NULL;

  if (global_rank == 0) {
    int idx = 0;
    for (int i = 0; i < remote_size; i++) {
      for (int j = 0; j < sendcount; j++) {
        sendbuf[idx] = i;
        idx++;
      }
    }

    if (non_blocking) {
      MPI_Iscatter(sendbuf, sendcount, MPI_INT, recvbuf, sendcount, MPI_INT, MPI_ROOT, myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
    } else {
      MPI_Scatter(sendbuf, sendcount, MPI_INT, recvbuf, sendcount, MPI_INT, MPI_ROOT, myFirstComm);
    }
  }
  else if (global_rank%2 == 0) {
    if (non_blocking) {
      MPI_Iscatter(sendbuf, sendcount, MPI_INT, recvbuf, sendcount, MPI_INT, MPI_PROC_NULL, myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
    } else {
      MPI_Scatter(sendbuf, sendcount, MPI_INT, recvbuf, sendcount, MPI_INT, MPI_PROC_NULL, myFirstComm);
    }
  }
  else {
    if (non_blocking) {
      MPI_Iscatter(sendbuf, sendcount, MPI_INT, recvbuf, sendcount, MPI_INT, root, myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
    }
    else {
      MPI_Scatter(sendbuf, sendcount, MPI_INT, recvbuf, sendcount, MPI_INT, root, myFirstComm);
    }

    int local_rank;
    MPI_Comm_rank(myFirstComm, &local_rank);
    if (verboseLevel > 10) printf ("[%d]", local_rank);
    for (int i = 0; i < sendcount; i++) {
      // each remote rank receives its local_rank in an array of size sendcount
      assert(recvbuf[i] == local_rank);
      if (verboseLevel > 10) printf(" %d", recvbuf[i]);
    }
    if (verboseLevel > 10) printf("\n");
  }
}

void intercomm_scatterv_test(MPI_Comm myFirstComm, int global_rank, int root, bool non_blocking) {
  int local_rank, remote_size;
  MPI_Comm_rank(myFirstComm, &local_rank);
  // each remote rank receives its local_rank number of elements
  int* recvbuf = (int*) malloc(local_rank * sizeof(int));
  MPI_Comm_remote_size(myFirstComm, &remote_size);
  MPI_Request req = MPI_REQUEST_NULL;

  int sendbuf_size = 0;
  int* send_counts = (int*) malloc(remote_size * sizeof(int));
  int* displacements = (int*) malloc(remote_size * sizeof(int));
  for (int i = 0; i < remote_size; i++) {
    sendbuf_size += i;
    send_counts[i] = i;
    if (i == 0) {
      displacements[i] = 0;
    }
    else {
      displacements[i] = displacements[i-1] + send_counts[i-1];
    }
  }
  int* sendbuf = (int*) malloc(sendbuf_size * sizeof(int));

  if (global_rank == 0) { // root for scatterv
    int idx = 0;
    for (int i = 0; i < remote_size; i++) {
      for (int j = 0; j < send_counts[i]; j++) {
        sendbuf[idx] = i;
        idx++;
      }
    }

    if (non_blocking) {
      MPI_Iscatterv(sendbuf, send_counts, displacements, MPI_INT, recvbuf, local_rank, MPI_INT, MPI_ROOT, myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
    }
    else {
      MPI_Scatterv(sendbuf, send_counts, displacements, MPI_INT, recvbuf, local_rank, MPI_INT, MPI_ROOT, myFirstComm);
    }
  }
  else if (global_rank%2 == 0) { // local group
    if (non_blocking) {
      MPI_Iscatterv(sendbuf, send_counts, displacements, MPI_INT, recvbuf, local_rank, MPI_INT, MPI_PROC_NULL, myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
    }
    else {
      MPI_Scatterv(sendbuf, send_counts, displacements, MPI_INT, recvbuf, local_rank, MPI_INT, MPI_PROC_NULL, myFirstComm);
    }
  }
  else if (global_rank%2 == 1) { // remote group
    if (non_blocking) {
      MPI_Iscatterv(sendbuf, send_counts, displacements, MPI_INT, recvbuf, local_rank, MPI_INT, root, myFirstComm, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
    }
    else {
      MPI_Scatterv(sendbuf, send_counts, displacements, MPI_INT, recvbuf, local_rank, MPI_INT, root, myFirstComm);
    }

    // assert and print recvbuf here
    for (int i = 0; i < local_rank; i++) {
      assert(recvbuf[i] == local_rank);
      if (verboseLevel > 10 && i == 0) printf("[%d]", local_rank);
      if (verboseLevel > 10) printf(" %d", recvbuf[i]);
    }
    if (verboseLevel > 10 && local_rank > 0) printf("\n");
  }
}

int main(int argc, char **argv) {
  int size, global_rank;
  MPI_Comm myComm;
  MPI_Comm myFirstComm;
  int root = 0; // rank of root in local group
  if (argc>1) verboseLevel = atoi(argv[1]);

  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &global_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  int color = global_rank % 2;
  MPI_Comm_split(MPI_COMM_WORLD, color, global_rank, &myComm);
  MPI_Intercomm_create(myComm, 0, MPI_COMM_WORLD, (color+1)%2, 1, &myFirstComm);
  MPI_Comm_free(&myComm);

  /* Intercommunicator broadcast collective tests */
  if (global_rank == 0) printf("[0] Testing intercomm bcast\n");
  intercomm_bcast_test(myFirstComm, global_rank, root, false); /* Intercomm bcast test */
  intercomm_bcast_test(myFirstComm, global_rank, root, true); /* Intercomm ibcast test */

  /* Intercommunicator scatter collective tests */
  if (global_rank == 0) printf("[0] Testing intercomm scatter\n");
  intercomm_scatter_test(myFirstComm, global_rank, root, false); /* Intercomm scatter test */
  intercomm_scatter_test(myFirstComm, global_rank, root, true); /* Intercomm iscatter test */

  /* Intercommunicator scatterv collective tests */
  if (global_rank == 0) printf("[0] Testing intercomm scatterv\n");
  intercomm_scatterv_test(myFirstComm, global_rank, root, false); /* Intercomm scatterv test */
  intercomm_scatterv_test(myFirstComm, global_rank, root, true); /* Intercomm iscatterv test */

  /* Intercommunicator barrier collective tests */
  if (global_rank == 0) printf("[0] Testing intercomm barrier\n");
  intercomm_barrier_test(myFirstComm, global_rank, false); /* Intercomm barrier test */
  intercomm_barrier_test(myFirstComm, global_rank, true); /* Intercomm ibarrier test */

#if 0 /* AMPI does not implement gather and gatherv for intercommunicators */
  /* Intercommunicator gather collective tests */
  if (global_rank == 0) printf("[0] Testing intercomm gather\n");
  intercomm_gather_test(myFirstComm, global_rank, root, false); /* Intercomm gather test */
  intercomm_gather_test(myFirstComm, global_rank, root, true); /* Intercomm igather test */

  /* Intercommunicator gatherv collective tests */
  if (global_rank == 0) printf("[0] Testing intercomm gatherv\n");
  intercomm_gatherv_test(myFirstComm, global_rank, root, false); /* Intercomm gatherv test */
  intercomm_gatherv_test(myFirstComm, global_rank, root, true); /* Intercomm igatherv test */
#endif

  MPI_Comm_free(&myFirstComm);

  if (global_rank==size-1) printf("All tests passed\n");
  MPI_Finalize();
  return 0;

}

