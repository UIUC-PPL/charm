/*
  Keep this header free of:
    * include guards
    * extern "C"
    * typedef
    * #define without #ifndef guard or #undef after use
    * global variables
    * basically anything other than function declarations of the form
      AMPI_FUNC/AMPI_CUSTOM_FUNC, with no trailing semicolon
    * #include of files violating any of the above
 */

#ifndef CMK_AMPI_WITH_ROMIO
# error You must include conv-config.h before including this file!
#endif

#ifndef AMPI_NOIMPL_ONLY

#ifndef AMPI_FUNC
# error You must define AMPI_FUNC before including this file!
#endif
#ifndef AMPI_CUSTOM_FUNC
# error You must define AMPI_CUSTOM_FUNC before including this file!
#endif

#if CMK_CUDA
#include "hapi_functions.h"
#endif

AMPI_CUSTOM_FUNC(void, AMPI_Exit, int exitCode)

AMPI_CUSTOM_FUNC(void, ampiMarkAtexit, void)

#ifndef MPI_COMM_NULL_COPY_FN
#define MPI_COMM_NULL_COPY_FN   MPI_comm_null_copy_fn
#endif
#ifndef MPI_COMM_NULL_DELETE_FN
#define MPI_COMM_NULL_DELETE_FN MPI_comm_null_delete_fn
#endif
#ifndef MPI_COMM_DUP_FN
#define MPI_COMM_DUP_FN         MPI_comm_dup_fn
#endif

#ifndef MPI_NULL_COPY_FN
#define MPI_NULL_COPY_FN   MPI_comm_null_copy_fn
#endif
#ifndef MPI_NULL_DELETE_FN
#define MPI_NULL_DELETE_FN MPI_comm_null_delete_fn
#endif
#ifndef MPI_DUP_FN
#define MPI_DUP_FN         MPI_comm_dup_fn
#endif

AMPI_CUSTOM_FUNC(int, MPI_COMM_NULL_COPY_FN   , MPI_Comm, int, void *, void *, void *, int * )
AMPI_CUSTOM_FUNC(int, MPI_COMM_NULL_DELETE_FN , MPI_Comm, int, void *, void * )
AMPI_CUSTOM_FUNC(int, MPI_COMM_DUP_FN         , MPI_Comm, int, void *, void *, void *, int * )

#ifndef MPI_TYPE_NULL_DELETE_FN
#define MPI_TYPE_NULL_DELETE_FN MPI_type_null_delete_fn
#endif
#ifndef MPI_TYPE_NULL_COPY_FN
#define MPI_TYPE_NULL_COPY_FN   MPI_type_null_copy_fn
#endif
#ifndef MPI_TYPE_DUP_FN
#define MPI_TYPE_DUP_FN         MPI_type_dup_fn
#endif

AMPI_CUSTOM_FUNC(int, MPI_TYPE_NULL_COPY_FN   , MPI_Datatype, int, void *, void *, void *, int * )
AMPI_CUSTOM_FUNC(int, MPI_TYPE_NULL_DELETE_FN , MPI_Datatype, int, void *, void * )
AMPI_CUSTOM_FUNC(int, MPI_TYPE_DUP_FN         , MPI_Datatype, int, void *, void *, void *, int * )

#include "pup_c_functions.h"

/***pt2pt***/
AMPI_FUNC(int, MPI_Send, const void *buf, int count, MPI_Datatype type, int dest,
              int tag, MPI_Comm comm)
AMPI_FUNC(int, MPI_Ssend, const void *buf, int count, MPI_Datatype type, int dest,
               int tag, MPI_Comm comm)
AMPI_FUNC(int, MPI_Recv, void *buf, int count, MPI_Datatype type, int src, int tag,
              MPI_Comm comm, MPI_Status *status)
AMPI_FUNC(int, MPI_Mrecv, void* buf, int count, MPI_Datatype datatype, MPI_Message *message,
                  MPI_Status *status)
AMPI_FUNC(int, MPI_Get_count, const MPI_Status *sts, MPI_Datatype dtype, int *count)
AMPI_FUNC(int, MPI_Bsend, const void *buf, int count, MPI_Datatype datatype,
             int dest, int tag,MPI_Comm comm)
AMPI_FUNC(int, MPI_Rsend, const void *buf, int count, MPI_Datatype datatype,
             int dest, int tag,MPI_Comm comm)
AMPI_FUNC(int, MPI_Buffer_attach, void *buffer, int size)
AMPI_FUNC(int, MPI_Buffer_detach, void *buffer, int *size)
AMPI_FUNC(int, MPI_Isend, const void *buf, int count, MPI_Datatype datatype, int dest,
               int tag, MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Ibsend, const void *buf, int count, MPI_Datatype datatype, int dest,
               int tag, MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Issend, const void *buf, int count, MPI_Datatype datatype, int dest,
             int tag, MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Irsend, const void *buf, int count, MPI_Datatype datatype, int dest,
             int tag, MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Irecv, void *buf, int count, MPI_Datatype datatype, int src,
               int tag, MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Imrecv, void* buf, int count, MPI_Datatype datatype, MPI_Message *message,
                  MPI_Request *request)
AMPI_FUNC(int, MPI_Wait, MPI_Request *request, MPI_Status *sts)
AMPI_FUNC(int, MPI_Test, MPI_Request *request, int *flag, MPI_Status *sts)
AMPI_FUNC(int, MPI_Waitany, int count, MPI_Request *request, int *index, MPI_Status *sts)
AMPI_FUNC(int, MPI_Testany, int count, MPI_Request *request, int *index, int *flag, MPI_Status *status)
AMPI_FUNC(int, MPI_Waitall, int count, MPI_Request *request, MPI_Status *sts)
AMPI_FUNC(int, MPI_Testall, int count, MPI_Request *request, int *flag, MPI_Status *sts)
AMPI_FUNC(int, MPI_Waitsome, int incount, MPI_Request *array_of_requests, int *outcount,
                  int *array_of_indices, MPI_Status *array_of_statuses)
AMPI_FUNC(int, MPI_Testsome, int incount, MPI_Request *array_of_requests, int *outcount,
                  int *array_of_indices, MPI_Status *array_of_statuses)
AMPI_FUNC(int, MPI_Request_get_status, MPI_Request request, int *flag, MPI_Status *sts)
AMPI_FUNC(int, MPI_Request_free, MPI_Request *request)
AMPI_FUNC(int, MPI_Grequest_start, MPI_Grequest_query_function *query_fn, MPI_Grequest_free_function *free_fn,\
                  MPI_Grequest_cancel_function *cancel_fn, void *extra_state, MPI_Request *request)
AMPI_FUNC(int, MPI_Grequest_complete, MPI_Request request)
AMPI_FUNC(int, MPI_Cancel, MPI_Request *request)
AMPI_FUNC(int, MPI_Test_cancelled, const MPI_Status *status, int *flag) /* FIXME: always returns success */
AMPI_FUNC(int, MPI_Status_set_cancelled, MPI_Status *status, int flag)
AMPI_FUNC(int, MPI_Status_c2f, const MPI_Status *c_status, MPI_Fint *f_status)
AMPI_FUNC(int, MPI_Status_f2c, const MPI_Fint *f_status, MPI_Status *c_status)
AMPI_FUNC(int, MPI_Iprobe, int src, int tag, MPI_Comm comm, int *flag, MPI_Status *sts)
AMPI_FUNC(int, MPI_Probe, int source, int tag, MPI_Comm comm, MPI_Status *sts)
AMPI_FUNC(int, MPI_Improbe, int source, int tag, MPI_Comm comm, int *flag,
                  MPI_Message *message, MPI_Status *status)
AMPI_FUNC(int, MPI_Mprobe, int source, int tag, MPI_Comm comm, MPI_Message *message,
                  MPI_Status *status)
AMPI_FUNC(int, MPI_Send_init, const void *buf, int count, MPI_Datatype type, int dest, int tag,
                  MPI_Comm comm, MPI_Request *req)
AMPI_FUNC(int, MPI_Ssend_init, const void *buf, int count, MPI_Datatype type, int dest, int tag,
                    MPI_Comm comm, MPI_Request *req)
AMPI_FUNC(int, MPI_Rsend_init, const void *buf, int count, MPI_Datatype type, int dest, int tag,
                  MPI_Comm comm, MPI_Request *req)
AMPI_FUNC(int, MPI_Bsend_init, const void *buf, int count, MPI_Datatype type, int dest, int tag,
                  MPI_Comm comm, MPI_Request *req)
AMPI_FUNC(int, MPI_Recv_init, void *buf, int count, MPI_Datatype type, int src, int tag,
                   MPI_Comm comm, MPI_Request *req)
AMPI_FUNC(int, MPI_Start, MPI_Request *reqnum)
AMPI_FUNC(int, MPI_Startall, int count, MPI_Request *array_of_requests)
AMPI_FUNC(int, MPI_Sendrecv, const void *sbuf, int scount, MPI_Datatype stype, int dest,
                  int stag, void *rbuf, int rcount, MPI_Datatype rtype,
                  int src, int rtag, MPI_Comm comm, MPI_Status *sts)
AMPI_FUNC(int, MPI_Sendrecv_replace, void* buf, int count, MPI_Datatype datatype,
                          int dest, int sendtag, int source, int recvtag,
                          MPI_Comm comm, MPI_Status *status)

/***datatypes***/
AMPI_FUNC(int, MPI_Type_contiguous, int count, MPI_Datatype oldtype,
                         MPI_Datatype *newtype)
AMPI_FUNC(int, MPI_Type_match_size, int typeclass, int size, MPI_Datatype *datatype)
AMPI_FUNC(int, MPI_Type_vector, int count, int blocklength, int stride,
                     MPI_Datatype oldtype, MPI_Datatype *newtype)
AMPI_FUNC(int, MPI_Type_create_hvector, int count, int blocklength, MPI_Aint stride,
                             MPI_Datatype oldtype, MPI_Datatype *newtype)
AMPI_FUNC(int, MPI_Type_hvector, int count, int blocklength, MPI_Aint stride,
                      MPI_Datatype oldtype, MPI_Datatype *newtype)
AMPI_FUNC(int, MPI_Type_indexed, int count, const int* arrBlength, const int* arrDisp,
                      MPI_Datatype oldtype, MPI_Datatype *newtype)
AMPI_FUNC(int, MPI_Type_create_hindexed, int count, const int* arrBlength, const MPI_Aint* arrDisp,
                              MPI_Datatype oldtype, MPI_Datatype *newtype)
AMPI_FUNC(int, MPI_Type_create_indexed_block, int count, int Blength, const int *arrDisp,
                                   MPI_Datatype oldtype, MPI_Datatype *newtype)
AMPI_FUNC(int, MPI_Type_create_hindexed_block, int count, int Blength, const MPI_Aint *arrDisp,
                                    MPI_Datatype oldtype, MPI_Datatype *newtype)
AMPI_FUNC(int, MPI_Type_hindexed, int count, int* arrBlength, MPI_Aint* arrDisp,
                       MPI_Datatype oldtype, MPI_Datatype *newtype)
AMPI_FUNC(int, MPI_Type_create_struct, int count, const int* arrBLength, const MPI_Aint* arrDisp,
                            const MPI_Datatype *oldType, MPI_Datatype *newType)
AMPI_FUNC(int, MPI_Type_struct, int count, int* arrBLength, MPI_Aint* arrDisp,
                     MPI_Datatype *oldType, MPI_Datatype *newType)
AMPI_FUNC(int, MPI_Type_get_envelope, MPI_Datatype datatype, int *num_integers, int *num_addresses,
                           int *num_datatypes, int *combiner)
AMPI_FUNC(int, MPI_Type_get_contents, MPI_Datatype datatype, int max_integers, int max_addresses,
                           int max_datatypes, int array_of_integers[], MPI_Aint array_of_addresses[],
                           MPI_Datatype array_of_datatypes[])
AMPI_FUNC(int, MPI_Type_commit, MPI_Datatype *datatype)
AMPI_FUNC(int, MPI_Type_free, MPI_Datatype *datatype)
AMPI_FUNC(int, MPI_Type_get_extent, MPI_Datatype datatype, MPI_Aint *lb, MPI_Aint *extent)
AMPI_FUNC(int, MPI_Type_get_extent_x, MPI_Datatype datatype, MPI_Count *lb, MPI_Count *extent)
AMPI_FUNC(int, MPI_Type_extent, MPI_Datatype datatype, MPI_Aint *extent)
AMPI_FUNC(int, MPI_Type_get_true_extent, MPI_Datatype datatype, MPI_Aint *true_lb, MPI_Aint *true_extent)
AMPI_FUNC(int, MPI_Type_get_true_extent_x, MPI_Datatype datatype, MPI_Count *true_lb, MPI_Count *true_extent)
AMPI_FUNC(int, MPI_Type_size, MPI_Datatype datatype, int *size)
AMPI_FUNC(int, MPI_Type_size_x, MPI_Datatype datatype, MPI_Count *size)
AMPI_FUNC(int, MPI_Type_lb, MPI_Datatype datatype, MPI_Aint* displacement)
AMPI_FUNC(int, MPI_Type_ub, MPI_Datatype datatype, MPI_Aint* displacement)
AMPI_FUNC(int, MPI_Type_set_name, MPI_Datatype datatype, const char *name)
AMPI_FUNC(int, MPI_Type_get_name, MPI_Datatype datatype, char *name, int *resultlen)
AMPI_FUNC(int, MPI_Type_dup, MPI_Datatype oldtype, MPI_Datatype *newtype)
AMPI_FUNC(int, MPI_Type_create_resized, MPI_Datatype oldtype, MPI_Aint lb, MPI_Aint extent, MPI_Datatype *newtype)
AMPI_FUNC(int, MPI_Type_set_attr, MPI_Datatype datatype, int type_keyval, void *attribute_val)
AMPI_FUNC(int, MPI_Type_get_attr, MPI_Datatype datatype, int type_keyval, void *attribute_val, int *flag)
AMPI_FUNC(int, MPI_Type_delete_attr, MPI_Datatype datatype, int type_keyval)
AMPI_FUNC(int, MPI_Type_create_keyval, MPI_Type_copy_attr_function *type_copy_attr_fn,
                            MPI_Type_delete_attr_function *type_delete_attr_fn,
                            int *type_keyval, void *extra_state)
AMPI_FUNC(int, MPI_Type_free_keyval, int *type_keyval)
AMPI_FUNC(int, MPI_Type_create_darray, int size, int rank, int ndims,
  const int array_of_gsizes[], const int array_of_distribs[],
  const int array_of_dargs[], const int array_of_psizes[],
  int order, MPI_Datatype oldtype, MPI_Datatype *newtype)
AMPI_FUNC(int, MPI_Type_create_subarray, int ndims, const int array_of_sizes[],
  const int array_of_subsizes[], const int array_of_starts[], int order,
  MPI_Datatype oldtype, MPI_Datatype *newtype)
AMPI_FUNC(int, MPI_Get_address, const void* location, MPI_Aint *address)
AMPI_FUNC(int, MPI_Address, void* location, MPI_Aint *address)
AMPI_FUNC(int, MPI_Status_set_elements, MPI_Status *status, MPI_Datatype datatype, int count)
AMPI_FUNC(int, MPI_Status_set_elements_x, MPI_Status *status, MPI_Datatype datatype, MPI_Count count)
AMPI_FUNC(int, MPI_Get_elements, const MPI_Status *status, MPI_Datatype datatype, int *count)
AMPI_FUNC(int, MPI_Get_elements_x, const MPI_Status *status, MPI_Datatype datatype, MPI_Count *count)
AMPI_FUNC(int, MPI_Pack, const void *inbuf, int incount, MPI_Datatype dtype, void *outbuf,
              int outsize, int *position, MPI_Comm comm)
AMPI_FUNC(int, MPI_Unpack, const void *inbuf, int insize, int *position, void *outbuf,
                int outcount, MPI_Datatype dtype, MPI_Comm comm)
AMPI_FUNC(int, MPI_Pack_size, int incount,MPI_Datatype datatype,MPI_Comm comm,int *sz)

/***collectives***/
AMPI_FUNC(int, MPI_Barrier, MPI_Comm comm)
AMPI_FUNC(int, MPI_Ibarrier, MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Bcast, void *buf, int count, MPI_Datatype type, int root, MPI_Comm comm)
AMPI_FUNC(int, MPI_Ibcast, void *buf, int count, MPI_Datatype type, int root, MPI_Comm comm,
                MPI_Request *request)
AMPI_FUNC(int, MPI_Gather, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                void *recvbuf, int recvcount, MPI_Datatype recvtype,
                int root, MPI_Comm comm)
AMPI_FUNC(int, MPI_Igather, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                 int root, MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Gatherv, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 void *recvbuf, const int *recvcounts, const int *displs,
                 MPI_Datatype recvtype, int root, MPI_Comm comm)
AMPI_FUNC(int, MPI_Igatherv, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                  void *recvbuf, const int *recvcounts, const int *displs,
                  MPI_Datatype recvtype, int root, MPI_Comm comm,
                  MPI_Request *request)
AMPI_FUNC(int, MPI_Scatter, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                 int root, MPI_Comm comm)
AMPI_FUNC(int, MPI_Iscatter, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                  void *recvbuf, int recvcount, MPI_Datatype recvtype,
                  int root, MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Scatterv, const void *sendbuf, const int *sendcounts, const int *displs, MPI_Datatype sendtype,
                  void *recvbuf, int recvcount, MPI_Datatype recvtype,
                  int root, MPI_Comm comm)
AMPI_FUNC(int, MPI_Iscatterv, const void *sendbuf, const int *sendcounts, const int *displs, MPI_Datatype sendtype,
                   void *recvbuf, int recvcount, MPI_Datatype recvtype,
                   int root, MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Allgather, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                   void *recvbuf, int recvcount, MPI_Datatype recvtype,
                   MPI_Comm comm)
AMPI_FUNC(int, MPI_Iallgather, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                    void *recvbuf, int recvcount, MPI_Datatype recvtype,
                    MPI_Comm comm, MPI_Request* request)
AMPI_FUNC(int, MPI_Allgatherv, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                    void *recvbuf, const int *recvcounts, const int *displs,
                    MPI_Datatype recvtype, MPI_Comm comm)
AMPI_FUNC(int, MPI_Iallgatherv, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                     void *recvbuf, const int *recvcounts, const int *displs,
                     MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Alltoall, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                  void *recvbuf, int recvcount, MPI_Datatype recvtype,
                  MPI_Comm comm)
AMPI_FUNC(int, MPI_Ialltoall, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                   void *recvbuf, int recvcount, MPI_Datatype recvtype,
                   MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Alltoallv, const void *sendbuf, const int *sendcounts, const int *sdispls,
                   MPI_Datatype sendtype, void *recvbuf, const int *recvcounts,
                   const int *rdispls, MPI_Datatype recvtype, MPI_Comm comm)
AMPI_FUNC(int, MPI_Ialltoallv, void *sendbuf, int *sendcounts, int *sdispls,
                    MPI_Datatype sendtype, void *recvbuf, int *recvcounts,
                    int *rdispls, MPI_Datatype recvtype, MPI_Comm comm,
                    MPI_Request *request)
AMPI_FUNC(int, MPI_Alltoallw, const void *sendbuf, const int *sendcounts, const int *sdispls,
                   const MPI_Datatype *sendtypes, void *recvbuf, const int *recvcounts,
                   const int *rdispls, const MPI_Datatype *recvtypes, MPI_Comm comm)
AMPI_FUNC(int, MPI_Ialltoallw, const void *sendbuf, const int *sendcounts, const int *sdispls,
                    const MPI_Datatype *sendtypes, void *recvbuf, const int *recvcounts,
                    const int *rdispls, const MPI_Datatype *recvtypes, MPI_Comm comm,
                    MPI_Request *request)
AMPI_FUNC(int, MPI_Reduce, const void *inbuf, void *outbuf, int count, MPI_Datatype type,
                MPI_Op op, int root, MPI_Comm comm)
AMPI_FUNC(int, MPI_Ireduce, const void *sendbuf, void *recvbuf, int count, MPI_Datatype type,
                 MPI_Op op, int root, MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Allreduce, const void *inbuf, void *outbuf, int count, MPI_Datatype type,
                   MPI_Op op, MPI_Comm comm)
AMPI_FUNC(int, MPI_Iallreduce, const void *inbuf, void *outbuf, int count, MPI_Datatype type,
                    MPI_Op op, MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Reduce_local, const void *inbuf, void *outbuf, int count,
                      MPI_Datatype datatype, MPI_Op op)
AMPI_FUNC(int, MPI_Reduce_scatter_block, const void* sendbuf, void* recvbuf, int count,
                              MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
AMPI_FUNC(int, MPI_Ireduce_scatter_block, const void* sendbuf, void* recvbuf, int count,
                              MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Reduce_scatter, const void* sendbuf, void* recvbuf, const int *recvcounts,
                        MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
AMPI_FUNC(int, MPI_Ireduce_scatter, const void* sendbuf, void* recvbuf, const int *recvcounts,
                        MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Scan, const void* sendbuf, void* recvbuf, int count, MPI_Datatype datatype,
              MPI_Op op, MPI_Comm comm )
AMPI_FUNC(int, MPI_Iscan, const void* sendbuf, void* recvbuf, int count, MPI_Datatype datatype,
              MPI_Op op, MPI_Comm comm, MPI_Request *request)
AMPI_FUNC(int, MPI_Exscan, const void* sendbuf, void* recvbuf, int count, MPI_Datatype datatype,
                MPI_Op op, MPI_Comm comm)
AMPI_FUNC(int, MPI_Iexscan, const void* sendbuf, void* recvbuf, int count, MPI_Datatype datatype,
                MPI_Op op, MPI_Comm comm, MPI_Request *request)

/***neighborhood collectives***/
AMPI_FUNC(int, MPI_Neighbor_alltoall, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                           void* recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm)
AMPI_FUNC(int, MPI_Ineighbor_alltoall, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                            void* recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm,
                            MPI_Request* request)
AMPI_FUNC(int, MPI_Neighbor_alltoallv, const void* sendbuf, const int* sendcounts, const int* sdispls,
                            MPI_Datatype sendtype, void* recvbuf, const int* recvcounts, const int* rdispls,
                            MPI_Datatype recvtype, MPI_Comm comm)
AMPI_FUNC(int, MPI_Ineighbor_alltoallv, const void* sendbuf, const int* sendcounts, const int* sdispls,
                             MPI_Datatype sendtype, void* recvbuf, const int* recvcounts, const int* rdispls,
                             MPI_Datatype recvtype, MPI_Comm comm, MPI_Request* request)
AMPI_FUNC(int, MPI_Neighbor_alltoallw, const void* sendbuf, const int* sendcounts, const MPI_Aint* sdipls,
                            const MPI_Datatype* sendtypes, void* recvbuf, const int* recvcounts, const MPI_Aint* rdispls,
                            const MPI_Datatype* recvtypes, MPI_Comm comm)
AMPI_FUNC(int, MPI_Ineighbor_alltoallw, const void* sendbuf, const int* sendcounts, const MPI_Aint* sdispls,
                             const MPI_Datatype* sendtypes, void* recvbuf, const int* recvcounts, const MPI_Aint* rdispls,
                             const MPI_Datatype* recvtypes, MPI_Comm comm, MPI_Request* request)
AMPI_FUNC(int, MPI_Neighbor_allgather, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                            void* recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm)
AMPI_FUNC(int, MPI_Ineighbor_allgather, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                             void* recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm,
                             MPI_Request *request)
AMPI_FUNC(int, MPI_Neighbor_allgatherv, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                             void* recvbuf, const int* recvcounts, const int* displs, MPI_Datatype recvtype,
                             MPI_Comm comm)
AMPI_FUNC(int, MPI_Ineighbor_allgatherv, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                              void* recvbuf, const int* recvcounts, const int* displs, MPI_Datatype recvtype,
                              MPI_Comm comm, MPI_Request *request)

/***ops***/
AMPI_FUNC(int, MPI_Op_create, MPI_User_function *function, int commute, MPI_Op *op)
AMPI_FUNC(int, MPI_Op_free, MPI_Op *op)
AMPI_FUNC(int, MPI_Op_commutative, MPI_Op op, int* commute)

/***groups***/
AMPI_FUNC(int, MPI_Group_size, MPI_Group group, int *size)
AMPI_FUNC(int, MPI_Group_rank, MPI_Group group, int *rank)
AMPI_FUNC(int, MPI_Group_translate_ranks, MPI_Group group1, int n, const int *ranks1, MPI_Group group2, int *ranks2)
AMPI_FUNC(int, MPI_Group_compare, MPI_Group group1,MPI_Group group2, int *result)
AMPI_FUNC(int, MPI_Comm_group, MPI_Comm comm, MPI_Group *group)
AMPI_FUNC(int, MPI_Group_union, MPI_Group group1, MPI_Group group2, MPI_Group *newgroup)
AMPI_FUNC(int, MPI_Group_intersection, MPI_Group group1, MPI_Group group2, MPI_Group *newgroup)
AMPI_FUNC(int, MPI_Group_difference, MPI_Group group1, MPI_Group group2, MPI_Group *newgroup)
AMPI_FUNC(int, MPI_Group_incl, MPI_Group group, int n, const int *ranks, MPI_Group *newgroup)
AMPI_FUNC(int, MPI_Group_excl, MPI_Group group, int n, const int *ranks, MPI_Group *newgroup)
AMPI_FUNC(int, MPI_Group_range_incl, MPI_Group group, int n, int ranges[][3], MPI_Group *newgroup)
AMPI_FUNC(int, MPI_Group_range_excl, MPI_Group group, int n, int ranges[][3], MPI_Group *newgroup)
AMPI_FUNC(int, MPI_Group_free, MPI_Group *group)

/***communicators***/
AMPI_FUNC(int, MPI_Intercomm_create, MPI_Comm local_comm, int local_leader, MPI_Comm peer_comm,
                          int remote_leader, int tag, MPI_Comm *newintercomm)
AMPI_FUNC(int, MPI_Intercomm_merge, MPI_Comm intercomm, int high, MPI_Comm *newintracomm)
AMPI_FUNC(int, MPI_Comm_create, MPI_Comm comm, MPI_Group group, MPI_Comm* newcomm)
AMPI_FUNC(int, MPI_Comm_create_group, MPI_Comm comm, MPI_Group group, int tag, MPI_Comm *newcomm)
AMPI_FUNC(int, MPI_Comm_size, MPI_Comm comm, int *size)
AMPI_FUNC(int, MPI_Comm_rank, MPI_Comm comm, int *rank)
AMPI_FUNC(int, MPI_Comm_compare, MPI_Comm comm1,MPI_Comm comm2, int *result)
AMPI_FUNC(int, MPI_Comm_split, MPI_Comm src, int color, int key, MPI_Comm *dest)
AMPI_FUNC(int, MPI_Comm_split_type, MPI_Comm src, int split_type, int key, MPI_Info info, MPI_Comm *dest)
AMPI_FUNC(int, MPI_Comm_dup, MPI_Comm src, MPI_Comm *dest)
AMPI_FUNC(int, MPI_Comm_idup, MPI_Comm comm, MPI_Comm *newcomm, MPI_Request *request)
AMPI_FUNC(int, MPI_Comm_dup_with_info, MPI_Comm src, MPI_Info info, MPI_Comm *dest)
AMPI_FUNC(int, MPI_Comm_idup_with_info, MPI_Comm src, MPI_Info info, MPI_Comm *dest, MPI_Request *request)
AMPI_FUNC(int, MPI_Comm_free, MPI_Comm *comm)
AMPI_FUNC(int, MPI_Comm_test_inter, MPI_Comm comm, int *flag)
AMPI_FUNC(int, MPI_Comm_remote_size, MPI_Comm comm, int *size)
AMPI_FUNC(int, MPI_Comm_remote_group, MPI_Comm comm, MPI_Group *group)
AMPI_FUNC(int, MPI_Comm_set_name, MPI_Comm comm, const char *name)
AMPI_FUNC(int, MPI_Comm_get_name, MPI_Comm comm, char *comm_name, int *resultlen)
AMPI_FUNC(int, MPI_Comm_set_info, MPI_Comm comm, MPI_Info info)
AMPI_FUNC(int, MPI_Comm_get_info, MPI_Comm comm, MPI_Info *info)
AMPI_FUNC(int, MPI_Comm_call_errhandler, MPI_Comm comm, int errorcode)
AMPI_FUNC(int, MPI_Comm_create_errhandler, MPI_Comm_errhandler_fn *function, MPI_Errhandler *errhandler)
AMPI_FUNC(int, MPI_Comm_set_errhandler, MPI_Comm comm, MPI_Errhandler errhandler)
AMPI_FUNC(int, MPI_Comm_get_errhandler, MPI_Comm comm, MPI_Errhandler *errhandler)
AMPI_FUNC(int, MPI_Comm_free_errhandler, MPI_Errhandler *errhandler)
AMPI_FUNC(int, MPI_Comm_create_keyval, MPI_Comm_copy_attr_function *copy_fn, MPI_Comm_delete_attr_function *delete_fn,
                            int *keyval, void* extra_state)
AMPI_FUNC(int, MPI_Comm_free_keyval, int *keyval)
AMPI_FUNC(int, MPI_Comm_set_attr, MPI_Comm comm, int keyval, void* attribute_val)
AMPI_FUNC(int, MPI_Comm_get_attr, MPI_Comm comm, int keyval, void *attribute_val, int *flag)
AMPI_FUNC(int, MPI_Comm_delete_attr, MPI_Comm comm, int keyval)

/***keyvals/attributes***/
AMPI_FUNC(int, MPI_Keyval_create, MPI_Copy_function *copy_fn, MPI_Delete_function *delete_fn,
                       int *keyval, void* extra_state)
AMPI_FUNC(int, MPI_Keyval_free, int *keyval)
AMPI_FUNC(int, MPI_Attr_put, MPI_Comm comm, int keyval, void* attribute_val)
AMPI_FUNC(int, MPI_Attr_get, MPI_Comm comm, int keyval, void *attribute_val, int *flag)
AMPI_FUNC(int, MPI_Attr_delete, MPI_Comm comm, int keyval)

/***topologies***/
AMPI_FUNC(int, MPI_Cart_create, MPI_Comm comm_old, int ndims, const int *dims,
                     const int *periods, int reorder, MPI_Comm *comm_cart)
AMPI_FUNC(int, MPI_Graph_create, MPI_Comm comm_old, int nnodes, const int *index,
                      const int *edges, int reorder, MPI_Comm *comm_graph)
AMPI_FUNC(int, MPI_Dist_graph_create_adjacent, MPI_Comm comm_old, int indegree, const int sources[],
                                    const int sourceweights[], int outdegree,
                                    const int destinations[], const int destweights[],
                                    MPI_Info info, int reorder, MPI_Comm *comm_dist_graph)
AMPI_FUNC(int, MPI_Dist_graph_create, MPI_Comm comm_old, int n, const int sources[], const int degrees[],
                           const int destintations[], const int weights[], MPI_Info info,
                           int reorder, MPI_Comm *comm_dist_graph)
AMPI_FUNC(int, MPI_Topo_test, MPI_Comm comm, int *status)
AMPI_FUNC(int, MPI_Cart_map, MPI_Comm comm, int ndims, const int *dims, const int *periods,
                  int *newrank)
AMPI_FUNC(int, MPI_Graph_map, MPI_Comm comm, int nnodes, const int *index, const int *edges,
                   int *newrank)
AMPI_FUNC(int, MPI_Cartdim_get, MPI_Comm comm, int *ndims)
AMPI_FUNC(int, MPI_Cart_get, MPI_Comm comm, int maxdims, int *dims, int *periods, int *coords)
AMPI_FUNC(int, MPI_Cart_rank, MPI_Comm comm, const int *coords, int *rank)
AMPI_FUNC(int, MPI_Cart_coords, MPI_Comm comm, int rank, int maxdims, int *coords)
AMPI_FUNC(int, MPI_Cart_shift, MPI_Comm comm, int direction, int disp, int *rank_source, int *rank_dest)
AMPI_FUNC(int, MPI_Graphdims_get, MPI_Comm comm, int *nnodes, int *nedges)
AMPI_FUNC(int, MPI_Graph_get, MPI_Comm comm, int maxindex, int maxedges, int *index, int *edges)
AMPI_FUNC(int, MPI_Graph_neighbors_count, MPI_Comm comm, int rank, int *nneighbors)
AMPI_FUNC(int, MPI_Graph_neighbors, MPI_Comm comm, int rank, int maxneighbors, int *neighbors)
AMPI_FUNC(int, MPI_Dims_create, int nnodes, int ndims, int *dims)
AMPI_FUNC(int, MPI_Cart_sub, MPI_Comm comm, const int *remain_dims, MPI_Comm *newcomm)
AMPI_FUNC(int, MPI_Dist_graph_neighbors, MPI_Comm comm, int maxindegree, int sources[], int sourceweights[],
                              int maxoutdegree, int destinations[], int destweights[])
AMPI_FUNC(int, MPI_Dist_graph_neighbors_count, MPI_Comm comm, int *indegree, int *outdegree, int *weighted)

/***environment management***/
AMPI_FUNC(int, MPI_Errhandler_create, MPI_Handler_function *function, MPI_Errhandler *errhandler)
AMPI_FUNC(int, MPI_Errhandler_set, MPI_Comm comm, MPI_Errhandler errhandler)
AMPI_FUNC(int, MPI_Errhandler_get, MPI_Comm comm, MPI_Errhandler *errhandler)
AMPI_FUNC(int, MPI_Errhandler_free, MPI_Errhandler *errhandler)
AMPI_FUNC(int, MPI_Add_error_code, int errorclass, int *errorcode)
AMPI_FUNC(int, MPI_Add_error_class, int *errorclass)
AMPI_FUNC(int, MPI_Add_error_string, int errorcode, const char *errorstring)
AMPI_FUNC(int, MPI_Error_class, int errorcode, int *errorclass)
AMPI_FUNC(int, MPI_Error_string, int errorcode, char *string, int *resultlen)
AMPI_FUNC(int, MPI_Get_version, int *version, int *subversion)
AMPI_FUNC(int, MPI_Get_library_version, char *version, int *resultlen)
AMPI_FUNC(int, MPI_Get_processor_name, char *name, int *resultlen)
AMPI_FUNC(double, MPI_Wtime, void)
AMPI_FUNC(double, MPI_Wtick, void)
AMPI_FUNC(int, MPI_Is_thread_main, int *flag)
AMPI_FUNC(int, MPI_Query_thread, int *provided)
AMPI_FUNC(int, MPI_Init_thread, int *argc, char*** argv, int required, int *provided)
AMPI_FUNC(int, MPI_Init, int *argc, char*** argv)
AMPI_FUNC(int, MPI_Initialized, int *isInit)
AMPI_FUNC(int, MPI_Finalize, void)
AMPI_FUNC(int, MPI_Finalized, int *finalized)
AMPI_FUNC(int, MPI_Abort, MPI_Comm comm, int errorcode)
AMPI_FUNC(int, MPI_Pcontrol, const int level, ...)
AMPI_FUNC(int, MPI_File_call_errhandler, MPI_File fh, int errorcode)
AMPI_FUNC(int, MPI_File_create_errhandler, MPI_File_errhandler_function *function, MPI_Errhandler *errhandler)
#if !CMK_AMPI_WITH_ROMIO
/* Disable in ROMIO's mpio_functions.h if enabling these. */
AMPI_FUNC(int, MPI_File_get_errhandler, MPI_File file, MPI_Errhandler *errhandler)
AMPI_FUNC(int, MPI_File_set_errhandler, MPI_File file, MPI_Errhandler errhandler)
#endif

/*********************One sided communication routines *****************/
#ifndef MPI_WIN_NULL_DELETE_FN
#define MPI_WIN_NULL_DELETE_FN MPI_win_null_delete_fn
#endif
#ifndef MPI_WIN_NULL_COPY_FN
#define MPI_WIN_NULL_COPY_FN   MPI_win_null_copy_fn
#endif
#ifndef MPI_WIN_DUP_FN
#define MPI_WIN_DUP_FN         MPI_win_dup_fn
#endif

AMPI_CUSTOM_FUNC(int, MPI_WIN_NULL_COPY_FN   , MPI_Win, int, void *, void *, void *, int * )
AMPI_CUSTOM_FUNC(int, MPI_WIN_NULL_DELETE_FN , MPI_Win, int, void *, void * )
AMPI_CUSTOM_FUNC(int, MPI_WIN_DUP_FN         , MPI_Win, int, void *, void *, void *, int * )

/***windows/rma***/
AMPI_FUNC(int, MPI_Win_create, void *base, MPI_Aint size, int disp_unit,
                    MPI_Info info, MPI_Comm comm, MPI_Win *newwin)
AMPI_FUNC(int, MPI_Win_allocate, MPI_Aint size, int disp_unit, MPI_Info info, MPI_Comm comm, void *baseptr, MPI_Win *win)
AMPI_FUNC(int, MPI_Win_free, MPI_Win *win)
AMPI_FUNC(int, MPI_Win_create_errhandler, MPI_Win_errhandler_function *win_errhandler_fn,
                               MPI_Errhandler *errhandler)
AMPI_FUNC(int, MPI_Win_call_errhandler, MPI_Win win, int errorcode)
AMPI_FUNC(int, MPI_Win_get_errhandler, MPI_Win win, MPI_Errhandler *errhandler)
AMPI_FUNC(int, MPI_Win_set_errhandler, MPI_Win win, MPI_Errhandler errhandler)
AMPI_FUNC(int, MPI_Win_create_keyval, MPI_Win_copy_attr_function *copy_fn,
                           MPI_Win_delete_attr_function *delete_fn,
                           int *keyval, void *extra_state)
AMPI_FUNC(int, MPI_Win_free_keyval, int *keyval)
AMPI_FUNC(int, MPI_Win_delete_attr, MPI_Win win, int key)
AMPI_FUNC(int, MPI_Win_get_attr, MPI_Win win, int win_keyval, void *attribute_val, int *flag)
AMPI_FUNC(int, MPI_Win_set_attr, MPI_Win win, int win_keyval, void *attribute_val)
AMPI_FUNC(int, MPI_Win_get_group, MPI_Win win, MPI_Group *group)
AMPI_FUNC(int, MPI_Win_set_name, MPI_Win win, const char *name)
AMPI_FUNC(int, MPI_Win_get_name, MPI_Win win, char *name, int *length)
AMPI_FUNC(int, MPI_Win_set_info, MPI_Win win, MPI_Info info)
AMPI_FUNC(int, MPI_Win_get_info, MPI_Win win, MPI_Info *info)
AMPI_FUNC(int, MPI_Win_lock_all, int assert, MPI_Win win)
AMPI_FUNC(int, MPI_Win_unlock_all, MPI_Win win)
AMPI_FUNC(int, MPI_Win_fence, int assertion, MPI_Win win)
AMPI_FUNC(int, MPI_Win_lock, int lock_type, int rank, int assert, MPI_Win win)
AMPI_FUNC(int, MPI_Win_unlock, int rank, MPI_Win win)
AMPI_FUNC(int, MPI_Win_post, MPI_Group group, int assertion, MPI_Win win)
AMPI_FUNC(int, MPI_Win_wait, MPI_Win win)
AMPI_FUNC(int, MPI_Win_start, MPI_Group group, int assertion, MPI_Win win)
AMPI_FUNC(int, MPI_Win_complete, MPI_Win win)
AMPI_FUNC(int, MPI_Win_test, MPI_Win win, int *flag)
AMPI_FUNC(int, MPI_Alloc_mem, MPI_Aint size, MPI_Info info, void *baseptr)
AMPI_FUNC(int, MPI_Free_mem, void *base)
AMPI_FUNC(int, MPI_Put, const void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
             MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, MPI_Win win)
AMPI_FUNC(int, MPI_Get, void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
             MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, MPI_Win win)
AMPI_FUNC(int, MPI_Accumulate, const void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
                    MPI_Aint targdisp, int targcnt, MPI_Datatype targtype,
                    MPI_Op op, MPI_Win win)
AMPI_FUNC(int, MPI_Get_accumulate, const void *orgaddr, int orgcnt, MPI_Datatype orgtype,
                        void *resaddr, int rescnt, MPI_Datatype restype,
                        int rank, MPI_Aint targdisp, int targcnt,
                        MPI_Datatype targtype, MPI_Op op, MPI_Win win)
AMPI_FUNC(int, MPI_Rput, const void *orgaddr, int orgcnt, MPI_Datatype orgtype, int targrank,
             MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, MPI_Win win,
             MPI_Request *request)
AMPI_FUNC(int, MPI_Rget, void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
              MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, MPI_Win win,
              MPI_Request *request)
AMPI_FUNC(int, MPI_Raccumulate, const void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
                     MPI_Aint targdisp, int targcnt, MPI_Datatype targtype,
                     MPI_Op op, MPI_Win win, MPI_Request *request)
AMPI_FUNC(int, MPI_Rget_accumulate, const void *orgaddr, int orgcnt, MPI_Datatype orgtype,
                         void *resaddr, int rescnt, MPI_Datatype restype,
                         int rank, MPI_Aint targdisp, int targcnt,
                         MPI_Datatype targtype, MPI_Op op, MPI_Win win,
                         MPI_Request *request)
AMPI_FUNC(int, MPI_Fetch_and_op, const void *orgaddr, void *resaddr, MPI_Datatype type,
                      int rank, MPI_Aint targdisp, MPI_Op op, MPI_Win win)
AMPI_FUNC(int, MPI_Compare_and_swap, const void *orgaddr, const void *compaddr, void *resaddr,
                          MPI_Datatype type, int rank, MPI_Aint targdisp,
                          MPI_Win win)

/***infos***/
AMPI_FUNC(int, MPI_Info_create, MPI_Info *info)
AMPI_FUNC(int, MPI_Info_set, MPI_Info info, const char *key, const char *value)
AMPI_FUNC(int, MPI_Info_delete, MPI_Info info, const char *key)
AMPI_FUNC(int, MPI_Info_get, MPI_Info info, const char *key, int valuelen, char *value, int *flag)
AMPI_FUNC(int, MPI_Info_get_valuelen, MPI_Info info, const char *key, int *valuelen, int *flag)
AMPI_FUNC(int, MPI_Info_get_nkeys, MPI_Info info, int *nkeys)
AMPI_FUNC(int, MPI_Info_get_nthkey, MPI_Info info, int n, char *key)
AMPI_FUNC(int, MPI_Info_dup, MPI_Info info, MPI_Info *newinfo)
AMPI_FUNC(int, MPI_Info_free, MPI_Info *info)


/***MPIX***/
AMPI_FUNC(int, MPIX_Grequest_start, MPI_Grequest_query_function *query_fn,
  MPI_Grequest_free_function *free_fn, MPI_Grequest_cancel_function *cancel_fn,
  MPIX_Grequest_poll_function *poll_fn, void *extra_state, MPI_Request *request)
AMPI_FUNC(int, MPIX_Grequest_class_create, MPI_Grequest_query_function *query_fn,
  MPI_Grequest_free_function *free_fn, MPI_Grequest_cancel_function *cancel_fn,
  MPIX_Grequest_poll_function *poll_fn, MPIX_Grequest_wait_function *wait_fn,
  MPIX_Grequest_class *greq_class)
AMPI_FUNC(int, MPIX_Grequest_class_allocate, MPIX_Grequest_class greq_class,
  void *extra_state, MPI_Request *request)


/* Extensions needed by ROMIO */
AMPI_FUNC(int, MPIR_Status_set_bytes, MPI_Status *sts, MPI_Datatype dtype, MPI_Count nbytes)

#include "mpio_functions.h"


/*** AMPI Extensions ***/
AMPI_CUSTOM_FUNC(char **, AMPI_Get_argv, void)
AMPI_CUSTOM_FUNC(int, AMPI_Get_argc, void)
AMPI_CUSTOM_FUNC(int, AMPI_Migrate, MPI_Info hints)
AMPI_CUSTOM_FUNC(int, AMPI_Load_start_measure, void)
AMPI_CUSTOM_FUNC(int, AMPI_Load_stop_measure, void)
AMPI_CUSTOM_FUNC(int, AMPI_Load_reset_measure, void)
AMPI_CUSTOM_FUNC(int, AMPI_Load_set_value, double value)
AMPI_CUSTOM_FUNC(int, AMPI_Load_set_phase, int phase)
AMPI_CUSTOM_FUNC(int, AMPI_Migrate_to_pe, int dest)
AMPI_CUSTOM_FUNC(int, AMPI_Set_migratable, int mig)
AMPI_CUSTOM_FUNC(int, AMPI_Register_pup, MPI_PupFn fn, void *data, int *idx)
AMPI_CUSTOM_FUNC(int, AMPI_Get_pup_data, int idx, void *data)
AMPI_CUSTOM_FUNC(int, AMPI_Register_about_to_migrate, MPI_MigrateFn fn)
AMPI_CUSTOM_FUNC(int, AMPI_Register_just_migrated, MPI_MigrateFn fn)
AMPI_CUSTOM_FUNC(int, AMPI_Iget, MPI_Aint orgdisp, int orgcnt, MPI_Datatype orgtype, int rank,
              MPI_Aint targdisp, int targcnt, MPI_Datatype targtype,
              MPI_Win win, MPI_Request *request)
AMPI_CUSTOM_FUNC(int, AMPI_Iget_wait, MPI_Request *request, MPI_Status *status, MPI_Win win)
AMPI_CUSTOM_FUNC(int, AMPI_Iget_free, MPI_Request *request, MPI_Status *status, MPI_Win win)
AMPI_CUSTOM_FUNC(int, AMPI_Iget_data, void *data, MPI_Status status)
AMPI_CUSTOM_FUNC(int, AMPI_Type_is_contiguous, MPI_Datatype datatype, int *flag)
AMPI_CUSTOM_FUNC(int, AMPI_Yield, void)
AMPI_CUSTOM_FUNC(int, AMPI_Suspend, void)
AMPI_CUSTOM_FUNC(int, AMPI_Resume, int dest, MPI_Comm comm)
AMPI_CUSTOM_FUNC(int, AMPI_Print, const char *str)
AMPI_CUSTOM_FUNC(int, AMPI_Trace_begin, void)
AMPI_CUSTOM_FUNC(int, AMPI_Trace_end, void)
AMPI_CUSTOM_FUNC(int, AMPI_Alltoall_medium, void *sendbuf, int sendcount, MPI_Datatype sendtype,
                         void *recvbuf, int recvcount, MPI_Datatype recvtype,
                         MPI_Comm comm)
AMPI_CUSTOM_FUNC(int, AMPI_Alltoall_long, void *sendbuf, int sendcount, MPI_Datatype sendtype,
                       void *recvbuf, int recvcount, MPI_Datatype recvtype,
                       MPI_Comm comm)


#ifdef __cplusplus
#if CMK_CUDA
AMPI_CUSTOM_FUNC(int, AMPI_GPU_Iinvoke_wr, hapiWorkRequest *to_call, MPI_Request *request)
AMPI_CUSTOM_FUNC(int, AMPI_GPU_Iinvoke, cudaStream_t stream, MPI_Request *request)
AMPI_CUSTOM_FUNC(int, AMPI_GPU_Invoke_wr, hapiWorkRequest *to_call)
AMPI_CUSTOM_FUNC(int, AMPI_GPU_Invoke, cudaStream_t stream)
#endif
#endif

/* Execute this shell command , just like "system, )") */
AMPI_CUSTOM_FUNC(int, AMPI_System, const char *cmd)

/* Determine approximate depth of stack at the point of this call */
AMPI_CUSTOM_FUNC(long, ampiCurrentStackUsage, void)

#endif /* !defined AMPI_NOIMPL_ONLY */


/* Functions unsupported in AMPI */

/* Disable deprecation warnings added in ampi.h */
#if defined __GNUC__ || defined __clang__
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined _MSC_VER
# pragma warning(push)
# pragma warning(disable : 4996)
#elif defined __INTEL_COMPILER
# pragma warning push
# pragma warning disable 1478
# pragma warning disable 1786
#endif

#ifndef AMPI_FUNC_NOIMPL
# error You must define AMPI_FUNC_NOIMPL before including this file!
#endif

/* MPI 3.1 standards compliance overview.
 * This list contains all MPI functions not supported in AMPI currently.
*/

/* A.2.1 Point-to-Point Communication C Bindings */

/* A.2.2 Datatypes C Bindings */

AMPI_FUNC_NOIMPL(int, MPI_Pack_external, const char datarep[], const void *inbuf, int incount, MPI_Datatype datatype, void *outbuf, MPI_Aint outsize, MPI_Aint *position)
AMPI_FUNC_NOIMPL(int, MPI_Pack_external_size, const char datarep[], int incount, MPI_Datatype datatype, MPI_Aint *size)
AMPI_FUNC_NOIMPL(int, MPI_Unpack_external, const char datarep[], const void *inbuf, MPI_Aint insize, MPI_Aint *position, void *outbuf, int outcount, MPI_Datatype datatype)

/* A.2.3 Collective Communication C Bindings */

/* A.2.4 Groups, Contexts, Communicators, and Caching C Bindings */

/* A.2.6 MPI Environmental Management C Bindings */

/* A.2.7 The Info Object C Bindings */

/* A.2.8 Process Creation and Management C Bindings */

AMPI_FUNC_NOIMPL(int, MPI_Close_port, const char *port_name)
AMPI_FUNC_NOIMPL(int, MPI_Comm_accept, const char *port_name, MPI_Info info, int root, MPI_Comm comm, MPI_Comm *newcomm)
AMPI_FUNC_NOIMPL(int, MPI_Comm_connect, const char *port_name, MPI_Info info, int root, MPI_Comm comm, MPI_Comm *newcomm)
AMPI_FUNC_NOIMPL(int, MPI_Comm_disconnect, MPI_Comm *comm)
AMPI_FUNC_NOIMPL(int, MPI_Comm_get_parent, MPI_Comm *parent)
AMPI_FUNC_NOIMPL(int, MPI_Comm_join, int fd, MPI_Comm *intercomm)
AMPI_FUNC_NOIMPL(int, MPI_Comm_spawn, const char *command, char *argv[], int maxprocs, MPI_Info info, int root, MPI_Comm comm, MPI_Comm *intercomm, int array_of_errcodes[])
AMPI_FUNC_NOIMPL(int, MPI_Comm_spawn_multiple, int count, char *array_of_commands[], char **array_of_argv[], const int array_of_maxprocs[], const MPI_Info array_of_info[], int root, MPI_Comm comm, MPI_Comm *intercomm, int array_of_errcodes[])
AMPI_FUNC_NOIMPL(int, MPI_Lookup_name, const char *service_name, MPI_Info info, char *port_name)
AMPI_FUNC_NOIMPL(int, MPI_Open_port, MPI_Info info, char *port_name)
AMPI_FUNC_NOIMPL(int, MPI_Publish_name, const char *service_name, MPI_Info info, const char *port_name)
AMPI_FUNC_NOIMPL(int, MPI_Unpublish_name, const char *service_name, MPI_Info info, const char *port_name)


/* A.2.9 One-Sided Communications C Bindings */

AMPI_FUNC_NOIMPL(int, MPI_Win_allocate_shared, MPI_Aint size, int disp_unit, MPI_Info info, MPI_Comm comm, void *baseptr, MPI_Win *win)
AMPI_FUNC_NOIMPL(int, MPI_Win_attach, MPI_Win win, void *base, MPI_Aint size)
AMPI_FUNC_NOIMPL(int, MPI_Win_create_dynamic, MPI_Info info, MPI_Comm comm, MPI_Win *win)
AMPI_FUNC_NOIMPL(int, MPI_Win_detach, MPI_Win win, const void *base)
AMPI_FUNC_NOIMPL(int, MPI_Win_flush, int rank, MPI_Win win)
AMPI_FUNC_NOIMPL(int, MPI_Win_flush_all, MPI_Win win)
AMPI_FUNC_NOIMPL(int, MPI_Win_flush_local, int rank, MPI_Win win)
AMPI_FUNC_NOIMPL(int, MPI_Win_flush_local_all, MPI_Win win)
AMPI_FUNC_NOIMPL(int, MPI_Win_shared_query, MPI_Win win, int rank, MPI_Aint *size, int *disp_unit, void *baseptr)
AMPI_FUNC_NOIMPL(int, MPI_Win_sync, MPI_Win win)


/* A.2.10 External Interfaces C Bindings */

/* A.2.11 I/O C Bindings */

AMPI_FUNC_NOIMPL(int, MPI_CONVERSION_FN_NULL, void *userbuf, MPI_Datatype datatype, int count, void *filebuf, MPI_Offset position, void *extra_state)


/* A.2.12 Language Bindings C Bindings */

AMPI_FUNC_NOIMPL(int, MPI_Status_f082f, MPI_F08_status *f08_status, MPI_Fint *f_status)
AMPI_FUNC_NOIMPL(int, MPI_Status_f2f08, MPI_Fint *f_status, MPI_F08_status *f08_status)
AMPI_FUNC_NOIMPL(int, MPI_Type_create_f90_complex, int p, int r, MPI_Datatype *newtype)
AMPI_FUNC_NOIMPL(int, MPI_Type_create_f90_integer, int r, MPI_Datatype *newtype)
AMPI_FUNC_NOIMPL(int, MPI_Type_create_f90_real, int p, int r, MPI_Datatype *newtype)
AMPI_FUNC_NOIMPL(int, MPI_Status_c2f08, const MPI_Status *c_status, MPI_F08_status *f08_status)
AMPI_FUNC_NOIMPL(int, MPI_Status_f082c, const MPI_F08_status *f08_status, MPI_Status *c_status)


/* A.2.14 Tools / MPI Tool Information Interface C Bindings */

AMPI_FUNC_NOIMPL(int, MPI_T_category_changed, int *stamp)
AMPI_FUNC_NOIMPL(int, MPI_T_category_get_categories, int cat_index, int len, int indices[])
AMPI_FUNC_NOIMPL(int, MPI_T_category_get_cvars, int cat_index, int len, int indices[])
AMPI_FUNC_NOIMPL(int, MPI_T_category_get_index, const char *name, int *cat_index)
AMPI_FUNC_NOIMPL(int, MPI_T_category_get_info, int cat_index, char *name, int *name_len, char *desc, int *desc_len, int *num_cvars, int *num_pvars, int *num_categories)
AMPI_FUNC_NOIMPL(int, MPI_T_category_get_num, int *num_cat)
AMPI_FUNC_NOIMPL(int, MPI_T_category_get_pvars, int cat_index, int len, int indices[])
AMPI_FUNC_NOIMPL(int, MPI_T_cvar_get_index, const char *name, int *cvar_index)
AMPI_FUNC_NOIMPL(int, MPI_T_cvar_get_info, int cvar_index, char *name, int *name_len, int *verbosity, MPI_Datatype *datatype, MPI_T_enum *enumtype, char *desc, int *desc_len, int *bind, int *scope)
AMPI_FUNC_NOIMPL(int, MPI_T_cvar_get_num, int *num_cvar)
AMPI_FUNC_NOIMPL(int, MPI_T_cvar_handle_alloc, int cvar_index, void *obj_handle, MPI_T_cvar_handle *handle, int *count)
AMPI_FUNC_NOIMPL(int, MPI_T_cvar_handle_free, MPI_T_cvar_handle *handle)
AMPI_FUNC_NOIMPL(int, MPI_T_cvar_read, MPI_T_cvar_handle handle, void* buf)
AMPI_FUNC_NOIMPL(int, MPI_T_cvar_write, MPI_T_cvar_handle handle, const void* buf)
AMPI_FUNC_NOIMPL(int, MPI_T_enum_get_info, MPI_T_enum enumtype, int *num, char *name, int *name_len)
AMPI_FUNC_NOIMPL(int, MPI_T_enum_get_item, MPI_T_enum enumtype, int index, int *value, char *name, int *name_len)
AMPI_FUNC_NOIMPL(int, MPI_T_finalize, void)
AMPI_FUNC_NOIMPL(int, MPI_T_init_thread, int required, int *provided)
AMPI_FUNC_NOIMPL(int, MPI_T_pvar_get_index, const char *name, int var_class, int *pvar_index)
AMPI_FUNC_NOIMPL(int, MPI_T_pvar_get_info, int pvar_index, char *name, int *name_len, int *verbosity, int *var_class, MPI_Datatype *datatype, MPI_T_enum *enumtype, char *desc, int *desc_len, int *bind, int *readonly, int *continuous, int *atomic)
AMPI_FUNC_NOIMPL(int, MPI_T_pvar_get_num, int *num_pvar)
AMPI_FUNC_NOIMPL(int, MPI_T_pvar_handle_alloc, MPI_T_pvar_session session, int pvar_index, void *obj_handle, MPI_T_pvar_handle *handle, int *count)
AMPI_FUNC_NOIMPL(int, MPI_T_pvar_handle_free, MPI_T_pvar_session session,MPI_T_pvar_handle *handle)
AMPI_FUNC_NOIMPL(int, MPI_T_pvar_read, MPI_T_pvar_session session, MPI_T_pvar_handle handle,void* buf)
AMPI_FUNC_NOIMPL(int, MPI_T_pvar_readreset, MPI_T_pvar_session session,MPI_T_pvar_handle handle, void* buf)
AMPI_FUNC_NOIMPL(int, MPI_T_pvar_reset, MPI_T_pvar_session session, MPI_T_pvar_handle handle)
AMPI_FUNC_NOIMPL(int, MPI_T_pvar_session_create, MPI_T_pvar_session *session)
AMPI_FUNC_NOIMPL(int, MPI_T_pvar_session_free, MPI_T_pvar_session *session)
AMPI_FUNC_NOIMPL(int, MPI_T_pvar_start, MPI_T_pvar_session session, MPI_T_pvar_handle handle)
AMPI_FUNC_NOIMPL(int, MPI_T_pvar_stop, MPI_T_pvar_session session, MPI_T_pvar_handle handle)
AMPI_FUNC_NOIMPL(int, MPI_T_pvar_write, MPI_T_pvar_session session, MPI_T_pvar_handle handle, const void* buf)


/* A.2.15 Deprecated C Bindings */


#if defined __GNUC__ || defined __clang__
# pragma GCC diagnostic pop
#elif defined _MSC_VER
# pragma warning(pop)
#elif defined __INTEL_COMPILER
# pragma warning pop
#endif
