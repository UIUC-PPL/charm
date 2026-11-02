/*
 * campi (coroutine AMPI): application header. Requires C++20.
 *
 * Usage:
 *   #include "mpi.h"
 *   #include "campi.h"
 *   CAMPI_MAIN(argc, argv) {
 *     MPI_Init(&argc, &argv);          // expands to co_await campi::Init(...)
 *     ...
 *     MPI_Recv(buf, n, MPI_INT, 0, 0, MPI_COMM_WORLD, &sts);   // co_await
 *     ...
 *     MPI_Finalize();
 *     co_return 0;
 *   }
 *
 * The blocking MPI names below are redefined as co_await expressions on the
 * campi awaitables (campi_core.h). Using one of them in a function that is
 * not a coroutine is a compile error: any function that (transitively) calls
 * a blocking MPI routine must itself return campi::task<T> and be awaited.
 * Blocking routines campi does not provide yet expand to an undeclared
 * identifier naming the routine, so they fail to compile instead of
 * aborting at run time. Nonblocking and local routines are AMPI's own,
 * except the test and probe routines (MPI_Test*, MPI_Request_get_status,
 * MPI_Iprobe, MPI_Improbe): a miss yields to the scheduler so that messages
 * can arrive, so they are awaitables too and a polling loop must be in a
 * coroutine.
 *
 * Unlike thread AMPI, where MPI_Finalize ends the rank and the process exits
 * with status 0, MPI_Finalize does not end a campi rank: code after it runs,
 * and the process exit code is the value main co_returns.
 */
#ifndef CAMPI_H
#define CAMPI_H

#include "campi_core.h"

#define CAMPI_MAIN(argc_, argv_)                                              \
  ::campi::task<int> campi_main(int argc_, char **argv_);                     \
  static ::campi::main_registrar campi_main_registrar_(&campi_main);          \
  ::campi::task<int> campi_main(int argc_, char **argv_)

#define CAMPI_AWAIT(expr) (co_await (expr))

#undef MPI_Init
#undef MPI_Init_thread
#undef MPI_Finalize
#undef MPI_Send
#undef MPI_Ssend
#undef MPI_Recv
#undef MPI_Sendrecv
#undef MPI_Wait
#undef MPI_Waitall
#undef MPI_Barrier
#undef MPI_Bcast
#undef MPI_Reduce
#undef MPI_Allreduce
#undef MPI_Gather
#undef MPI_Gatherv
#undef MPI_Allgather
#undef MPI_Allgatherv
#undef MPI_Scatter
#undef MPI_Scatterv
#undef MPI_Alltoall
#undef MPI_Alltoallv
#undef MPI_Scan
#undef MPI_Exscan
#undef MPI_Comm_split
#undef MPI_Comm_dup
#undef MPI_Comm_dup_with_info
#undef MPI_Comm_create
#undef MPI_Test
#undef MPI_Testany
#undef MPI_Testall
#undef MPI_Testsome
#undef MPI_Request_get_status
#undef MPI_Iprobe
#undef MPI_Improbe
#undef AMPI_Migrate

#define MPI_Init(...)        CAMPI_AWAIT(::campi::Init(__VA_ARGS__))
#define MPI_Init_thread(...) CAMPI_AWAIT(::campi::Init_thread(__VA_ARGS__))
#define MPI_Finalize()       CAMPI_AWAIT(::campi::Finalize())
#define MPI_Send(...)        CAMPI_AWAIT(::campi::Send(__VA_ARGS__))
#define MPI_Ssend(...)       CAMPI_AWAIT(::campi::Ssend(__VA_ARGS__))
#define MPI_Recv(...)        CAMPI_AWAIT(::campi::Recv(__VA_ARGS__))
#define MPI_Sendrecv(...)    CAMPI_AWAIT(::campi::Sendrecv(__VA_ARGS__))
#define MPI_Wait(...)        CAMPI_AWAIT(::campi::Wait(__VA_ARGS__))
#define MPI_Waitall(...)     CAMPI_AWAIT(::campi::Waitall(__VA_ARGS__))
#define MPI_Barrier(...)     CAMPI_AWAIT(::campi::Barrier(__VA_ARGS__))
#define MPI_Bcast(...)       CAMPI_AWAIT(::campi::Bcast(__VA_ARGS__))
#define MPI_Reduce(...)      CAMPI_AWAIT(::campi::Reduce(__VA_ARGS__))
#define MPI_Allreduce(...)   CAMPI_AWAIT(::campi::Allreduce(__VA_ARGS__))
#define MPI_Gather(...)      CAMPI_AWAIT(::campi::Gather(__VA_ARGS__))
#define MPI_Gatherv(...)     CAMPI_AWAIT(::campi::Gatherv(__VA_ARGS__))
#define MPI_Allgather(...)   CAMPI_AWAIT(::campi::Allgather(__VA_ARGS__))
#define MPI_Allgatherv(...)  CAMPI_AWAIT(::campi::Allgatherv(__VA_ARGS__))
#define MPI_Scatter(...)     CAMPI_AWAIT(::campi::Scatter(__VA_ARGS__))
#define MPI_Scatterv(...)    CAMPI_AWAIT(::campi::Scatterv(__VA_ARGS__))
#define MPI_Alltoall(...)    CAMPI_AWAIT(::campi::Alltoall(__VA_ARGS__))
#define MPI_Alltoallv(...)   CAMPI_AWAIT(::campi::Alltoallv(__VA_ARGS__))
#define MPI_Scan(...)        CAMPI_AWAIT(::campi::Scan(__VA_ARGS__))
#define MPI_Exscan(...)      CAMPI_AWAIT(::campi::Exscan(__VA_ARGS__))
#define MPI_Comm_split(...)  CAMPI_AWAIT(::campi::Comm_split(__VA_ARGS__))
#define MPI_Comm_dup(...)    CAMPI_AWAIT(::campi::Comm_dup(__VA_ARGS__))
#define MPI_Comm_dup_with_info(...) CAMPI_AWAIT(::campi::Comm_dup_with_info(__VA_ARGS__))
#define MPI_Comm_create(...) CAMPI_AWAIT(::campi::Comm_create(__VA_ARGS__))
#define MPI_Test(...)        CAMPI_AWAIT(::campi::Test(__VA_ARGS__))
#define MPI_Testany(...)     CAMPI_AWAIT(::campi::Testany(__VA_ARGS__))
#define MPI_Testall(...)     CAMPI_AWAIT(::campi::Testall(__VA_ARGS__))
#define MPI_Testsome(...)    CAMPI_AWAIT(::campi::Testsome(__VA_ARGS__))
#define MPI_Request_get_status(...) CAMPI_AWAIT(::campi::Request_get_status(__VA_ARGS__))
#define MPI_Iprobe(...)      CAMPI_AWAIT(::campi::Iprobe(__VA_ARGS__))
#define MPI_Improbe(...)     CAMPI_AWAIT(::campi::Improbe(__VA_ARGS__))
#define AMPI_Migrate(...)    CAMPI_AWAIT(::campi::Migrate(__VA_ARGS__))

/* Routines that block inside AMPI (directly, or through a blocking MPI call,
 * a barrier, an internal waitall, or a [sync] entry method as every one-sided
 * operation and window lock does) and have no campi awaitable yet: a use
 * fails to compile instead of aborting at run time in TCharm::stop. */
#undef MPI_Probe
#undef MPI_Mprobe
#undef MPI_Mrecv
#undef MPI_Waitany
#undef MPI_Waitsome
#undef MPI_Bsend
#undef MPI_Rsend
#undef MPI_Sendrecv_replace
#undef MPI_Reduce_scatter
#undef MPI_Reduce_scatter_block
#undef MPI_Alltoallw
#undef MPI_Intercomm_create
#undef MPI_Intercomm_merge
#undef MPI_Cart_create
#undef MPI_Cart_sub
#undef MPI_Graph_create
#undef MPI_Dist_graph_create
#undef MPI_Dist_graph_create_adjacent
#undef MPI_Comm_split_type
#undef MPI_Comm_idup
#undef MPI_Comm_idup_with_info
#undef MPI_Comm_create_group
#undef MPI_Ireduce_scatter
#undef MPI_Ireduce_scatter_block
#undef MPI_Neighbor_allgather
#undef MPI_Neighbor_allgatherv
#undef MPI_Neighbor_alltoall
#undef MPI_Neighbor_alltoallv
#undef MPI_Neighbor_alltoallw
#undef MPI_Win_create
#undef MPI_Win_allocate
#undef MPI_Win_free
#undef MPI_Win_fence
#undef MPI_Win_lock
#undef MPI_Win_unlock
#undef MPI_Win_lock_all
#undef MPI_Win_unlock_all
#undef MPI_Win_post
#undef MPI_Win_start
#undef MPI_Win_complete
#undef MPI_Win_wait
#undef MPI_Put
#undef MPI_Get
#undef MPI_Accumulate
#undef MPI_Get_accumulate
#undef MPI_Rput
#undef MPI_Rget
#undef MPI_Raccumulate
#undef MPI_Rget_accumulate
#undef MPI_Fetch_and_op
#undef MPI_Compare_and_swap
#undef AMPI_Iget_wait
#undef AMPI_Migrate_to_pe
#undef AMPI_GPU_Invoke
#undef AMPI_GPU_Invoke_wr
#undef AMPI_Yield
#undef AMPI_Suspend
#define MPI_Probe(...)                      campi_unsupported_blocking_call_MPI_Probe
#define MPI_Mprobe(...)                     campi_unsupported_blocking_call_MPI_Mprobe
#define MPI_Mrecv(...)                      campi_unsupported_blocking_call_MPI_Mrecv
#define MPI_Waitany(...)                    campi_unsupported_blocking_call_MPI_Waitany
#define MPI_Waitsome(...)                   campi_unsupported_blocking_call_MPI_Waitsome
#define MPI_Bsend(...)                      campi_unsupported_blocking_call_MPI_Bsend
#define MPI_Rsend(...)                      campi_unsupported_blocking_call_MPI_Rsend
#define MPI_Sendrecv_replace(...)           campi_unsupported_blocking_call_MPI_Sendrecv_replace
#define MPI_Reduce_scatter(...)             campi_unsupported_blocking_call_MPI_Reduce_scatter
#define MPI_Reduce_scatter_block(...)       campi_unsupported_blocking_call_MPI_Reduce_scatter_block
#define MPI_Alltoallw(...)                  campi_unsupported_blocking_call_MPI_Alltoallw
#define MPI_Intercomm_create(...)           campi_unsupported_blocking_call_MPI_Intercomm_create
#define MPI_Intercomm_merge(...)            campi_unsupported_blocking_call_MPI_Intercomm_merge
#define MPI_Cart_create(...)                campi_unsupported_blocking_call_MPI_Cart_create
#define MPI_Cart_sub(...)                   campi_unsupported_blocking_call_MPI_Cart_sub
#define MPI_Graph_create(...)               campi_unsupported_blocking_call_MPI_Graph_create
#define MPI_Dist_graph_create(...)          campi_unsupported_blocking_call_MPI_Dist_graph_create
#define MPI_Dist_graph_create_adjacent(...) campi_unsupported_blocking_call_MPI_Dist_graph_create_adjacent
#define MPI_Comm_split_type(...)            campi_unsupported_blocking_call_MPI_Comm_split_type
#define MPI_Comm_idup(...)                  campi_unsupported_blocking_call_MPI_Comm_idup
#define MPI_Comm_idup_with_info(...)        campi_unsupported_blocking_call_MPI_Comm_idup_with_info
#define MPI_Comm_create_group(...)          campi_unsupported_blocking_call_MPI_Comm_create_group
#define MPI_Ireduce_scatter(...)            campi_unsupported_blocking_call_MPI_Ireduce_scatter
#define MPI_Ireduce_scatter_block(...)      campi_unsupported_blocking_call_MPI_Ireduce_scatter_block
#define MPI_Neighbor_allgather(...)         campi_unsupported_blocking_call_MPI_Neighbor_allgather
#define MPI_Neighbor_allgatherv(...)        campi_unsupported_blocking_call_MPI_Neighbor_allgatherv
#define MPI_Neighbor_alltoall(...)          campi_unsupported_blocking_call_MPI_Neighbor_alltoall
#define MPI_Neighbor_alltoallv(...)         campi_unsupported_blocking_call_MPI_Neighbor_alltoallv
#define MPI_Neighbor_alltoallw(...)         campi_unsupported_blocking_call_MPI_Neighbor_alltoallw
#define MPI_Win_create(...)                 campi_unsupported_blocking_call_MPI_Win_create
#define MPI_Win_allocate(...)               campi_unsupported_blocking_call_MPI_Win_allocate
#define MPI_Win_free(...)                   campi_unsupported_blocking_call_MPI_Win_free
#define MPI_Win_fence(...)                  campi_unsupported_blocking_call_MPI_Win_fence
#define MPI_Win_lock(...)                   campi_unsupported_blocking_call_MPI_Win_lock
#define MPI_Win_unlock(...)                 campi_unsupported_blocking_call_MPI_Win_unlock
#define MPI_Win_lock_all(...)               campi_unsupported_blocking_call_MPI_Win_lock_all
#define MPI_Win_unlock_all(...)             campi_unsupported_blocking_call_MPI_Win_unlock_all
#define MPI_Win_post(...)                   campi_unsupported_blocking_call_MPI_Win_post
#define MPI_Win_start(...)                  campi_unsupported_blocking_call_MPI_Win_start
#define MPI_Win_complete(...)               campi_unsupported_blocking_call_MPI_Win_complete
#define MPI_Win_wait(...)                   campi_unsupported_blocking_call_MPI_Win_wait
#define MPI_Put(...)                        campi_unsupported_blocking_call_MPI_Put
#define MPI_Get(...)                        campi_unsupported_blocking_call_MPI_Get
#define MPI_Accumulate(...)                 campi_unsupported_blocking_call_MPI_Accumulate
#define MPI_Get_accumulate(...)             campi_unsupported_blocking_call_MPI_Get_accumulate
#define MPI_Rput(...)                       campi_unsupported_blocking_call_MPI_Rput
#define MPI_Rget(...)                       campi_unsupported_blocking_call_MPI_Rget
#define MPI_Raccumulate(...)                campi_unsupported_blocking_call_MPI_Raccumulate
#define MPI_Rget_accumulate(...)            campi_unsupported_blocking_call_MPI_Rget_accumulate
#define MPI_Fetch_and_op(...)               campi_unsupported_blocking_call_MPI_Fetch_and_op
#define MPI_Compare_and_swap(...)           campi_unsupported_blocking_call_MPI_Compare_and_swap
#define AMPI_Iget_wait(...)                 campi_unsupported_blocking_call_AMPI_Iget_wait
#define AMPI_Migrate_to_pe(...)             campi_unsupported_blocking_call_AMPI_Migrate_to_pe
#define AMPI_GPU_Invoke(...)                campi_unsupported_blocking_call_AMPI_GPU_Invoke
#define AMPI_GPU_Invoke_wr(...)             campi_unsupported_blocking_call_AMPI_GPU_Invoke_wr
#define AMPI_Yield(...)                     CAMPI_AWAIT(::campi::yield())
#define AMPI_Suspend(...)                   campi_unsupported_blocking_call_AMPI_Suspend

#endif // CAMPI_H
