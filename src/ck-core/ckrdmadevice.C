/*
 * Direct GPU Messaging
 *
 * Uses host-bypass mechanisms to directly transfer data between GPU devices.
 *
 * 1) Intra-process (intra-node): The sender sends a metadata message to the
 *    receiver containing the pointer to the source GPU buffer. There is
 *    no setup needed on the sender side. The receiver invokes device-to-device
 *    transfer from the source GPU buffer to the destination GPU buffer.
 *
 * 2) Inter-process (intra-node): The pointer to the source GPU buffer will be
 *    invalid on the receiver as it is a different process. Thus CUDA IPC is
 *    used to create a handle to the source GPU buffer, which can be opened on
 *    the receiver side to initiate the data transfer. To mitigate the overheads
 *    of creating and destroying IPC handles, the runtime first allocates a
 *    'device communication buffer' on each GPU device, creates IPC handles
 *    only for these buffers, and then exchanges the handles between processes
 *    on the same physical node. This means each process will have IPC handles
 *    for all device communication buffers on the same host (that are potentially
 *    managed by other processes) and can perform data transfers using these
 *    handles. Each GPU-GPU data transfer invovles requesting a block from the
 *    device communication buffer on the sender, copying the source GPU buffer
 *    to the allocated block, sending a metadata message to the receiver
 *    (that contains the offset of the allocated block), and performing a
 *    transfer from the block on the sender's device communication buffer to
 *    the destination GPU buffer. CUDA Events are used to enforce the correct
 *    ordering between these data transfers. Because multiple PEs can be mapped
 *    to the same GPU and hence concurrently request allocations from the same
 *    device communication buffer, a thread-safe allocator using the buddy
 *    allocation algorithm was implemented. The allocator first calls cudaMalloc
 *    to obtain a relatively large chunk of memory and then services allocation
 *    and deallocation requests from PEs that are mapped to its GPU device.
 *    The buddy algorithm was used to minimize the external fragmentation that
 *    could occur from concurrent manipulations of the device communication
 *    buffer.
 *
 * TODO
 * 3) Inter-node: This currently uses a simple host-staged mechanism to perform
 *    a device-to-host copy of the source GPU buffer to a message, which is sent
 *    to the receiver. The receiver then performs a host-to-device copy to the
 *    destination GPU buffer. This will be updated to use GPUDirect RDMA to
 *    directly performa true device-to-device transfer.
 */

#ifndef _WIN32
#include <pthread.h>
#endif
#include "envelope.h"
#include "charm++.h"
#include "ck.h"
#include "ckrdmadevice.h"

#if CMK_CUDA

#include "hapi.h"
#include "gpumanager.h"

CsvExtern(GPUManager, gpu_manager);

// Invoked when a GPU buffer arrives on the receiver
#if !CMK_GPU_COMM
void CkRdmaDeviceRecvHandler(void* data, void* msg)
#else
void CkRdmaDeviceRecvHandler(void* data)
#endif
{
#if CMK_GPU_COMM
  // Process QD to mark completion of buffer transfer
  QdProcess(1);
#endif

  DeviceRdmaOp* op = (DeviceRdmaOp*)data;
  DeviceRdmaInfo* info = op->info;

  // Invoke source callbacks
  if (op->src_cb) {
    CkCallback* cb = (CkCallback*)op->src_cb;
    cb->send();
    delete cb;
  }

  // Update counter (there may be multiple buffers in transit)
  info->counter++;

  // Check if all buffers have been received
  // If so, invoke regular entry method
  if (info->counter == info->n_ops) {
    QdCreate(1);

    enqueueNcpyMessage(op->dest_pe, info->msg);

    // Free RDMA metadata
    CmiFree(info);
  }
}

/****************************** Direct (Persistent) API ******************************/

void CkDevicePersistent::init() {
  pe = CkMyPe();
  cb_msg = nullptr;
  ipc_ptr = nullptr;
  ipc_open = false;
}

void CkDevicePersistent::open() {
  // Create a CUDA IPC handle for inter-process communication
  hapiCheck(cudaIpcGetMemHandle(&cuda_ipc_handle, (void*)ptr));
}

void CkDevicePersistent::close() {
  // Close the CUDA IPC handle if it was opened
  hapiCheck(cudaIpcCloseMemHandle(ipc_ptr));
}

void CkDevicePersistent::set_msg(void* msg) {
  cb_msg = msg;
}

void CkDevicePersistent::pup(PUP::er& p) {
  p((char*)&ptr, sizeof(ptr));
  p|cnt;
  p|pe;
  p|cb;
  p((char*)&cuda_ipc_handle, sizeof(cuda_ipc_handle));
}

CkDeviceStatus CkDevicePersistent::get(CkDevicePersistent& src) {
  // Check that the source buffer fits into the destination buffer
  if (cnt < src.cnt) {
    CkAbort("CkDevicePersistent::get: Destination buffer is smaller than source buffer\n");
  }

  CkNcpyModeDevice mode = findTransferModeDevice(src.pe, CkMyPe());

  // Perform get
  if (mode == CkNcpyModeDevice::MEMCPY) {
    cudaMemcpyAsync((void*)ptr, src.ptr, cnt, cudaMemcpyDeviceToDevice, cuda_stream);
  } else if (mode == CkNcpyModeDevice::IPC) {
    if (!src.ipc_open) {
      hapiCheck(cudaIpcOpenMemHandle(&src.ipc_ptr, src.cuda_ipc_handle,
            cudaIpcMemLazyEnablePeerAccess));
      src.ipc_open = true;
    }
    cudaMemcpyAsync((void*)ptr, src.ipc_ptr, cnt, cudaMemcpyDeviceToDevice, cuda_stream);
  } else {
    CkAbort("Persistant GPU messaging is currently not supported for inter-node messages");
  }

  // Set callbacks to be invoked once get is complete
  if (src.cb.type != CkCallback::ignore) {
    hapiAddCallback(cuda_stream, src.cb, src.cb_msg);
  }
  if (cb.type != CkCallback::ignore) {
    hapiAddCallback(cuda_stream, cb, cb_msg);
  }

  return CkDeviceStatus::incomplete;
}

CkDeviceStatus CkDevicePersistent::put(CkDevicePersistent& dst) {
  // Check that the source buffer fits into the destination buffer
  if (dst.cnt < cnt) {
    CkAbort("CkDevicePersistent::put: Destination buffer is smaller than source buffer\n");
  }

  CkNcpyModeDevice mode = findTransferModeDevice(CkMyPe(), dst.pe);

  // Perform put
  if (mode == CkNcpyModeDevice::MEMCPY) {
    cudaMemcpyAsync((void*)dst.ptr, ptr, cnt, cudaMemcpyDeviceToDevice, cuda_stream);
  } else if (mode == CkNcpyModeDevice::IPC) {
    if (!dst.ipc_open) {
      hapiCheck(cudaIpcOpenMemHandle(&dst.ipc_ptr, dst.cuda_ipc_handle,
            cudaIpcMemLazyEnablePeerAccess));
      dst.ipc_open = true;
    }
    cudaMemcpyAsync(dst.ipc_ptr, ptr, cnt, cudaMemcpyDeviceToDevice, cuda_stream);
  } else {
    CkAbort("Persistant GPU messaging is not yet supported for inter-node messages");
  }

  // Set callbacks to be invoked once get is complete
  if (cb.type != CkCallback::ignore) {
    hapiAddCallback(cuda_stream, cb, cb_msg);
  }
  if (dst.cb.type != CkCallback::ignore) {
    hapiAddCallback(cuda_stream, dst.cb, dst.cb_msg);
  }

  return CkDeviceStatus::incomplete;
}

/****************************** Recv Entry Method API ******************************/

// Invoked after post entry method
void CkRdmaDeviceIssueRgets(envelope *env, int numops, void **arrPtrs, int *arrSizes, CkDeviceBufferPost *postStructs) {
  // Change message header to invoke regular entry method
  CMI_ZC_MSGTYPE(env) = CMK_REG_NO_ZC_MSG;

  // Create a copy of this message for regular entry method invocation
  // FIXME: Reuse the old message instead of creating a new one
  void* old_msg = EnvToUsr(env);
  envelope* new_env = UsrToEnv(CkCopyMsg(&old_msg));

  // Start unpacking marshalled message
  PUP::fromMem up((void *)((CkMarshallMsg *)EnvToUsr(env))->msgBuf);
  int received_numops;
  up|received_numops;
  CkAssert(numops == received_numops);

  CkDeviceBuffer source;

#if !CMK_GPU_COMM
  // Machine layer does not support GPU-aware communication
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  // Find which mode of transfer should be used
  CkNcpyModeDevice mode = findTransferModeDevice(env->getSrcPe(), CkMyPe());
#endif

  // Allocate and fill in metadata for this zerocopy operation
  void* rdma_data = CmiAlloc(sizeof(DeviceRdmaInfo) + sizeof(DeviceRdmaOp) * numops);
  CmiEnforce(rdma_data);
  DeviceRdmaInfo* rdma_info = (DeviceRdmaInfo*)rdma_data;
  rdma_info->n_ops = numops;
  rdma_info->counter = 0;
  rdma_info->msg = new_env;

  for (int i = 0; i < numops; i++) {
    // Unpack source buffer from sender
    up|source;

    if (arrSizes[i] > source.cnt) {
      CkAbort("CkRdmaDeviceIssueRgets: posted data size is larger than source data size!");
    }

    // Store information about this buffer
    DeviceRdmaOp& save_op = *(DeviceRdmaOp*)((char*)rdma_data
        + sizeof(DeviceRdmaInfo) + sizeof(DeviceRdmaOp) * i);
    save_op.dest_pe = CkMyPe();
    save_op.dest_ptr = arrPtrs[i];
    save_op.size = (size_t)arrSizes[i];
    save_op.info = rdma_info;
    save_op.src_cb = (source.cb.type != CkCallback::ignore) ? new CkCallback(source.cb) : nullptr;
    save_op.dst_cb = nullptr;

#if !CMK_GPU_COMM
    // Machine layer does not support GPU-aware communication
    // Check if destination PE is correct
    // TODO: Handle this case instead of aborting
    if (source.dest_pe != CkMyPe()) {
      CkAbort("Current PE does not match the destination PE determined by the sender. "
          "Please enable CMK_GLOBAL_LOCATION_UPDATE.");
    }

    // Destination buffer (on this receiver)
    CkDeviceBuffer dest((const void *)arrPtrs[i], arrSizes[i]);

    // Perform data transfers
    if (mode == CkNcpyModeDevice::MEMCPY) {
      // Source and destination PEs are in the same process (logical node)
      // Directly invoke memcpy from source buffer to destination buffer
      hapiCheck(cudaMemcpyAsync((void*)dest.ptr, source.ptr, dest.cnt,
            cudaMemcpyDeviceToDevice, postStructs[i].cuda_stream));
    } else if (mode == CkNcpyModeDevice::IPC && csv_gpu_manager.use_shm) {
      // Inter-process using shared memory optimizations
      // Use optimiziations with POSIX shared memory
      cuda_ipc_device_info& device_info =
        csv_gpu_manager.cuda_ipc_device_infos[source.device_idx];

      // 1. Make user-provided stream wait for IPC event using cudaStreamWaitEvent
      //    (source buffer to device comm buffer on source)
      hapiCheck(cudaStreamWaitEvent(postStructs[i].cuda_stream,
            device_info.src_event_pool[source.event_idx], 0));

      // 2. Invoke cudaMemcpyAsync (from source device comm buffer to destination buffer)
      hapiCheck(cudaMemcpyAsync((void*)dest.ptr,
            (void*)((char*)device_info.buffer + source.comm_offset),
            dest.cnt, cudaMemcpyDeviceToDevice, postStructs[i].cuda_stream));

      // 3. Record IPC event so that the sender can query it for freeing
      //    device comm buffer and corresponding pair of CUDA IPC events
      hapiCheck(cudaEventRecord(device_info.dst_event_pool[source.event_idx],
            postStructs[i].cuda_stream));

      // 4. Set flag in shared memory so that the sender can start querying
      //    completion of the IPC event
      cuda_ipc_event_shared* shm_event_shared =
        (cuda_ipc_event_shared*)((char*)csv_gpu_manager.shm_ptr
            + csv_gpu_manager.shm_chunk_size * source.device_idx
            + sizeof(cudaIpcMemHandle_t)) + source.event_idx;
      pthread_mutex_lock(&shm_event_shared->lock);
      shm_event_shared->dst_flag = true;
      pthread_mutex_unlock(&shm_event_shared->lock);
    } else {
      // Handle all other cases (basic inter-process and inter-node)
      // Transfer the received/unpacked data on host to the destination device buffer
      // FIXME: Print warning that this is slow?
      CkAssert(source.data_stored);
      hapiCheck(cudaMemcpyAsync((void*)dest.ptr, source.data, dest.cnt,
            cudaMemcpyHostToDevice, postStructs[i].cuda_stream));
    }

    // Add source callback for polling, so that it can be invoked once the transfer is complete
    hapiAddCallback(postStructs[i].cuda_stream, CkCallback(CkRdmaDeviceRecvHandler, &save_op));
#else
    // Machine layer supports GPU-aware communication
    save_op.tag = source.tag;
#endif // CMK_GPU_COMM
  }

#if CMK_GPU_COMM
  // Post ucp_tag_recv_nb's to receive GPU data
  for (int i = 0; i < numops; i++) {
    DeviceRdmaOp* save_op = (DeviceRdmaOp*)((char*)rdma_data
        + sizeof(DeviceRdmaInfo) + sizeof(DeviceRdmaOp) * i);
    QdCreate(1);
    CmiRecvDevice(save_op, COMM_TYPE_CHARM);
  }
#endif
}

// Unused, left for future reference
/*
int CkRdmaGetDestPEChare(int dest_pe, void* obj_ptr) {
  // Mechanism extracted from _prepareMsg() in ck.C
  if (dest_pe < 0) {
    int pe = -(dest_pe+1);
    if (pe == CkMyPe()) {
      VidBlock* vblk = CkpvAccess(vidblocks)[(CmiIntPtr)obj_ptr];
      void *objPtr = vblk->getLocalChare();
      dest_pe = objPtr ? pe : vblk->getActualID().onPE;
    } else {
      dest_pe = pe;
    }
  }

  return dest_pe;
}
*/

static int findFreeIpcEvent(DeviceManager* dm, const size_t comm_offset) {
  int pool_size = CsvAccess(gpu_manager).cuda_ipc_event_pool_size_pe;
  int pool_start = CkMyRank() * pool_size;
  int device_index = dm->global_index;
  cuda_ipc_device_info& my_device_info = CsvAccess(gpu_manager).cuda_ipc_device_infos[device_index];

  // Free IPC events that are complete
  // TODO: Don't do this every time but only when the event pool is somewhat empty
  for (int i = pool_start; i < pool_start + pool_size; i++) {
    int& event_flag = my_device_info.event_pool_flags[i];
    cudaEvent_t& ev = my_device_info.dst_event_pool[i];
    size_t& buff_offset = my_device_info.event_pool_buff_offsets[i];
    // For a used event, check if it's complete and mark as free if so
    if (event_flag != 0) {
      // Check in shared memory if receiver has invoked the memcpy from
      // the device comm buffer on sender to destination buffer
      cuda_ipc_event_shared* shm_event_shared =
        (cuda_ipc_event_shared*)((char*)CsvAccess(gpu_manager).shm_ptr
            + CsvAccess(gpu_manager).shm_chunk_size * device_index
            + sizeof(cudaIpcMemHandle_t)) + i;
      bool can_query = false;
      pthread_mutex_lock(&shm_event_shared->lock);
      if (shm_event_shared->dst_flag == true) {
        shm_event_shared->dst_flag = false;
        can_query = true;
      }
      pthread_mutex_unlock(&shm_event_shared->lock);

      // If the receiver has invoked the memcpy,
      // the sender can query the event for completion
      if (can_query) {
        if (cudaEventQuery(ev) == cudaSuccess) {
          // Event completion means that the transfer from source device comm buffer
          // to dest buffer is complete, so free the allocated block
          if (event_flag == 1) {
            dm->free_comm_buffer(buff_offset);
          } else {
            CkAbort("Retrieved cudaSuccess for a free IPC event");
          }

          // Mark event as free
          event_flag = 0;
        }
      }
    }
  }

  // Allocate CUDA IPC events from the pool
  // Two events are used per message:
  // 1) Recorded by the sender after 'source buffer -> device comm buffer' cudaMemcpy.
  //    Can be used by the sender to determine if the sender buffer is free for reuse.
  //    It is also used by the receiver to create a dependency for the second cudaMemcpy
  //    ('device comm buffer -> dest buffer')
  // 2) Recorded by the receiver after 'device comm buffer -> dest buffer' cudaMemcpy.
  //    It is used by the sender to determine when the allocated block on
  //    device comm buffer and IPC events can be freed.
  for (int i = pool_start; i < pool_start + pool_size; i++) {
    int& event_flag = my_device_info.event_pool_flags[i];
    size_t& buff_offset = my_device_info.event_pool_buff_offsets[i];
    if (event_flag == 0) {
      event_flag = 1;
      buff_offset = comm_offset;
      return i;
    }
  }

  return -1;
}

// Performs sender-side operations necessary for device zerocopy
void CkRdmaDeviceOnSender(int dest_pe, int numops, CkDeviceBuffer** buffers) {
  // TODO: Need to handle the case where the destination PE could be wrong
  //       (due to migration, etc.). Currently the code relies on a global
  //       location update after migration (with CMK_GLOBAL_LOCATION_UPDATE).
#if !CMK_GPU_COMM
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  // Determine transfer mode (intra-process, inter-process, inter-node)
  CkNcpyModeDevice transfer_mode = findTransferModeDevice(CkMyPe(), dest_pe);

  // Store destination PE in the metadata message
  // FIXME: Not necessary? save_op.dest_pe is set to CkMyPe() on the receiver
  for (int i = 0; i < numops; i++) {
    buffers[i]->dest_pe = dest_pe;
  }

  if (transfer_mode == CkNcpyModeDevice::MEMCPY) {
    // Don't need to do anything for intra-process
    return;
  } else if (transfer_mode == CkNcpyModeDevice::IPC && csv_gpu_manager.use_shm) {
    // Use optimizations with POSIX shaerd memory
    // Allocate blocks on device comm buffer
    DeviceManager* dm = csv_gpu_manager.device_map[CkMyPe()];

    for (int i = 0; i < numops; i++) {
#if CMK_SMP
      CmiLock(dm->lock);
#endif
      void* alloc_comm_buffer = dm->alloc_comm_buffer(buffers[i]->cnt);
      if (alloc_comm_buffer == nullptr) {
        CkAbort("PE %d, device %d: Not enough memory on device communication buffer (%zu free)",
            CkMyPe(), dm->global_index, dm->get_comm_buffer_free_size());
      }
      buffers[i]->comm_offset = (char*)alloc_comm_buffer - (char*)dm->comm_buffer->base_ptr;
      buffers[i]->device_idx = dm->global_index;
      buffers[i]->event_idx = findFreeIpcEvent(dm, buffers[i]->comm_offset);
      // Abort if no free IPC event was found
      // FIXME: Instead of aborting, we can maybe create IPC events on demand
      // (although they probably cannot be shared through the shared memory
      // allocated and shared between processes at init time)
      if (buffers[i]->event_idx == -1) {
        CkAbort("CUDA IPC event pool empty");
      }
#if CMK_SMP
      CmiUnlock(dm->lock);
#endif

      // Initiate transfer from source buffer to device comm buffer
      hapiCheck(cudaMemcpyAsync(alloc_comm_buffer, buffers[i]->ptr, buffers[i]->cnt,
            cudaMemcpyDeviceToDevice, buffers[i]->cuda_stream));

      // Record event
      cuda_ipc_device_info& my_device_info = csv_gpu_manager.cuda_ipc_device_infos[dm->global_index];
      hapiCheck(cudaEventRecord(my_device_info.src_event_pool[buffers[i]->event_idx], buffers[i]->cuda_stream));
    }
  } else {
    // Use a naive host-staged mechanism
    // Allocate temporary host buffers and copy source buffers
    for (int i = 0; i < numops; i++) {
      buffers[i]->data_stored = true;
      hapiCheck(cudaMallocHost(&buffers[i]->data, buffers[i]->cnt));
      hapiCheck(cudaMemcpyAsync(buffers[i]->data, buffers[i]->ptr, buffers[i]->cnt,
            cudaMemcpyDeviceToHost, buffers[i]->cuda_stream));
    }

    // Wait for the copies to finish
    for (int i = 0; i < numops; i++) {
      hapiCheck(cudaStreamSynchronize(buffers[i]->cuda_stream));
    }
  }
#else
  // Post ucp_tag_send_nb's to send GPU data. When receiver receives the metadata,
  // it should post ucp_tag_recv_nb's to receive the GPU data.
  for (int i = 0; i < numops; i++) {
    CmiSendDevice(dest_pe, buffers[i]->ptr, buffers[i]->cnt, buffers[i]->tag);
  }
#endif // CMK_GPU_COMM
}
#endif // CMK_CUDA
