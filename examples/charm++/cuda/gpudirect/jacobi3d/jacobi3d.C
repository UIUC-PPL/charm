#include "hapi.h"
#include "hapi_nvtx.h"
#include "jacobi3d.decl.h"
#include "jacobi3d.h"
#include <utility>
#include <string>
#include <sstream>

#define CUDA_SYNC 0
#define USE_NVTX 0

#define USE_TIMER 0
#define TIMER_CNT 16384
#define TIMERS_PER_ITER 8
#define DURS_CNT (TIMERS_PER_ITER-1)

#define COMM_ONLY 0

/* readonly */ CProxy_Main main_proxy;
/* readonly */ CProxy_Block block_proxy;
/* readonly */ int num_chares;
/* readonly */ int grid_width;
/* readonly */ int grid_height;
/* readonly */ int grid_depth;
/* readonly */ int block_width;
/* readonly */ int block_height;
/* readonly */ int block_depth;
/* readonly */ int x_surf_count;
/* readonly */ int y_surf_count;
/* readonly */ int z_surf_count;
/* readonly */ size_t x_surf_size;
/* readonly */ size_t y_surf_size;
/* readonly */ size_t z_surf_size;
/* readonly */ int n_chares_x;
/* readonly */ int n_chares_y;
/* readonly */ int n_chares_z;
/* readonly */ int n_iters;
/* readonly */ int warmup_iters;
/* readonly */ bool use_channel;
/* readonly */ bool host_staging;
/* readonly */ bool manual_overlap;
/* readonly */ bool fuse_pack;
/* readonly */ bool fuse_unpack;
/* readonly */ bool fuse_update_pack;
/* readonly */ bool fuse_update_all;
/* readonly */ bool use_cuda_graph;
/* readonly */ bool print_elements;

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
    bool use_channel, bool host_staging);
extern void packGhostsFusedDevice(DataType* temperature, DataType* left_ghost,
    DataType* right_ghost, DataType* top_ghost, DataType* bottom_ghost,
    DataType* front_ghost, DataType* back_ghost, bool left_bound, bool right_bound,
    bool top_bound, bool bottom_bound, bool front_bound, bool back_bound,
    int block_width, int block_height, int block_depth, cudaStream_t comm_stream);
extern void unpackGhostDevice(DataType* temperature, DataType* d_ghost, DataType* h_ghost,
    int dir, int block_width, int block_height, int block_depth, size_t ghost_size,
    cudaStream_t comm_stream, cudaStream_t h2d_stream, cudaEvent_t unpack_events[],
    bool use_channel, bool host_staging);
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

class CallbackMsg : public CMessage_CallbackMsg {
public:
  bool recv;
  int dir;

  CallbackMsg(bool recv_, int dir_) : recv(recv_), dir(dir_) {}
};

class Main : public CBase_Main {
  double init_start_time;
  double start_time;
  int fuse_val;

public:
  Main(CkArgMsg* m) {
#if USE_NVTX
    NVTXTracer nvtx_range("Main::Main", NVTXColor::Turquoise);
#endif

    // Set default values
    main_proxy = thisProxy;
    num_chares = CkNumPes();
    grid_width = 512;
    grid_height = 512;
    grid_depth = 512;
    n_iters = 100;
    warmup_iters = 10;
    use_channel = false;
    host_staging = false;
    manual_overlap = false;
    fuse_val = 0;
    fuse_pack = false;
    fuse_unpack = false;
    fuse_update_pack = false;
    fuse_update_all = false;
    use_cuda_graph = false;
    print_elements = false;

    // Process arguments
    int c;
    bool dims[3] = {false, false, false};
    while ((c = getopt(m->argc, m->argv, "c:x:y:z:i:w:domf:gp")) != -1) {
      switch (c) {
        case 'c':
          num_chares = atoi(optarg);
          break;
        case 'x':
          grid_width = atoi(optarg);
          dims[0] = true;
          break;
        case 'y':
          grid_height = atoi(optarg);
          dims[1] = true;
          break;
        case 'z':
          grid_depth = atoi(optarg);
          dims[2] = true;
          break;
        case 'i':
          n_iters = atoi(optarg);
          break;
        case 'w':
          warmup_iters = atoi(optarg);
          break;
        case 'd':
          use_channel = true;
          break;
        case 'o':
          host_staging = true;
          break;
        case 'm':
          manual_overlap = true;
          break;
        case 'f':
          fuse_val = atoi(optarg);
          if (fuse_val == 1) fuse_pack = true;
          else if (fuse_val == 2) fuse_unpack = true;
          else if (fuse_val == 3) fuse_pack = fuse_unpack = true;
          else if (fuse_val == 4) fuse_update_pack = true;
          else if (fuse_val == 5) fuse_update_all = true;
          else {
            CkAbort("ERROR: Invalid fusion value: %d\n", fuse_val);
            CkExit(-1);
          }
          break;
        case 'g':
          use_cuda_graph = true;
          break;
        case 'p':
          print_elements = true;
          break;
        default:
          CkPrintf(
              "Usage: %s -x [grid width] -y [grid height] -z [grid depth] "
              "-c [number of chares] -i [iterations] -w [warmup iterations] "
              "-d [use Channel API] -o [use host staging] -m [use manual overlap]"
              "-f [fusion value] -g [use CUDA Graphs] -p (print blocks)\n", m->argv[0]);
          CkExit();
      }
    }
    delete m;

    // Kernel fusion can only be used with the Channel API
    if (fuse_val != 0 && !use_channel) {
      CkPrintf("ERROR: Kernel fusion can only be used with the Channel API\n");
      CkExit(-1);
    }

    // CUDA Graphs can only be used with the Channel API
    if (use_cuda_graph && !use_channel) {
      CkPrintf("ERROR: CUDA Graphs can only be used with the Channel API\n");
      CkExit(-1);
    }

    // Kernel fusion or CUDA Graphs cannot be used with host-staging Channel API
    if (use_channel && host_staging && (fuse_val != 0 || use_cuda_graph)) {
      CkPrintf("ERROR: Host-staging Channel API cannot be used with kernel fusion or CUDA Graphs\n");
      CkExit(-1);
    }

    // If only the X dimension is given, use it for Y and Z as well
    if (dims[0] && !dims[1] && !dims[2]) grid_height = grid_depth = grid_width;

    // Setup 3D grid of chares
    double area[3];
    int ipx, ipy, ipz, nremain;
    double surf, bestsurf;
    area[0] = grid_width * grid_height;
    area[1] = grid_width * grid_depth;
    area[2] = grid_height * grid_depth;
    bestsurf = 2.0 * (area[0] + area[1] + area[2]);
    ipx = 1;
    while (ipx <= num_chares) {
      if (num_chares % ipx == 0) {
        nremain = num_chares / ipx;
        ipy = 1;

        while (ipy <= nremain) {
          if (nremain % ipy == 0) {
            ipz = nremain / ipy;
            surf = area[0] / ipx / ipy + area[1] / ipx / ipz + area[2] / ipy / ipz;

            if (surf < bestsurf) {
              bestsurf = surf;
              n_chares_x = ipx;
              n_chares_y = ipy;
              n_chares_z = ipz;
            }
          }
          ipy++;
        }
      }
      ipx++;
    }

    if (n_chares_x * n_chares_y * n_chares_z != num_chares) {
      CkPrintf("ERROR: Bad grid of chares: %d x %d x %d != %d\n",
          n_chares_x, n_chares_y, n_chares_z, num_chares);
      CkExit(-1);
    }

    // Calculate block size
    block_width = grid_width / n_chares_x;
    block_height = grid_height / n_chares_y;
    block_depth = grid_depth / n_chares_z;

    // Calculate surface count and sizes
    x_surf_count = block_height * block_depth;
    y_surf_count = block_width * block_depth;
    z_surf_count = block_width * block_height;
    x_surf_size = x_surf_count * sizeof(DataType);
    y_surf_size = y_surf_count * sizeof(DataType);
    z_surf_size = z_surf_count * sizeof(DataType);

    // Print configuration
    CkPrintf("\n[CUDA 3D Jacobi example]\n");
    CkPrintf("Grid: %d x %d x %d, Block: %d x %d x %d, Chares: %d x %d x %d, "
        "Iterations: %d, Warm-up: %d, Channel API: %d, Host-staging: %d, "
        "Manual overlap: %d, Fusion: %d, CUDA Graphs: %d, Print: %d\n\n",
        grid_width, grid_height, grid_depth, block_width, block_height, block_depth,
        n_chares_x, n_chares_y, n_chares_z, n_iters, warmup_iters, use_channel,
        host_staging, manual_overlap, fuse_val, use_cuda_graph, print_elements);

    // Create blocks and start iteration
    block_proxy = CProxy_Block::ckNew(n_chares_x, n_chares_y, n_chares_z);
    init_start_time = CkWallTimer();
    block_proxy.init();
  }

  void initDone() {
#if USE_NVTX
    NVTXTracer nvtx_range("Main::initDone", NVTXColor::Turquoise);
#endif

    CkPrintf("Init time: %.3lf s\n", CkWallTimer() - init_start_time);

    startIter();
  }

  void startIter() {
    start_time = CkWallTimer();

    block_proxy.run();
  }

  void warmupDone() {
#if USE_NVTX
    NVTXTracer nvtx_range("Main::warmupDone", NVTXColor::Turquoise);
#endif

    startIter();
  }

  void allDone() {
#if USE_NVTX
    NVTXTracer nvtx_range("Main::allDone", NVTXColor::Turquoise);
#endif

    double total_time = CkWallTimer() - start_time;
    CkPrintf("Total time: %.3lf s\nAverage iteration time: %.3lf us\n",
        total_time, (total_time / n_iters) * 1e6);
    CkExit();
  }
};

class Block : public CBase_Block {
  Block_SDAG_CODE

 public:
  int my_iter;
  int n_nbr;
  int n_low_nbr;
  int n_high_nbr;
  int nbr_count;
  int x, y, z;
  int linear_index;
  std::string index_str;

  int channel_ids[DIR_COUNT];
  CkChannel channels[DIR_COUNT];
  CkCallback channel_cb;

  DataType* h_temperature;
  DataType* d_temperature;
  DataType* d_new_temperature;

  DataType* h_send_ghosts[DIR_COUNT];
  DataType* h_recv_ghosts[DIR_COUNT];
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

  CkCallback init_cb;
  CkCallback pack_cb;
  CkCallback update_cb;

  bool* bounds;

#if USE_TIMER
  double timers[TIMER_CNT];
  int timer_idx;
#endif

  Block() {}

  ~Block() {
    hapiCheck(cudaFreeHost(h_temperature));
    hapiCheck(cudaFree(d_temperature));
    hapiCheck(cudaFree(d_new_temperature));
    for (int i = 0; i < DIR_COUNT; i++) {
      hapiCheck(cudaFreeHost(h_send_ghosts[i]));
      hapiCheck(cudaFreeHost(h_recv_ghosts[i]));
      hapiCheck(cudaFree(d_send_ghosts[i]));
      hapiCheck(cudaFree(d_recv_ghosts[i]));
    }
    hapiCheck(cudaFreeHost(d_send_ghosts));
    hapiCheck(cudaFreeHost(d_recv_ghosts));
    hapiCheck(cudaFreeHost(bounds));

    hapiCheck(cudaStreamDestroy(compute_stream));
    hapiCheck(cudaStreamDestroy(comm_stream));
    hapiCheck(cudaStreamDestroy(h2d_stream));
    hapiCheck(cudaStreamDestroy(d2h_stream));
    hapiCheck(cudaStreamDestroy(graph_stream));

    hapiCheck(cudaEventDestroy(compute_event));
    hapiCheck(cudaEventDestroy(comm_event));
    for (int i = 0; i < DIR_COUNT; i++) {
      hapiCheck(cudaEventDestroy(pack_events[i]));
      hapiCheck(cudaEventDestroy(unpack_events[i]));
    }
  }

  void init() {
    // Initialize values
    my_iter = 0;
    n_nbr = 0;
    n_low_nbr = 0;
    n_high_nbr = 0;
    x = thisIndex.x;
    y = thisIndex.y;
    z = thisIndex.z;
    linear_index = x * n_chares_y * n_chares_z + y * n_chares_z + z;
    index_str = "[" + std::to_string(x) + "," + std::to_string(y)
      + "," + std::to_string(z) + "]";

#if USE_TIMER
    for (int i = 0; i < TIMER_CNT; i++) timers[i] = 0.0;
    timer_idx = 0;
#endif

    // Channel API
    for (int i = 0; i < DIR_COUNT; i++) channel_ids[i] = -1;
    channel_cb = CkCallback(CkIndex_Block::channelCallback(nullptr), thisProxy[thisIndex]);

    // Check bounds and set number of valid neighbors
    hapiCheck(cudaMallocHost((void**)&bounds, sizeof(bool) * DIR_COUNT));
    for (int i = 0; i < DIR_COUNT; i++) bounds[i] = false;

    if (x == 0)            bounds[LEFT] = true;
    else                   { n_nbr++; n_low_nbr++; }
    if (x == n_chares_x-1) bounds[RIGHT] = true;
    else                   { n_nbr++; n_high_nbr++; }
    if (y == 0)            bounds[TOP] = true;
    else                   { n_nbr++; n_low_nbr++; }
    if (y == n_chares_y-1) bounds[BOTTOM] = true;
    else                   { n_nbr++; n_high_nbr++; }
    if (z == 0)            bounds[FRONT] = true;
    else                   { n_nbr++; n_low_nbr++; }
    if (z == n_chares_z-1) bounds[BACK] = true;
    else                   { n_nbr++; n_high_nbr++; }

    // Allocate memory and create CUDA entities
    hapiCheck(cudaMallocHost((void**)&h_temperature,
          sizeof(DataType) * (block_width+2) * (block_height+2) * (block_depth+2)));
    hapiCheck(cudaMalloc((void**)&d_temperature,
          sizeof(DataType) * (block_width+2) * (block_height+2) * (block_depth+2)));
    hapiCheck(cudaMalloc((void**)&d_new_temperature,
          sizeof(DataType) * (block_width+2) * (block_height+2) * (block_depth+2)));
    ghost_sizes[LEFT] = x_surf_size;
    ghost_sizes[RIGHT] = x_surf_size;
    ghost_sizes[TOP] = y_surf_size;
    ghost_sizes[BOTTOM] = y_surf_size;
    ghost_sizes[FRONT] = z_surf_size;
    ghost_sizes[BACK] = z_surf_size;
    hapiCheck(cudaMallocHost((void**)&d_send_ghosts, sizeof(DataType*) * DIR_COUNT));
    hapiCheck(cudaMallocHost((void**)&d_recv_ghosts, sizeof(DataType*) * DIR_COUNT));
    for (int i = 0; i < DIR_COUNT; i++) {
      hapiCheck(cudaMallocHost((void**)&h_send_ghosts[i], ghost_sizes[i]));
      hapiCheck(cudaMallocHost((void**)&h_recv_ghosts[i], ghost_sizes[i]));
      hapiCheck(cudaMalloc((void**)&d_send_ghosts[i], ghost_sizes[i]));
      hapiCheck(cudaMalloc((void**)&d_recv_ghosts[i], ghost_sizes[i]));
    }

    // Create CUDA streams and events
    hapiCheck(cudaStreamCreateWithPriority(&compute_stream, cudaStreamDefault, 0));
    hapiCheck(cudaStreamCreateWithPriority(&comm_stream, cudaStreamDefault, -1));
    hapiCheck(cudaStreamCreateWithPriority(&h2d_stream, cudaStreamDefault, -1));
    hapiCheck(cudaStreamCreateWithPriority(&d2h_stream, cudaStreamDefault, -1));
    hapiCheck(cudaStreamCreateWithFlags(&graph_stream, cudaStreamNonBlocking));

    hapiCheck(cudaEventCreateWithFlags(&compute_event, cudaEventDisableTiming));
    hapiCheck(cudaEventCreateWithFlags(&comm_event, cudaEventDisableTiming));
    for (int i = 0; i < DIR_COUNT; i++) {
      hapiCheck(cudaEventCreateWithFlags(&pack_events[i], cudaEventDisableTiming));
      hapiCheck(cudaEventCreateWithFlags(&unpack_events[i], cudaEventDisableTiming));
    }

    // Create Charm++ callbacks
    init_cb = CkCallback(CkIndex_Block::initDone(), thisProxy[thisIndex]);
    pack_cb = CkCallback(CkIndex_Block::packGhostsDone(), thisProxy[thisIndex]);
    update_cb = CkCallback(CkIndex_Block::updateDone(), thisProxy[thisIndex]);

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

    // TODO: Support reduction callback in hapiAddCallback
#if CUDA_SYNC
    cudaStreamSynchronize(compute_stream);
    thisProxy[thisIndex].initDone();
#else
    hapiAddCallback(compute_stream, init_cb);
#endif
  }

  void sendChannelIDs() {
    // Create channel IDs for "higher" neighbors and send them
    int channel_id = -1;
    if (!bounds[RIGHT]) {
      channel_id = linear_index * (DIR_COUNT / 2);
      channel_ids[RIGHT] = channel_id;
      thisProxy(x+1, y, z).recvChannelID(LEFT, channel_id);
    }
    if (!bounds[BOTTOM]) {
      channel_id = linear_index * (DIR_COUNT / 2) + 1;
      channel_ids[BOTTOM] = channel_id;
      thisProxy(x, y+1, z).recvChannelID(TOP, channel_id);
    }
    if (!bounds[BACK]) {
      channel_id = linear_index * (DIR_COUNT / 2) + 2;
      channel_ids[BACK] = channel_id;
      thisProxy(x, y, z+1).recvChannelID(FRONT, channel_id);
    }
  }

  void createChannels() {
    // Check if channel IDs are properly stored
    for (int dir = 0; dir < DIR_COUNT; dir++) {
      if (!bounds[dir]) {
        CkAssert(channel_ids[dir] != -1);
      }
    }

    // Create channels
    if (!bounds[LEFT]) {
      channels[LEFT] = CkChannel(channel_ids[LEFT], thisProxy(x-1, y, z));
    }
    if (!bounds[RIGHT]) {
      channels[RIGHT] = CkChannel(channel_ids[RIGHT], thisProxy(x+1, y, z));
    }
    if (!bounds[TOP]) {
      channels[TOP] = CkChannel(channel_ids[TOP], thisProxy(x, y-1, z));
    }
    if (!bounds[BOTTOM]) {
      channels[BOTTOM] = CkChannel(channel_ids[BOTTOM], thisProxy(x, y+1, z));
    }
    if (!bounds[FRONT]) {
      channels[FRONT] = CkChannel(channel_ids[FRONT], thisProxy(x, y, z-1));
    }
    if (!bounds[BACK]) {
      channels[BACK] = CkChannel(channel_ids[BACK], thisProxy(x, y, z+1));
    }
  }

  void createCudaGraph(cudaGraph_t& cg, cudaGraphExec_t& cge,
      DataType* d_temp, DataType* d_new_temp) {
    /*
    std::vector<cudaGraphNode_t> dep1;
    std::vector<cudaGraphNode_t> dep2;
    std::vector<cudaGraphNode_t>* dep = &dep1;
    std::vector<cudaGraphNode_t>* new_dep = &dep2;

    hapiCheck(cudaGraphCreate(&cuda_graph, 0));
    */

    // Start capturing
    if (!fuse_update_all) {
      hapiCheck(cudaStreamBeginCapture(comm_stream, cudaStreamCaptureModeGlobal));
    } else {
      hapiCheck(cudaStreamBeginCapture(compute_stream, cudaStreamCaptureModeGlobal));
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
        hapiCheck(cudaGraphAddKernelNode(&fuse_unpack_node, cuda_graph,
              dep->data(), dep->size(), &fu_params));
        new_dep->push_back(fuse_unpack_node);
        */
      } else {
        for (int dir = 0; dir < DIR_COUNT; dir++) {
          if (!bounds[dir]) {
            unpackGhostDevice(d_temp, d_recv_ghosts[dir], nullptr, dir,
                block_width, block_height, block_depth, ghost_sizes[dir],
                comm_stream, h2d_stream, unpack_events, use_channel, host_staging);
            /*
            cudaKernelNodeParams u_params = {0};
            setUnpackNode(u_params, d_temperature, d_recv_ghosts[dir], dir,
                block_width, block_height, block_depth);
            hapiCheck(cudaGraphAddKernelNode(&unpack_nodes[dir], cuda_graph,
                  dep->data(), dep->size(), &u_params));
            new_dep->push_back(unpack_nodes[dir]);
            */
          }
        }
      }

      hapiCheck(cudaEventRecord(comm_event, comm_stream));
      hapiCheck(cudaStreamWaitEvent(compute_stream, comm_event, 0));
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
    hapiCheck(cudaGraphAddKernelNode(&update_node, cuda_graph,
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
        hapiCheck(cudaGraphAddKernelNode(&fuse_pack_node, cuda_graph,
              dep->data(), dep->size(), &fp_params));
        new_dep->push_back(fuse_pack_node);
        */
      } else if (!fuse_update_pack) {
        packGhostsDevice(d_new_temp, d_send_ghosts, h_send_ghosts, bounds,
            block_width, block_height, block_depth, x_surf_size, y_surf_size, z_surf_size,
            comm_stream, d2h_stream, pack_events, use_channel, host_staging);
        /*
        for (int dir = 0; dir < DIR_COUNT; dir++) {
          if (!bounds[dir]) {
            cudaKernelNodeParams p_params = {0};
            setPackNode(p_params, d_new_temperature, d_send_ghosts[dir], dir,
                block_width, block_height, block_depth);
            hapiCheck(cudaGraphAddKernelNode(&pack_nodes[dir], cuda_graph,
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
      hapiCheck(cudaStreamEndCapture(comm_stream, &cg));
    } else {
      hapiCheck(cudaStreamEndCapture(compute_stream, &cg));
    }

    // Instantiate CUDA graph
    hapiCheck(cudaGraphInstantiate(&cge, cg, NULL, NULL, 0));
  }

  void createCudaGraphs() {
    createCudaGraph(cuda_graph_1, cuda_graph_exec_1, d_temperature, d_new_temperature);
    createCudaGraph(cuda_graph_2, cuda_graph_exec_2, d_new_temperature, d_temperature);
    cuda_graph_exec = &cuda_graph_exec_1;
    cuda_graph_exec_next = &cuda_graph_exec_2;
  }

  void launchCudaGraph() {
    std::swap(cuda_graph_exec, cuda_graph_exec_next);
    hapiCheck(cudaGraphLaunch(*cuda_graph_exec, graph_stream));
  }

  void packGhosts() {
#if USE_NVTX
    NVTXTracer nvtx_range(index_str + " packGhosts", NVTXColor::PeterRiver);
#endif

#if !COMM_ONLY
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
              comm_stream, d2h_stream, pack_events, use_channel, host_staging);
        }
      }

      if (manual_overlap) {
        // Invoke GPU kernel for interior
        invokeJacobiInteriorKernel(d_temperature, d_new_temperature,
            block_width, block_height, block_depth, compute_stream);
      }

      // Add asynchronous callback to be invoked when packing kernels and
      // ghost transfers are complete
      if (use_channel && !host_staging) {
        if (fuse_update_pack || fuse_update_all) {
#if CUDA_SYNC
          cudaStreamSynchronize(compute_stream);
          thisProxy[thisIndex].packGhostsDone();
#else
          hapiAddCallback(compute_stream, pack_cb);
#endif
        } else {
#if CUDA_SYNC
          cudaStreamSynchronize(comm_stream);
          thisProxy[thisIndex].packGhostsDone();
#else
          hapiAddCallback(comm_stream, pack_cb);
#endif
        }
      } else {
#if CUDA_SYNC
        cudaStreamSynchronize(d2h_stream);
        thisProxy[thisIndex].packGhostsDone();
#else
        hapiAddCallback(d2h_stream, pack_cb);
#endif
      }
    } else {
      // Communication should follow completion of CUDA graph
#if CUDA_SYNC
      cudaStreamSynchronize(graph_stream);
      thisProxy[thisIndex].packGhostsDone();
#else
      hapiAddCallback(graph_stream, pack_cb);
#endif
    }
#else
    thisProxy[thisIndex].packGhostsDone();
#endif // COMM_ONLY
  }

  void sendGhosts() {
#if USE_NVTX
    NVTXTracer nvtx_range(index_str + " sendGhosts", NVTXColor::WetAsphalt);
#endif
#if USE_TIMER
    timers[timer_idx++] = CkWallTimer();
#endif

    // Increment iteration count and swap data pointers
    // to avoid host synchronization
    my_iter++;
    std::swap(d_temperature, d_new_temperature);

    // Send boundary data to neighbors
    if (use_channel) {
      // Set reference number for callback
      channel_cb.setRefNum(my_iter);

      // Send ghosts
      for (int dir = 0; dir < DIR_COUNT; dir++) {
        if (!bounds[dir]) {
          DataType* buffer = host_staging ? h_send_ghosts[dir] : d_send_ghosts[dir];
          channels[dir].send(buffer, ghost_sizes[dir], true, channel_cb,
              new CallbackMsg(false, dir));
        }
      }
    } else {
      if (!bounds[LEFT])
        thisProxy(x-1, y, z).recvGhostReg(my_iter, RIGHT,
            x_surf_count, h_send_ghosts[LEFT]);
      if (!bounds[RIGHT])
        thisProxy(x+1, y, z).recvGhostReg(my_iter, LEFT,
            x_surf_count, h_send_ghosts[RIGHT]);
      if (!bounds[TOP])
        thisProxy(x, y-1, z).recvGhostReg(my_iter, BOTTOM,
            y_surf_count, h_send_ghosts[TOP]);
      if (!bounds[BOTTOM])
        thisProxy(x, y+1, z).recvGhostReg(my_iter, TOP,
            y_surf_count, h_send_ghosts[BOTTOM]);
      if (!bounds[FRONT])
        thisProxy(x, y, z-1).recvGhostReg(my_iter, BACK,
            z_surf_count, h_send_ghosts[FRONT]);
      if (!bounds[BACK])
        thisProxy(x, y, z+1).recvGhostReg(my_iter, FRONT,
            z_surf_count, h_send_ghosts[BACK]);
    }

#if USE_TIMER
    timers[timer_idx++] = CkWallTimer();
#endif
  }

  void recvGhosts() {
#if USE_NVTX
    NVTXTracer nvtx_range(index_str + " recvGhosts", NVTXColor::Carrot);
#endif
#if USE_TIMER
    timers[timer_idx++] = CkWallTimer();
#endif

    // Receive ghosts
    for (int dir = 0; dir < DIR_COUNT; dir++) {
      if (!bounds[dir]) {
        DataType* buffer = host_staging ? h_recv_ghosts[dir] : d_recv_ghosts[dir];
        channels[dir].recv(buffer, ghost_sizes[dir], true, channel_cb,
            new CallbackMsg(true, dir));
      }
    }

#if USE_TIMER
    timers[timer_idx++] = CkWallTimer();
#endif
  }

  void processTimer() {
#if USE_TIMER
    if (nbr_count == 0) {
      timers[timer_idx++] = CkWallTimer();
    }
    if (nbr_count == 2*n_nbr-1) {
      timers[timer_idx++] = CkWallTimer();
    }
#endif
  }

  void processGhostChannel(int dir) {
#if USE_NVTX
    NVTXTracer nvtx_range(index_str + " processGhostChannel " + std::to_string(dir), NVTXColor::Carrot);
#endif

#if !COMM_ONLY
    // Unpack received ghost
    unpackGhostDevice(d_temperature, d_recv_ghosts[dir], h_recv_ghosts[dir], dir,
        block_width, block_height, block_depth, ghost_sizes[dir],
        comm_stream, h2d_stream, unpack_events, use_channel, host_staging);
#endif
  }

  void processGhostReg(int dir, int count, DataType* gh) {
#if USE_NVTX
    NVTXTracer nvtx_range(index_str + " processGhostReg " + std::to_string(dir), NVTXColor::Carrot);
#endif

    CkAssert(dir >= 0 && dir < DIR_COUNT);
    DataType* h_recv_ghost = h_recv_ghosts[dir];

    size_t ghost_size = count * sizeof(DataType);
    memcpy(h_recv_ghost, gh, ghost_size);
    unpackGhostDevice(d_temperature, d_recv_ghosts[dir], h_recv_ghost, dir,
        block_width, block_height, block_depth, ghost_size,
        comm_stream, h2d_stream, unpack_events, use_channel, host_staging);
  }

  void processGhostsFused() {
#if USE_NVTX
    NVTXTracer nvtx_range(index_str + " processGhostsFused", NVTXColor::Carrot);
#endif

    // Unpack all ghosts together
    unpackGhostsFusedDevice(d_temperature, d_recv_ghosts[LEFT], d_recv_ghosts[RIGHT],
        d_recv_ghosts[TOP], d_recv_ghosts[BOTTOM], d_recv_ghosts[FRONT],
        d_recv_ghosts[BACK], bounds[LEFT], bounds[RIGHT], bounds[TOP], bounds[BOTTOM],
        bounds[FRONT], bounds[BACK], block_width, block_height, block_depth, comm_stream);
  }

  void update() {
#if USE_NVTX
    NVTXTracer nvtx_range(index_str + " update", NVTXColor::BelizeHole);
#endif
#if USE_TIMER
    timers[timer_idx++] = CkWallTimer();
#endif

#if !COMM_ONLY
    if (!use_cuda_graph) {
      if (!fuse_update_all) {
        // Update should only be performed after operations in communication stream
        // (transfers and unpacking) complete
        hapiCheck(cudaEventRecord(comm_event, comm_stream));
        hapiCheck(cudaStreamWaitEvent(compute_stream, comm_event, 0));
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
#endif

#if USE_TIMER
    timers[timer_idx++] = CkWallTimer();
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
        if (x == 0 && y == 0 && z == 0) {
          CkPrintf("Duration %d: %.3lf us\n", i, durations[i] * 1e6);
        }
      }
    }
#endif

    // Synchronize with host only when necessary
    if (print_elements || (my_iter == warmup_iters)
        || (my_iter == warmup_iters + n_iters)) {
#if !COMM_ONLY
      if (use_cuda_graph) {
#ifdef CUDA_SYNC
        cudaStreamSynchronize(graph_stream);
        thisProxy[thisIndex].updateDone();
#else
        hapiAddCallback(graph_stream, update_cb);
#endif
      } else {
#ifdef CUDA_SYNC
        cudaStreamSynchronize(compute_stream);
        thisProxy[thisIndex].updateDone();
#else
        hapiAddCallback(compute_stream, update_cb);
#endif
      }
#else
      thisProxy[thisIndex].updateDone();
#endif // COMM_ONLY
    } else {
      thisProxy[thisIndex].run();
    }
  }

  void updateDone() {
#if USE_NVTX
    NVTXTracer nvtx_range(index_str + " updateDone", NVTXColor::GreenSea);
#endif

    if (print_elements) {
      if (x == 0 && y == 0 && z == 0) {
        CkPrintf("Printing iteration %d\n", my_iter);
        thisProxy[thisIndex].print();
      }
    } else if (my_iter == warmup_iters) {
      contribute(CkCallback(CkReductionTarget(Main, warmupDone), main_proxy));
    } else {
      contribute(CkCallback(CkReductionTarget(Main, allDone), main_proxy));
    }
  }

  void print() {
    // Move data from device to host for printing
    hapiCheck(cudaMemcpyAsync(h_temperature, d_temperature,
          sizeof(DataType) * (block_width+2)*(block_height+2)*(block_depth+2),
          cudaMemcpyDeviceToHost, comm_stream));
    cudaStreamSynchronize(comm_stream);

    CkPrintf("[%d,%d,%d]\n", x, y, z);
    for (int k = 0; k < block_depth+2; k++) {
      for (int j = 0; j < block_height+2; j++) {
        for (int i = 0; i < block_width+2; i++) {
#ifdef TEST_CORRECTNESS
          CkPrintf("%d ", h_temperature[IDX(i,j,k)]);
#else
          CkPrintf("%.6lf ", h_temperature[IDX(i,j,k)]);
#endif
        }
        CkPrintf("\n");
      }
      CkPrintf("\n");
    }

    // Go around all chares and print each one, until last chare
    // returns control back to Main or resumes next iteration
    if (x == n_chares_x-1 && y == n_chares_y-1 && z == n_chares_z-1) {
      if (my_iter == warmup_iters) {
        main_proxy.warmupDone();
      } else if (my_iter == warmup_iters + n_iters) {
        main_proxy.allDone();
      } else {
        thisProxy.run();
      }
    } else {
      if (x == n_chares_x-1 && y == n_chares_y-1) {
        thisProxy(0,0,z+1).print();
      } else {
        if (x == n_chares_x-1) {
          thisProxy(0,y+1,z).print();
        } else {
          thisProxy(x+1,y,z).print();
        }
      }
    }
  }
};

#include "jacobi3d.def.h"
