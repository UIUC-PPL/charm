#include <mpi.h>
#include <getopt.h>
#include <vector>
#include <utility>
#include <string>
#include <sstream>
#include "jacobi3d.h"

#define USE_TIMER 0
#define TIMER_CNT 16384
#define TIMERS_PER_ITER 6
#define DURS_CNT (TIMERS_PER_ITER-1)

#define COMM_ONLY 0

extern void invokeInitKernel(DataType* temperature, int block_width,
    int block_height, int block_depth, cudaStream_t stream);
extern void invokeGhostInitKernels(const std::vector<DataType*>& ghosts,
    const std::vector<int>& ghost_counts, cudaStream_t stream);
extern void invokeBoundaryKernels(DataType* temperature, int block_width,
    int block_height, int block_depth, bool bounds[], cudaStream_t stream);
extern void invokeJacobiKernel(DataType* temperature, DataType* new_temperature,
    DataType* send_left_ghost, DataType* send_right_ghost, DataType* send_top_ghost,
    DataType* send_bottom_ghost, DataType* send_front_ghost, DataType* send_back_ghost,
    DataType* recv_left_ghost, DataType* recv_right_ghost, DataType* recv_top_ghost,
    DataType* recv_bottom_ghost, DataType* recv_front_ghost, DataType* recv_back_ghost,
    bool left_bound, bool right_bound, bool top_bound, bool bottom_bound,
    bool front_bound, bool back_bound, int block_width, int block_height, int block_depth,
    cudaStream_t stream, bool fuse_update_pack, bool fuse_update_all);
extern void invokeJacobiInteriorKernel(DataType* temperature, DataType* new_temperature,
    int block_width, int block_height, int block_depth, cudaStream_t stream);
extern void invokeJacobiBoundaryKernel(DataType* temperature, DataType* new_temperature,
    int block_width, int block_height, int block_depth, cudaStream_t stream);
extern void packGhostsDevice(DataType* temperature,
    DataType* d_ghosts[], DataType* h_ghosts[], bool bounds[],
    int block_width, int block_height, int block_depth,
    size_t x_surf_size, size_t y_surf_size, size_t z_surf_size,
    cudaStream_t comm_stream, cudaStream_t d2h_stream, cudaEvent_t pack_events[],
    bool cuda_aware);
extern void packGhostsFusedDevice(DataType* temperature, DataType* left_ghost,
    DataType* right_ghost, DataType* top_ghost, DataType* bottom_ghost,
    DataType* front_ghost, DataType* back_ghost, bool left_bound, bool right_bound,
    bool top_bound, bool bottom_bound, bool front_bound, bool back_bound,
    int block_width, int block_height, int block_depth, cudaStream_t comm_stream);
extern void unpackGhostDevice(DataType* temperature, DataType* d_ghost, DataType* h_ghost,
    int dir, int block_width, int block_height, int block_depth, size_t ghost_size,
    cudaStream_t comm_stream, cudaStream_t h2d_stream, cudaEvent_t unpack_events[],
    bool cuda_aware);
extern void unpackGhostsFusedDevice(DataType* temperature, DataType* left_ghost,
    DataType* right_ghost, DataType* top_ghost, DataType* bottom_ghost,
    DataType* front_ghost, DataType* back_ghost, bool left_bound, bool right_bound,
    bool top_bound, bool bottom_bound, bool front_bound, bool back_bound,
    int block_width, int block_height, int block_depth, cudaStream_t comm_stream);
extern void setUnpackNode(cudaKernelNodeParams& params, DataType* temperature,
  DataType* ghost, int dir, int block_width, int block_height, int block_depth);
extern void setUnpackFusedNode(cudaKernelNodeParams& params, DataType* temperature,
    DataType* ghosts[], bool bounds[], int block_width, int block_height,
    int block_depth);
extern void setUpdateNode(cudaKernelNodeParams& params, DataType* temperature,
    DataType* new_temperature, DataType* send_ghosts[], DataType* recv_ghosts[],
    bool bounds[], int block_width, int block_height, int block_depth,
    bool fuse_update_pack, bool fuse_update_all);
extern void setPackNode(cudaKernelNodeParams& params, DataType* temperature,
  DataType* ghost, int dir, int block_width, int block_height, int block_depth);
extern void setPackFusedNode(cudaKernelNodeParams& params, DataType* temperature,
    DataType* ghosts[], bool bounds[], int block_width, int block_height,
    int block_depth);

// Program parameters
struct Param {
  int n_procs;
  int rank;
  MPI_Comm cart_comm;

  int grid_width;
  int grid_height;
  int grid_depth;
  int block_width;
  int block_height;
  int block_depth;
  int n_procs_x;
  int n_procs_y;
  int n_procs_z;
  int n_iters;
  int warmup_iters;
  bool cuda_aware;
  bool manual_overlap;
  int fuse_val;
  bool fuse_pack;
  bool fuse_unpack;
  bool fuse_update_pack;
  bool fuse_update_all;
  bool use_cuda_graph;
  bool print_elements;

  Param(int n_procs_, int rank_) : n_procs(n_procs_), rank(rank_) {
    grid_width = grid_height = grid_depth = 512;
    n_iters = 100;
    warmup_iters = 10;
    cuda_aware = false;
    manual_overlap = false;
    fuse_val = 0;
    fuse_pack = false;
    fuse_unpack = false;
    fuse_update_pack = false;
    fuse_update_all = false;
    use_cuda_graph = false;
    print_elements = false;
  }

  void set() {
    // Calculate block size
    block_width  = grid_width  / n_procs_x;
    block_height = grid_height / n_procs_y;
    block_depth  = grid_depth  / n_procs_z;
  }

  void print() {
    printf("\n[CUDA 3D Jacobi example]\n");
    printf("Grid: %d x %d x %d, Block: %d x %d x %d, Procs: %d x %d x %d, "
        "Iterations: %d, Warm-up: %d, CUDA-aware: %d, Manual overlap: %d, "
        "Fusion: %d, CUDA Graph: %d, Print: %d\n\n",
        grid_width, grid_height, grid_depth, block_width, block_height, block_depth,
        n_procs_x, n_procs_y, n_procs_z, n_iters, warmup_iters, cuda_aware,
        manual_overlap, fuse_val, use_cuda_graph, print_elements);
  }
};

struct Block {
  int n_procs;
  int rank;
  MPI_Comm cart_comm;

  int grid_width;
  int grid_height;
  int grid_depth;
  int block_width;
  int block_height;
  int block_depth;
  int n_procs_x;
  int n_procs_y;
  int n_procs_z;
  int n_iters;
  int warmup_iters;
  bool cuda_aware;
  bool manual_overlap;
  bool fuse_pack;
  bool fuse_unpack;
  bool fuse_update_pack;
  bool fuse_update_all;
  bool use_cuda_graph;
  bool print_elements;

  int x_surf_count;
  int y_surf_count;
  int z_surf_count;
  size_t x_surf_size;
  size_t y_surf_size;
  size_t z_surf_size;

  int my_iter;
  int neighbor_ranks[DIR_COUNT];
  int n_nbr;
  int n_low_nbr;
  int n_high_nbr;
  int nbr_count;
  int x, y, z;
  int linear_index;

  DataType* h_temperature;
  DataType* d_temperature;
  DataType* d_new_temperature;

  DataType** h_send_ghosts;
  DataType** h_recv_ghosts;
  DataType** d_send_ghosts;
  DataType** d_recv_ghosts;
  size_t ghost_sizes[DIR_COUNT];

  cudaStream_t compute_stream;
  cudaStream_t comm_stream;
  cudaStream_t h2d_stream;
  cudaStream_t d2h_stream;
  cudaStream_t graph_stream;
  cudaGraph_t cuda_graph_1;
  cudaGraph_t cuda_graph_2;
  cudaGraphExec_t cuda_graph_exec_1;
  cudaGraphExec_t cuda_graph_exec_2;
  cudaGraphExec_t* cuda_graph_exec;
  cudaGraphExec_t* cuda_graph_exec_next;
  /*
  cudaGraphNode_t unpack_nodes[DIR_COUNT];
  cudaGraphNode_t fuse_unpack_node;
  cudaGraphNode_t update_node;
  cudaGraphNode_t pack_nodes[DIR_COUNT];
  cudaGraphNode_t fuse_pack_node;
  */

  cudaEvent_t compute_event;
  cudaEvent_t comm_event;
  cudaEvent_t pack_events[DIR_COUNT];
  cudaEvent_t unpack_events[DIR_COUNT];

  bool* bounds;

  Block(Param param, int x_, int y_, int z_)
    : x(x_), y(y_), z(z_) {
    // Initialize values using Param object
    n_procs          = param.n_procs;
    rank             = param.rank;
    cart_comm        = param.cart_comm;
    grid_width       = param.grid_width;
    grid_height      = param.grid_height;
    grid_depth       = param.grid_depth;
    block_width      = param.block_width;
    block_height     = param.block_height;
    block_depth      = param.block_depth;
    n_procs_x        = param.n_procs_x;
    n_procs_y        = param.n_procs_y;
    n_procs_z        = param.n_procs_z;
    n_iters          = param.n_iters;
    warmup_iters     = param.warmup_iters;
    cuda_aware       = param.cuda_aware;
    manual_overlap   = param.manual_overlap;
    fuse_pack        = param.fuse_pack;
    fuse_unpack      = param.fuse_unpack;
    fuse_update_pack = param.fuse_update_pack;
    fuse_update_all  = param.fuse_update_all;
    use_cuda_graph   = param.use_cuda_graph;
    print_elements   = param.print_elements;

    // Calculate surface count and sizes
    x_surf_count = block_height * block_depth;
    y_surf_count = block_width  * block_depth;
    z_surf_count = block_width  * block_height;
    x_surf_size  = x_surf_count * sizeof(DataType);
    y_surf_size  = y_surf_count * sizeof(DataType);
    z_surf_size  = z_surf_count * sizeof(DataType);

    // Initialize values
    my_iter = 0;
    n_nbr = 0;
    n_low_nbr = 0;
    n_high_nbr = 0;
    linear_index = x * n_procs_y * n_procs_z + y * n_procs_z + z;

    // Check bounds and set number of valid neighbors
    cudaCheck(cudaMallocHost((void**)&bounds, sizeof(bool) * DIR_COUNT));
    for (int i = 0; i < DIR_COUNT; i++) bounds[i] = false;

    if (x == 0)           bounds[LEFT] = true;
    else                  { n_nbr++; n_low_nbr++; }
    if (x == n_procs_x-1) bounds[RIGHT] = true;
    else                  { n_nbr++; n_high_nbr++; }
    if (y == 0)           bounds[TOP] = true;
    else                  { n_nbr++; n_low_nbr++; }
    if (y == n_procs_y-1) bounds[BOTTOM] = true;
    else                  { n_nbr++; n_high_nbr++; }
    if (z == 0)           bounds[FRONT] = true;
    else                  { n_nbr++; n_low_nbr++; }
    if (z == n_procs_z-1) bounds[BACK] = true;
    else                  { n_nbr++; n_high_nbr++; }

    // Determine neighbor ranks
    for (int i = 0; i < DIR_COUNT; i++) {
      neighbor_ranks[i] = MPI_PROC_NULL;
    }
    if (!bounds[LEFT]) {
      int coords[NDIMS] = {x-1, y, z};
      MPI_Cart_rank(param.cart_comm, coords, &neighbor_ranks[LEFT]);
    }
    if (!bounds[RIGHT]) {
      int coords[NDIMS] = {x+1, y, z};
      MPI_Cart_rank(param.cart_comm, coords, &neighbor_ranks[RIGHT]);
    }
    if (!bounds[TOP]) {
      int coords[NDIMS] = {x, y-1, z};
      MPI_Cart_rank(param.cart_comm, coords, &neighbor_ranks[TOP]);
    }
    if (!bounds[BOTTOM]) {
      int coords[NDIMS] = {x, y+1, z};
      MPI_Cart_rank(param.cart_comm, coords, &neighbor_ranks[BOTTOM]);
    }
    if (!bounds[FRONT]) {
      int coords[NDIMS] = {x, y, z-1};
      MPI_Cart_rank(param.cart_comm, coords, &neighbor_ranks[FRONT]);
    }
    if (!bounds[BACK]) {
      int coords[NDIMS] = {x, y, z+1};
      MPI_Cart_rank(param.cart_comm, coords, &neighbor_ranks[BACK]);
    }

    // Allocate memory and create CUDA entities
    cudaCheck(cudaMallocHost((void**)&h_temperature,
          sizeof(DataType) * (block_width+2) * (block_height+2) * (block_depth+2)));
    cudaCheck(cudaMalloc((void**)&d_temperature,
          sizeof(DataType) * (block_width+2) * (block_height+2) * (block_depth+2)));
    cudaCheck(cudaMalloc((void**)&d_new_temperature,
          sizeof(DataType) * (block_width+2) * (block_height+2) * (block_depth+2)));
    ghost_sizes[LEFT] = x_surf_size;
    ghost_sizes[RIGHT] = x_surf_size;
    ghost_sizes[TOP] = y_surf_size;
    ghost_sizes[BOTTOM] = y_surf_size;
    ghost_sizes[FRONT] = z_surf_size;
    ghost_sizes[BACK] = z_surf_size;
    cudaCheck(cudaMallocHost((void**)&h_send_ghosts, sizeof(DataType*) * DIR_COUNT));
    cudaCheck(cudaMallocHost((void**)&h_recv_ghosts, sizeof(DataType*) * DIR_COUNT));
    cudaCheck(cudaMallocHost((void**)&d_send_ghosts, sizeof(DataType*) * DIR_COUNT));
    cudaCheck(cudaMallocHost((void**)&d_recv_ghosts, sizeof(DataType*) * DIR_COUNT));
    for (int i = 0; i < DIR_COUNT; i++) {
      cudaCheck(cudaMallocHost((void**)&h_send_ghosts[i], ghost_sizes[i]));
      cudaCheck(cudaMallocHost((void**)&h_recv_ghosts[i], ghost_sizes[i]));
      cudaCheck(cudaMalloc((void**)&d_send_ghosts[i], ghost_sizes[i]));
      cudaCheck(cudaMalloc((void**)&d_recv_ghosts[i], ghost_sizes[i]));
    }

    // Create CUDA streams and events
    cudaCheck(cudaStreamCreateWithPriority(&compute_stream, cudaStreamDefault, 0));
    cudaCheck(cudaStreamCreateWithPriority(&comm_stream, cudaStreamDefault, -1));
    cudaCheck(cudaStreamCreateWithPriority(&h2d_stream, cudaStreamDefault, -1));
    cudaCheck(cudaStreamCreateWithPriority(&d2h_stream, cudaStreamDefault, -1));
    cudaCheck(cudaStreamCreateWithFlags(&graph_stream, cudaStreamNonBlocking));

    cudaCheck(cudaEventCreateWithFlags(&compute_event, cudaEventDisableTiming));
    cudaCheck(cudaEventCreateWithFlags(&comm_event, cudaEventDisableTiming));
    for (int i = 0; i < DIR_COUNT; i++) {
      cudaCheck(cudaEventCreateWithFlags(&pack_events[i], cudaEventDisableTiming));
      cudaCheck(cudaEventCreateWithFlags(&unpack_events[i], cudaEventDisableTiming));
    }

    // Initialize temperature data
    invokeInitKernel(d_temperature, block_width, block_height, block_depth, compute_stream);
    invokeInitKernel(d_new_temperature, block_width, block_height, block_depth, compute_stream);

    // Initialize ghost data
    std::vector<int> ghost_counts = {x_surf_count, x_surf_count, y_surf_count,
      y_surf_count, z_surf_count, z_surf_count};
    std::vector<DataType*> send_ghosts;
    std::vector<DataType*> recv_ghosts;
    for (int i = 0; i < DIR_COUNT; i++) {
      send_ghosts.push_back(d_send_ghosts[i]);
      recv_ghosts.push_back(d_recv_ghosts[i]);
    }
    invokeGhostInitKernels(send_ghosts, ghost_counts, compute_stream);
    invokeGhostInitKernels(recv_ghosts, ghost_counts, compute_stream);

    // Enforce boundary conditions
    invokeBoundaryKernels(d_temperature, block_width, block_height, block_depth,
        bounds, compute_stream);
    invokeBoundaryKernels(d_new_temperature, block_width, block_height, block_depth,
        bounds, compute_stream);

    cudaStreamSynchronize(compute_stream);
  }

  ~Block() {
    cudaCheck(cudaFreeHost(h_temperature));
    cudaCheck(cudaFree(d_temperature));
    cudaCheck(cudaFree(d_new_temperature));
    for (int i = 0; i < DIR_COUNT; i++) {
      cudaCheck(cudaFreeHost(h_send_ghosts[i]));
      cudaCheck(cudaFreeHost(h_recv_ghosts[i]));
      cudaCheck(cudaFree(d_send_ghosts[i]));
      cudaCheck(cudaFree(d_recv_ghosts[i]));
    }
    cudaCheck(cudaFreeHost(h_send_ghosts));
    cudaCheck(cudaFreeHost(h_recv_ghosts));
    cudaCheck(cudaFreeHost(d_send_ghosts));
    cudaCheck(cudaFreeHost(d_recv_ghosts));
    cudaCheck(cudaFreeHost(bounds));

    cudaCheck(cudaStreamDestroy(compute_stream));
    cudaCheck(cudaStreamDestroy(comm_stream));
    cudaCheck(cudaStreamDestroy(h2d_stream));
    cudaCheck(cudaStreamDestroy(d2h_stream));
    cudaCheck(cudaStreamDestroy(graph_stream));

    cudaCheck(cudaEventDestroy(compute_event));
    cudaCheck(cudaEventDestroy(comm_event));
    for (int i = 0; i < DIR_COUNT; i++) {
      cudaCheck(cudaEventDestroy(pack_events[i]));
      cudaCheck(cudaEventDestroy(unpack_events[i]));
    }
  }

  // FIXME
  void createCudaGraph(cudaGraph_t& cg, cudaGraphExec_t& cge,
      DataType* d_temp, DataType* d_new_temp) {
    /*
    std::vector<cudaGraphNode_t> dep1;
    std::vector<cudaGraphNode_t> dep2;
    std::vector<cudaGraphNode_t>* dep = &dep1;
    std::vector<cudaGraphNode_t>* new_dep = &dep2;

    cudaCheck(cudaGraphCreate(&cuda_graph, 0));
    */

    // Start capturing
    if (!fuse_update_all) {
      cudaCheck(cudaStreamBeginCapture(comm_stream, cudaStreamCaptureModeGlobal));
    } else {
      cudaCheck(cudaStreamBeginCapture(compute_stream, cudaStreamCaptureModeGlobal));
    }

    // Unpack
    if (!fuse_update_all) {
      if (fuse_unpack) {
        unpackGhostsFusedDevice(d_temp, d_recv_ghosts[LEFT], d_recv_ghosts[RIGHT],
            d_recv_ghosts[TOP], d_recv_ghosts[BOTTOM], d_recv_ghosts[FRONT],
            d_recv_ghosts[BACK], bounds[LEFT], bounds[RIGHT], bounds[TOP], bounds[BOTTOM],
            bounds[FRONT], bounds[BACK], block_width, block_height, block_depth, comm_stream);
        /*
        cudaKernelNodeParams fu_params = {0};
        setUnpackFusedNode(fu_params, d_temperature, d_recv_ghosts, bounds,
            block_width, block_height, block_depth);
        cudaCheck(cudaGraphAddKernelNode(&fuse_unpack_node, cuda_graph,
              dep->data(), dep->size(), &fu_params));
        new_dep->push_back(fuse_unpack_node);
        */
      } else {
        for (int dir = 0; dir < DIR_COUNT; dir++) {
          if (!bounds[dir]) {
            unpackGhostDevice(d_temp, d_recv_ghosts[dir], nullptr, dir,
                block_width, block_height, block_depth, ghost_sizes[dir],
                comm_stream, h2d_stream, unpack_events, cuda_aware);
            /*
            cudaKernelNodeParams u_params = {0};
            setUnpackNode(u_params, d_temperature, d_recv_ghosts[dir], dir,
                block_width, block_height, block_depth);
            cudaCheck(cudaGraphAddKernelNode(&unpack_nodes[dir], cuda_graph,
                  dep->data(), dep->size(), &u_params));
            new_dep->push_back(unpack_nodes[dir]);
            */
          }
        }
      }

      cudaCheck(cudaEventRecord(comm_event, comm_stream));
      cudaCheck(cudaStreamWaitEvent(compute_stream, comm_event, 0));
    }

    /*
    dep->clear();
    std::swap(dep, new_dep);
    */

    // Jacobi update
    invokeJacobiKernel(d_temp, d_new_temp, d_send_ghosts[LEFT],
        d_send_ghosts[RIGHT], d_send_ghosts[TOP], d_send_ghosts[BOTTOM],
        d_send_ghosts[FRONT], d_send_ghosts[BACK], d_recv_ghosts[LEFT],
        d_recv_ghosts[RIGHT], d_recv_ghosts[TOP], d_recv_ghosts[BOTTOM],
        d_recv_ghosts[FRONT], d_recv_ghosts[BACK], bounds[LEFT], bounds[RIGHT],
        bounds[TOP], bounds[BOTTOM], bounds[FRONT], bounds[BACK],
        block_width, block_height, block_depth, compute_stream,
        fuse_update_pack, fuse_update_all);
    /*
    cudaKernelNodeParams j_params = {0};
    setUpdateNode(j_params, d_temperature, d_new_temperature, d_send_ghosts,
        d_recv_ghosts, bounds, block_width, block_height, block_depth,
        fuse_update_pack, fuse_update_all);
    cudaCheck(cudaGraphAddKernelNode(&update_node, cuda_graph,
          dep->data(), dep->size(), &j_params));
    new_dep->push_back(update_node);

    dep->clear();
    std::swap(dep, new_dep);
    */

    // Pack
    if (!fuse_update_all) {
      cudaEventRecord(compute_event, compute_stream);
      cudaStreamWaitEvent(comm_stream, compute_event, 0);

      if (fuse_pack) {
        packGhostsFusedDevice(d_new_temp, d_send_ghosts[LEFT], d_send_ghosts[RIGHT],
            d_send_ghosts[TOP], d_send_ghosts[BOTTOM], d_send_ghosts[FRONT],
            d_send_ghosts[BACK], bounds[LEFT], bounds[RIGHT], bounds[TOP], bounds[BOTTOM],
            bounds[FRONT], bounds[BACK], block_width, block_height, block_depth, comm_stream);
        /*
        cudaKernelNodeParams fp_params = {0};
        setPackFusedNode(fp_params, d_new_temperature, d_send_ghosts, bounds,
            block_width, block_height, block_depth);
        cudaCheck(cudaGraphAddKernelNode(&fuse_pack_node, cuda_graph,
              dep->data(), dep->size(), &fp_params));
        new_dep->push_back(fuse_pack_node);
        */
      } else if (!fuse_update_pack) {
        packGhostsDevice(d_new_temp, d_send_ghosts, h_send_ghosts, bounds,
            block_width, block_height, block_depth, x_surf_size, y_surf_size, z_surf_size,
            comm_stream, d2h_stream, pack_events, cuda_aware);
        /*
        for (int dir = 0; dir < DIR_COUNT; dir++) {
          if (!bounds[dir]) {
            cudaKernelNodeParams p_params = {0};
            setPackNode(p_params, d_new_temperature, d_send_ghosts[dir], dir,
                block_width, block_height, block_depth);
            cudaCheck(cudaGraphAddKernelNode(&pack_nodes[dir], cuda_graph,
                  dep->data(), dep->size(), &p_params));
            new_dep->push_back(pack_nodes[dir]);
          }
        }
        */
      }
    }

    /*
    dep->clear();
    std::swap(dep, new_dep);
    */

    // End capturing
    if (!fuse_update_all) {
      cudaCheck(cudaStreamEndCapture(comm_stream, &cg));
    } else {
      cudaCheck(cudaStreamEndCapture(compute_stream, &cg));
    }

    // Instantiate CUDA graph
    cudaCheck(cudaGraphInstantiate(&cge, cg, NULL, NULL, 0));
  }

  void createCudaGraphs() {
    createCudaGraph(cuda_graph_1, cuda_graph_exec_1, d_temperature, d_new_temperature);
    createCudaGraph(cuda_graph_2, cuda_graph_exec_2, d_new_temperature, d_temperature);
    cuda_graph_exec = &cuda_graph_exec_1;
    cuda_graph_exec_next = &cuda_graph_exec_2;
  }

  void launchCudaGraph() {
    std::swap(cuda_graph_exec, cuda_graph_exec_next);
    cudaCheck(cudaGraphLaunch(*cuda_graph_exec, graph_stream));
  }

  void packGhosts() {
    if (!use_cuda_graph) {
      if (!fuse_update_all) {
        // Packing must start only after update is complete on the device
        cudaEventRecord(compute_event, compute_stream);
        cudaStreamWaitEvent(comm_stream, compute_event, 0);

        if (fuse_pack) {
          packGhostsFusedDevice(d_new_temperature, d_send_ghosts[LEFT], d_send_ghosts[RIGHT],
              d_send_ghosts[TOP], d_send_ghosts[BOTTOM], d_send_ghosts[FRONT],
              d_send_ghosts[BACK], bounds[LEFT], bounds[RIGHT], bounds[TOP], bounds[BOTTOM],
              bounds[FRONT], bounds[BACK], block_width, block_height, block_depth, comm_stream);
        } else if (!fuse_update_pack) {
          // Pack non-contiguous ghosts to temporary contiguous buffers on the device
          // and transfer each from device to host
          packGhostsDevice(d_new_temperature, d_send_ghosts, h_send_ghosts, bounds,
              block_width, block_height, block_depth, x_surf_size, y_surf_size, z_surf_size,
              comm_stream, d2h_stream, pack_events, cuda_aware);
        }
      }

      if (manual_overlap) {
        // Invoke GPU kernel for interior
        invokeJacobiInteriorKernel(d_temperature, d_new_temperature,
            block_width, block_height, block_depth, compute_stream);
      }

      // Wait for packing to complete
      if (cuda_aware) {
        if (fuse_update_pack || fuse_update_all) {
          cudaStreamSynchronize(compute_stream);
        } else {
          cudaStreamSynchronize(comm_stream);
        }
      } else {
        cudaStreamSynchronize(d2h_stream);
      }
    } else {
      // Communication should follow completion of CUDA graph
      cudaStreamSynchronize(graph_stream);
    }
  }

  void exchangeGhosts() {
#if USE_TIMER
    static double timers[TIMER_CNT];
    static int timer_idx = 0;

    if (my_iter == 0) {
      for (int i = 0; i < TIMER_CNT; i++) {
        timers[i] = 0.0;
      }
    }
#endif

    // Increment iteration count and swap data pointers
    // to avoid host synchronization
    my_iter++;
    std::swap(d_temperature, d_new_temperature);

    // Data sizes
    size_t data_sizes[DIR_COUNT] = {x_surf_size, x_surf_size, y_surf_size,
      y_surf_size, z_surf_size, z_surf_size};

#if USE_TIMER
    timers[timer_idx++] = MPI_Wtime();
#endif

		// Send ghosts to neighbors
    MPI_Request send_requests[DIR_COUNT];
    int send_count = 0;
    for (int dir = 0; dir < DIR_COUNT; dir++) {
      DataType* send_ghost = cuda_aware ? d_send_ghosts[dir] : h_send_ghosts[dir];
      int rev_dir = (dir % 2 == 0) ? (dir + 1) : (dir - 1);
      if (!bounds[dir])
        MPI_Isend(send_ghost, data_sizes[dir], MPI_CHAR, neighbor_ranks[dir],
            my_iter * DIR_COUNT + rev_dir, cart_comm, &send_requests[send_count++]);
    }

#if USE_TIMER
    timers[timer_idx++] = MPI_Wtime();
#endif

    // Receive ghosts from neighbors
    MPI_Request recv_requests[DIR_COUNT];
    int recv_count = 0;
    int recv_dirs[DIR_COUNT];
    for (int dir = 0; dir < DIR_COUNT; dir++) {
      DataType* recv_ghost = cuda_aware ? d_recv_ghosts[dir] : h_recv_ghosts[dir];
      if (!bounds[dir]) {
        MPI_Irecv(recv_ghost, data_sizes[dir], MPI_CHAR, neighbor_ranks[dir],
            my_iter * DIR_COUNT + dir, cart_comm, &recv_requests[recv_count]);
        recv_dirs[recv_count++] = dir;
      }
    }

#if USE_TIMER
    timers[timer_idx++] = MPI_Wtime();
#endif

    // Invoke unpacking kernel for each received ghost
    MPI_Status recv_statuses[DIR_COUNT];
    for (int i = 0; i < recv_count; i++) {
      int index;
      // Wait for a ghost to arrive
      MPI_Waitany(recv_count, recv_requests, &index, recv_statuses);
      int dir = recv_dirs[index];

#if USE_TIMER
      if (i == 0 || i == recv_count-1) {
        timers[timer_idx++] = MPI_Wtime();
      }
#endif

#if !COMM_ONLY
      if (!use_cuda_graph) {
        if (!fuse_update_all && !fuse_unpack) {
          unpackGhostDevice(d_temperature, d_recv_ghosts[dir], h_recv_ghosts[dir], dir,
              block_width, block_height, block_depth, ghost_sizes[dir],
              comm_stream, h2d_stream, unpack_events, cuda_aware);
        }
      }
#endif
    }

    if (fuse_unpack) {
      // Unpack all ghosts together
      unpackGhostsFusedDevice(d_temperature, d_recv_ghosts[LEFT], d_recv_ghosts[RIGHT],
          d_recv_ghosts[TOP], d_recv_ghosts[BOTTOM], d_recv_ghosts[FRONT],
          d_recv_ghosts[BACK], bounds[LEFT], bounds[RIGHT], bounds[TOP], bounds[BOTTOM],
          bounds[FRONT], bounds[BACK], block_width, block_height, block_depth, comm_stream);
    }

    // Wait for sends to complete
    MPI_Status send_statuses[DIR_COUNT];
    MPI_Waitall(send_count, send_requests, send_statuses);

#if USE_TIMER
    timers[timer_idx++] = MPI_Wtime();

    if (my_iter == warmup_iters + n_iters) {
      double durations[DURS_CNT];

      for (int i = 0; i < DURS_CNT; i++) {
        durations[i] = 0.0;
      }

      for (int i = warmup_iters; i < (warmup_iters + n_iters); i++) {
        int idx = i * TIMERS_PER_ITER;
        for (int j = 0; j < DURS_CNT; j++) {
          durations[j] += (timers[idx+j+1] - timers[idx+j]);
        }
      }

      for (int i = 0; i < DURS_CNT; i++) {
        durations[i] /= n_iters;
        if (rank == 0) {
          printf("Durations %d: %.3lf us\n", i, durations[i] * 1e6);
        }
      }
    }
#endif
  }

  void update() {
    if (!use_cuda_graph) {
      if (!fuse_update_all) {
        // Update should only be performed after operations in communication stream
        // (transfers and unpacking) complete
        cudaCheck(cudaEventRecord(comm_event, comm_stream));
        cudaCheck(cudaStreamWaitEvent(compute_stream, comm_event, 0));
      }

      if (!manual_overlap) {
        // Invoke GPU kernel for Jacobi computation
        invokeJacobiKernel(d_temperature, d_new_temperature, d_send_ghosts[LEFT],
            d_send_ghosts[RIGHT], d_send_ghosts[TOP], d_send_ghosts[BOTTOM],
            d_send_ghosts[FRONT], d_send_ghosts[BACK], d_recv_ghosts[LEFT],
            d_recv_ghosts[RIGHT], d_recv_ghosts[TOP], d_recv_ghosts[BOTTOM],
            d_recv_ghosts[FRONT], d_recv_ghosts[BACK], bounds[LEFT], bounds[RIGHT],
            bounds[TOP], bounds[BOTTOM], bounds[FRONT], bounds[BACK],
            block_width, block_height, block_depth, compute_stream,
            fuse_update_pack, fuse_update_all);
      } else {
        // Invoke GPU kernel for boundary
        invokeJacobiBoundaryKernel(d_temperature, d_new_temperature,
            block_width, block_height, block_depth, compute_stream);
      }
    }

    // Synchronize with host only when necessary
    if (print_elements) {
      if (use_cuda_graph) {
        cudaStreamSynchronize(graph_stream);
      } else {
        cudaStreamSynchronize(compute_stream);
      }
      if (rank == 0) {
        print();
      }
    }
  }

  void print() {
    printf("Printing iteration %d\n", my_iter);

    // Move data from device to host for printing
    cudaCheck(cudaMemcpyAsync(h_temperature, d_temperature,
          sizeof(DataType) * (block_width+2)*(block_height+2)*(block_depth+2),
          cudaMemcpyDeviceToHost, comm_stream));
    cudaStreamSynchronize(comm_stream);

    printf("[%d,%d,%d]\n", x, y, z);
    for (int k = 0; k < block_depth+2; k++) {
      for (int j = 0; j < block_height+2; j++) {
        for (int i = 0; i < block_width+2; i++) {
#ifdef TEST_CORRECTNESS
          printf("%d ", h_temperature[IDX(i,j,k)]);
#else
          printf("%.6lf ", h_temperature[IDX(i,j,k)]);
#endif
        }
        printf("\n");
      }
      printf("\n");
    }
  }
};

int main(int argc, char** argv) {
  // Initialize MPI
  int n_procs;
  int rank;
  MPI_Init(&argc, &argv);
  MPI_Comm_size(MPI_COMM_WORLD, &n_procs);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  // Create parameter object
  Param param(n_procs, rank);

  // Process arguments
  int c;
  bool dims[3] = {false, false, false};
  while ((c = getopt(argc, argv, "x:y:z:i:w:dmf:gp")) != -1) {
    switch (c) {
      case 'x':
        param.grid_width = atoi(optarg);
        dims[0] = true;
        break;
      case 'y':
        param.grid_height = atoi(optarg);
        dims[1] = true;
        break;
      case 'z':
        param.grid_depth = atoi(optarg);
        dims[2] = true;
        break;
      case 'i':
        param.n_iters = atoi(optarg);
        break;
      case 'w':
        param.warmup_iters = atoi(optarg);
        break;
      case 'd':
        param.cuda_aware = true;
        break;
      case 'm':
        param.manual_overlap = true;
        break;
      case 'f':
        param.fuse_val = atoi(optarg);
        if (param.fuse_val == 1) param.fuse_pack = true;
        else if (param.fuse_val == 2) param.fuse_unpack = true;
        else if (param.fuse_val == 3) param.fuse_pack = param.fuse_unpack = true;
        else if (param.fuse_val == 4) param.fuse_update_pack = true;
        else if (param.fuse_val == 5) param.fuse_update_all = true;
        else {
          fprintf(stderr, "ERROR: Invalid fusion value: %d\n", param.fuse_val);
          MPI_Finalize();
          exit(EXIT_FAILURE);
        }
        break;
      case 'g':
        param.use_cuda_graph = true;
        break;
      case 'p':
        param.print_elements = true;
        break;
      default:
        if (rank == 0) {
          fprintf(stderr,
              "Usage: %s -x [grid width] -y [grid height] -z [grid depth] "
              "-i [iterations] -w [warmup iterations] -d [use CUDA-aware MPI] "
              "-m [use manual overlap] -f [fusion value] -g [use CUDA Graph] "
              "-p (print blocks)\n", argv[0]);
        }
        MPI_Finalize();
        exit(EXIT_FAILURE);
    }
  }

  // Kernel fusion can only be used with CUDA-aware MPI
  if (param.fuse_val != 0 && !param.cuda_aware) {
    fprintf(stderr, "ERROR: Kernel fusion can only be used with CUDA-aware MPI\n");
    MPI_Finalize();
    exit(EXIT_FAILURE);
  }

  // CUDA Graph can only be used with CUDA-aware MPI
  if (param.use_cuda_graph && !param.cuda_aware) {
    fprintf(stderr, "ERROR: CUDA Graph can only be used with CUDA-aware MPI\n");
    MPI_Finalize();
    exit(EXIT_FAILURE);
  }

  // If only the X dimension is given, use it for Y and Z as well
  if (dims[0] && !dims[1] && !dims[2]) {
    param.grid_height = param.grid_depth = param.grid_width;
  }

  // Setup 3D grid of processes
  double area[3];
  int ipx, ipy, ipz, nremain;
  double surf, bestsurf;
  area[0] = param.grid_width  * param.grid_height;
  area[1] = param.grid_width  * param.grid_depth;
  area[2] = param.grid_height * param.grid_depth;
  bestsurf = 2.0 * (area[0] + area[1] + area[2]);
  ipx = 1;
  while (ipx <= param.n_procs) {
    if (param.n_procs % ipx == 0) {
      nremain = param.n_procs / ipx;
      ipy = 1;

      while (ipy <= nremain) {
        if (nremain % ipy == 0) {
          ipz = nremain / ipy;
          surf = area[0] / ipx / ipy + area[1] / ipx / ipz + area[2] / ipy / ipz;

          if (surf < bestsurf) {
            bestsurf = surf;
            param.n_procs_x = ipx;
            param.n_procs_y = ipy;
            param.n_procs_z = ipz;
          }
        }
        ipy++;
      }
    }
    ipx++;
  }

  if (param.n_procs_x * param.n_procs_y * param.n_procs_z != param.n_procs) {
    if (rank == 0) {
      fprintf(stderr, "ERROR: Bad grid of MPI processes: %d x %d x %d != %d\n",
          param.n_procs_x, param.n_procs_y, param.n_procs_z, param.n_procs);
    }
    MPI_Finalize();
    exit(EXIT_FAILURE);
  }

  // Print configuration
  param.set();
  if (rank == 0) {
    param.print();
  }

  // Create 3D Cartesian topology and Block object
  int proc_dims[NDIMS] = {param.n_procs_x, param.n_procs_y, param.n_procs_z};
  int periods[NDIMS] = {0, 0, 0};
  int coords[NDIMS];
  MPI_Cart_create(MPI_COMM_WORLD, NDIMS, proc_dims, periods, 0, &param.cart_comm);
  MPI_Cart_coords(param.cart_comm, rank, NDIMS, coords);

	// Initialize block
  double init_start_time = MPI_Wtime();
	if (rank == 0) printf("Initializing blocks...\n");
  Block block(param, coords[0], coords[1], coords[2]);
  if (param.use_cuda_graph) {
    block.createCudaGraphs();
  }
  MPI_Barrier(MPI_COMM_WORLD);
  if (rank == 0) printf("Blocks initialized in %.3lf s\n", MPI_Wtime() - init_start_time);

  // Main iteration loop
  if (rank == 0) printf("Running...\n");
  double start_time;
  for (int i = 0; i < param.n_iters + param.warmup_iters; i++) {
    if (i == param.warmup_iters) start_time = MPI_Wtime();

#if !COMM_ONLY
    block.packGhosts();
#endif
    block.exchangeGhosts();
    if (param.use_cuda_graph) block.launchCudaGraph();
#if !COMM_ONLY
    block.update();
#endif
  }
  double total_time = MPI_Wtime() - start_time;

  // Finalize
  if (rank == 0) {
    printf("Iterations complete!\n\nTotal time: %.3lf s\nAverage iteration time: %.3lf ms\n",
        total_time, (total_time / param.n_iters) * 1e3);
  }

  MPI_Barrier(MPI_COMM_WORLD);

  MPI_Finalize();

  return EXIT_SUCCESS;
}
