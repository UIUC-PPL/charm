/*
 * Copyright (c) 2019, Mellanox Technologies. All rights reserved.
 * See LICENSE in this directory.
 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string>

#include "converse.h"
#include "cmirdmautils.h"
#include "machine.h"
#include "pcqueue.h"
#include "machine-lrts.h"
#include "machine-rdma.h"
#include "machine-common-core.C"

// UCX  headers
#include <ucp/api/ucp.h>
#include <ucs/type/status.h>
#include <ucs/datastruct/mpool.h>

#if CMK_USE_PMI || CMK_USE_SIMPLEPMI
#include "runtime-pmi.C"
#elif CMK_USE_PMI2
#include "runtime-pmi2.C"
#elif CMK_USE_PMIX
#include "runtime-pmix.C"
#endif

#define CmiSetMsgSize(msg, sz)    ((((CmiMsgHeaderBasic *)msg)->size) = (sz))

#define UCX_MSG_PROBE_THRESH            32768
#define UCX_MSG_NUM_RX_REQS             64
#define UCX_MSG_NUM_RX_REQS_MAX         1024
#define UCX_TAG_MSG_BITS                4
#define UCX_TAG_RMA_BITS                4
#define UCX_MSG_TAG_EAGER               UCS_BIT(0)
#define UCX_MSG_TAG_PROBE               UCS_BIT(1)
#define UCX_RMA_TAG_GET                 UCS_BIT(UCX_TAG_MSG_BITS + 1)
#define UCX_RMA_TAG_REG_AND_SEND_BACK   UCS_BIT(UCX_TAG_MSG_BITS + 2)
#define UCX_RMA_TAG_DEREG_AND_ACK       UCS_BIT(UCX_TAG_MSG_BITS + 3)
#define UCX_MSG_TAG_MASK                UCS_MASK(UCX_TAG_MSG_BITS)
#define UCX_RMA_TAG_MASK                (UCS_MASK(UCX_TAG_RMA_BITS) << UCX_TAG_MSG_BITS)
#define UCX_MSG_TAG_MASK_FULL           0xffffffffffffffffUL

#define UCX_LOG_PRIO 50 // Disabled by default

enum : unsigned char {
    UCX_SEND_OP,    // Regular Send using UcxSendMsg
    UCX_RMA_OP_PUT, // RMA Put operation using UcxRmaOp
    UCX_RMA_OP_GET  // RMA Get operation using UcxRmaOp
};

#define UCX_LOG(prio, fmt, ...) \
    do { \
        if (prio >= UCX_LOG_PRIO) { \
            CmiPrintf("UCX:%d-%d:%s> " fmt"\n",CmiMyNode(), CmiMyRank(), __func__, ##__VA_ARGS__); \
        } \
    } while (0)

#define UCX_REQUEST_FREE(req) \
    do { \
        req->msgBuf    = NULL; \
        req->completed = 0; \
        ucp_request_free(req); \
    } while(0)


typedef struct UcxRequest
{
    void           *msgBuf;
    int            idx;
    int            completed;
#if CMK_ONESIDED_IMPL
    void           *ncpyAck;
    ucp_rkey_h     rkey;
#endif
} UcxRequest;

typedef struct UcxContext
{
    ucp_context_h     context;
    ucp_worker_h      worker;
    ucp_ep_h          *eps;
    UcxRequest        **rxReqs;
#if CMK_SMP
    PCQueue           txQueue;
#endif
    int               eagerSize;
    int               numRxReqs;
} UcxContext;

#ifdef CMK_SMP
typedef struct UcxPendingRequest
{
    int                 state;
    int                 index;
    void                *msgBuf;
    int                 size;
    ucp_tag_t           tag;
    int                 dNode;
    int                 op;
    ucp_send_callback_t cb;
} UcxPendingRequest;
#endif

static UcxContext ucxCtx;

static void UcxRxReqCompleted(void *request, ucs_status_t status,
                              ucp_tag_recv_info_t *info);
static void UcxPrepostRxBuffers();

#if CMK_ONESIDED_IMPL
#include "machine-onesided.h"
#endif

#define UCX_CHECK_STATUS(_status, _str) \
{ \
    if (UCS_STATUS_IS_ERR(_status)) { \
        CmiAbort("UCX: " _str " failed: %s", ucs_status_string(_status)); \
    } \
}

#define UCX_CHECK_RET(_ret, _str, _cond) \
{ \
    if (_cond) { \
        CmiAbort("UCX: " _str " failed: %d", _ret); \
    } \
}

#define UCX_CHECK_PMI_RET(_ret, _str) UCX_CHECK_RET(_ret, _str, _ret)

void UcxRequestInit(void *request)
{
    UcxRequest *req = (UcxRequest*)request;
    req->msgBuf     = NULL;
    req->idx        = -1;
    req->completed  = 0;
}

static void UcxInitEps(int numNodes, int myId)
{
    size_t addrlen;
    ucp_address_t *address;
    ucs_status_t status;
    ucp_ep_params_t eParams;
    ucp_ep_h ep;
    int i, j, ret, peer, maxkey, maxval, parts, len, partLen;
    char *keys, *addrp, *remoteAddr;

    ret = runtime_get_max_keylen(&maxkey);
    UCX_CHECK_PMI_RET(ret, "UcxInitEps: runtime_get_max_keylen error");
    ret = runtime_get_max_vallen(&maxval);
    UCX_CHECK_PMI_RET(ret, "UcxInitEps: runtime_get_max_vallen error");

    // Reduce maxval value, because with PMI1 it has to fit cmd + key + value
    maxval -= 48;
    CmiEnforce(maxval > 0);

    keys = (char*)CmiAlloc(maxkey);
    CmiEnforce(keys);

    ucxCtx.eps = (ucp_ep_h*)CmiAlloc(sizeof(ucp_ep_h)*numNodes);
    CmiEnforce(ucxCtx.eps);

    status = ucp_worker_get_address(ucxCtx.worker, &address, &addrlen);
    UCX_CHECK_STATUS(status, "UcxInitEps: ucp_worker_get_address error");
    CmiEnforce(addrlen < std::numeric_limits<int>::max()); //address should fit to int

    parts = (addrlen / maxval) + 1;

    // Publish number of address parts at first
    ret = snprintf(keys, maxkey, "UCX-size-%d", myId);
    UCX_CHECK_RET(ret, "UcxInitEps: snprintf error", (ret <= 0));
    ret = runtime_kvs_put(keys, &parts, sizeof(parts));
    UCX_CHECK_PMI_RET(ret, "UcxInitEps: runtime_kvs_put error");

    addrp = (char*)address;
    len   = (int)addrlen;
    for (i = 0; i < parts; ++i) {
        partLen = std::min(maxval, len);
        ret = snprintf(keys, maxkey, "UCX-%d-%d", myId, i);
        UCX_CHECK_RET(ret, "UcxInitEps: snprintf error", (ret <= 0));
        ret = runtime_kvs_put(keys, addrp, partLen);
        UCX_CHECK_PMI_RET(ret, "UcxInitEps: runtime_kvs_put error");
        addrp += partLen;
        len   -= partLen;
    }

    // Ensure that all nodes published their worker addresses
    ret = runtime_barrier();
    UCX_CHECK_PMI_RET(ret, "UcxInitEps: runtime_barrier");

    ucp_worker_release_address(ucxCtx.worker, address);

    for (i = 0; i < numNodes; ++i) {
        peer = (i + myId) % numNodes;
        if (peer == myId) continue;

        ret = snprintf(keys, maxkey, "UCX-size-%d", peer);
        UCX_CHECK_RET(ret, "UcxInitEps: snprintf error", (ret <= 0));
        ret = runtime_kvs_get(keys, &parts, sizeof(parts), peer);
        UCX_CHECK_PMI_RET(ret, "UcxInitEps: runtime_kvs_get error");

        remoteAddr = (char*)CmiAlloc(addrlen);
        CmiEnforce(remoteAddr);

        addrp = remoteAddr;
        len   = addrlen;
        for (j = 0; j < parts; ++j) {
            partLen = std::min(maxval, len);
            ret = snprintf(keys, maxkey, "UCX-%d-%d", peer, j);
            UCX_CHECK_RET(ret, "UcxInitEps: snprintf error", (ret <= 0));
            ret = runtime_kvs_get(keys, addrp, partLen, peer);
            UCX_CHECK_PMI_RET(ret, "UcxInitEps: runtime_kvs_get error");
            addrp += maxval;
            len   -= maxval;
        }

        eParams.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
        eParams.address    = (const ucp_address_t*)remoteAddr;

        status = ucp_ep_create(ucxCtx.worker, &eParams, &ucxCtx.eps[peer]);
        UCX_CHECK_STATUS(status, "ucp_ep_create failed");
        UCX_LOG(4, "Connecting to %d (ep %p)", peer, ucxCtx.eps[peer]);
        CmiFree(remoteAddr);
    }

    CmiFree(keys);
}

// Should be called for every node (not PE)
void LrtsInit(int *argc, char ***argv, int *numNodes, int *myNodeID)
{
    ucp_params_t cParams;
    ucp_config_t *config;
    ucp_worker_params_t wParams;
    ucs_status_t status;
    int ret;

    ret = runtime_init(myNodeID, numNodes);
    UCX_CHECK_PMI_RET(ret, "runtime_init");

    status = ucp_config_read("Charm++", NULL, &config);
    UCX_CHECK_STATUS(status, "ucp_config_read");

    // Initialize UCX context
    cParams.field_mask        = UCP_PARAM_FIELD_FEATURES          |
                                UCP_PARAM_FIELD_REQUEST_SIZE      |
                                UCP_PARAM_FIELD_TAG_SENDER_MASK   |
                                UCP_PARAM_FIELD_REQUEST_INIT      |
                                UCP_PARAM_FIELD_MT_WORKERS_SHARED |
                                UCP_PARAM_FIELD_ESTIMATED_NUM_EPS;
    cParams.features          = UCP_FEATURE_TAG | UCP_FEATURE_RMA;
    cParams.request_size      = sizeof(UcxRequest);
    cParams.tag_sender_mask   = 0ul;
    cParams.request_init      = UcxRequestInit;
    cParams.mt_workers_shared = 0;
    cParams.estimated_num_eps = *numNodes;

    status = ucp_init(&cParams, config, &ucxCtx.context);
    ucp_config_release(config);
    UCX_CHECK_STATUS(status, "ucp_init");

    // Create UCP worker
    wParams.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    wParams.thread_mode = UCS_THREAD_MODE_SINGLE;
    status = ucp_worker_create(ucxCtx.context, &wParams, &ucxCtx.worker);
    UCX_CHECK_STATUS(status, "ucp_worker_create");

    ucxCtx.numRxReqs = UCX_MSG_NUM_RX_REQS;
    if (CmiGetArgInt(*argv, "+ucx_num_rx_reqs", &ucxCtx.numRxReqs)) {
        if ((ucxCtx.numRxReqs <= 0) || (ucxCtx.numRxReqs > UCX_MSG_NUM_RX_REQS_MAX)) {
            CmiPrintf("UCX: Invalid number of RX reqs: %d\n", ucxCtx.numRxReqs);
            CmiAbort(__func__);
        }
    }

    // Eager messages should fit NcpyOperationInfo data.
    // Adjust rendezvous threshold accordingly.
    int thresh = UCX_MSG_PROBE_THRESH;
    CmiGetArgInt(*argv, "+ucx_rndv_thresh", &thresh);
    ucxCtx.eagerSize = std::max(LrtsGetMaxNcpyOperationInfoSize(), thresh);

    UcxInitEps(*numNodes, *myNodeID);

    UcxPrepostRxBuffers();

    // Ensure connects completion
    status = ucp_worker_flush(ucxCtx.worker);
    UCX_CHECK_STATUS(status, "ucp_worker_flush");

#if CMK_SMP
    ucxCtx.txQueue = PCQueueCreate();
#endif

    UCX_LOG(5, "Initialized: preposted reqs %d, rndv thresh %d\n",
            ucxCtx.numRxReqs, ucxCtx.eagerSize);
}

static inline UcxRequest* UcxPostRxReqInternal(ucp_tag_t tag, size_t size,
                                               ucp_tag_message_h msg)
{
    void *buf = CmiAlloc(size);
    UcxRequest *req;

    if (tag == UCX_MSG_TAG_EAGER) {
        req = (UcxRequest*)ucp_tag_recv_nb(ucxCtx.worker, buf,
                                           ucxCtx.eagerSize,
                                           ucp_dt_make_contig(1), tag,
                                           UCX_MSG_TAG_MASK,
                                           UcxRxReqCompleted);
    } else {
        CmiEnforce(tag == UCX_MSG_TAG_PROBE);
        req = (UcxRequest*)ucp_tag_msg_recv_nb(ucxCtx.worker, buf, size,
                                               ucp_dt_make_contig(1), msg,
                                               UcxRxReqCompleted);
    }

    CmiEnforce(!UCS_PTR_IS_ERR(req));
    UCX_LOG(3, "Posted RX buf %p size %zu, req %p, tag %zu, comp %d\n",
            req->msgBuf, size, req, tag, req->completed);

    // Request completed immediately
    if (req->completed) {
        if (!(tag & UCX_RMA_TAG_MASK)) {
            handleOneRecvedMsg(size, (char*)buf);
        }
    } else {
        req->msgBuf = buf;
    }

    return req;
}

static inline UcxRequest* UcxPostRxReq(ucp_tag_t tag, size_t size,
                                       ucp_tag_message_h msg)
{
    UcxRequest *req = UcxPostRxReqInternal(tag, size, msg);
    int idx = req->idx;

    do {
        if (req->completed) {
            UCX_REQUEST_FREE(req);

            if (tag & UCX_MSG_TAG_EAGER) {
                req = UcxPostRxReqInternal(UCX_MSG_TAG_EAGER, ucxCtx.eagerSize, NULL);
                req->idx = idx;
                ucxCtx.rxReqs[idx] = req;
            } else {
                return NULL;
            }
        }
        else {
            return req;
        }
    }
    while (1);
}

static inline UcxRequest* UcxHandleRxReq(UcxRequest *request, char *rxBuf,
                                         size_t size, ucp_tag_t tag, int idx)
{
    if (!(tag & UCX_RMA_TAG_MASK)) {
        handleOneRecvedMsg(size, rxBuf);
    }

    UCX_REQUEST_FREE(request);

    if (tag & UCX_MSG_TAG_EAGER) {
        ucxCtx.rxReqs[idx]      = UcxPostRxReq(UCX_MSG_TAG_EAGER,
                                               ucxCtx.eagerSize, NULL);
        ucxCtx.rxReqs[idx]->idx = idx;
        return ucxCtx.rxReqs[idx];
    }

    return NULL;
}

static void UcxRxReqCompleted(void *request, ucs_status_t status,
                              ucp_tag_recv_info_t *info)
{
    UcxRequest *req = (UcxRequest*)request;

    UCX_LOG(3, "status %d len %zu, buf %p, req %p, tag %zu\n",
            status,  info->length, req->msgBuf, request, info->sender_tag);

    if (ucs_unlikely(status == UCS_ERR_CANCELED)) {
        return;
    }

#if CMK_ONESIDED_IMPL
    if (info->sender_tag & UCX_RMA_TAG_REG_AND_SEND_BACK) {

        // Register the source buffer and send back to destination to perform GET

        NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)(req->msgBuf);
        UCX_LOG(4, "Got ncpy size %d (meta size %d)", ncpyOpInfo->srcSize, ncpyOpInfo->ncpyOpInfoSize);
        resetNcpyOpInfoPointers(ncpyOpInfo);

        UcxRdmaInfo *info = (UcxRdmaInfo *)(ncpyOpInfo->srcLayerInfo + CmiGetRdmaCommonInfoSize());

        UcxMemMap(info,
                  (void *)ncpyOpInfo->srcPtr,
                  ncpyOpInfo->srcSize);

        ncpyOpInfo->isSrcRegistered = 1;

        ncpyOpInfo->freeMe = CMK_FREE_NCPYOPINFO; // It's a message, not a realy ncpy Obj
        UCX_LOG(4, "Reset ncpy size %d (meta size %d)", ncpyOpInfo->destSize, ncpyOpInfo->ncpyOpInfoSize);

        // send back to destination process to perform GET
        UcxSendMsg(CmiNodeOf(ncpyOpInfo->destPe), ncpyOpInfo->destPe,
                   ncpyOpInfo->ncpyOpInfoSize, (char*)ncpyOpInfo,
                   UCX_RMA_TAG_GET, UcxRmaSendCompletedAndFree);

    } else if (info->sender_tag & UCX_RMA_TAG_GET) {
        NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)(req->msgBuf);
        resetNcpyOpInfoPointers(ncpyOpInfo);

        ncpyOpInfo->freeMe = CMK_FREE_NCPYOPINFO; // It's a message, not a real ncpy Obj
        UcxRmaOp(ncpyOpInfo, UCX_RMA_OP_GET);

    } else if (info->sender_tag & UCX_RMA_TAG_DEREG_AND_ACK) {
        NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)(req->msgBuf);
        resetNcpyOpInfoPointers(ncpyOpInfo);
        ncpyOpInfo->freeMe = CMK_FREE_NCPYOPINFO;

        if(CmiMyNode() == CmiNodeOf(ncpyOpInfo->srcPe)) { // source node
            LrtsDeregisterMem(ncpyOpInfo->srcPtr,
                              ncpyOpInfo->srcLayerInfo + CmiGetRdmaCommonInfoSize(),
                              ncpyOpInfo->srcPe,
                              ncpyOpInfo->srcRegMode);

            ncpyOpInfo->isSrcRegistered = 0; // Set isSrcRegistered to 0 after de-registration

            // Invoke source ack
            if(ncpyOpInfo->opMode != CMK_BCAST_EM_API) {
                ncpyOpInfo->opMode = CMK_EM_API_SRC_ACK_INVOKE;
                CmiInvokeNcpyAck(ncpyOpInfo);
            }

        } else if(CmiMyNode() == CmiNodeOf(ncpyOpInfo->destPe)) { // destination node

            LrtsDeregisterMem(ncpyOpInfo->destPtr,
                              ncpyOpInfo->destLayerInfo + CmiGetRdmaCommonInfoSize(),
                              ncpyOpInfo->destPe,
                              ncpyOpInfo->destRegMode);

            ncpyOpInfo->isDestRegistered = 0; // Set isDestRegistered to 0 after de-registration

            // Invoke destination ack
            ncpyOpInfo->opMode = CMK_EM_API_DEST_ACK_INVOKE;
            CmiInvokeNcpyAck(ncpyOpInfo);

        } else {
            CmiAbort(" Cannot de-register on a different node than the source or destinaton");
        }
    }
#endif

    if (req->msgBuf != NULL) {
        // Request is not completed immediately
        UcxHandleRxReq(req, (char*)req->msgBuf, info->length, info->sender_tag, req->idx);
    } else {
        req->completed = 1;
    }
}

static void UcxPrepostRxBuffers()
{
    int i;

    ucxCtx.rxReqs = (UcxRequest**)CmiAlloc(sizeof(UcxRequest*) * ucxCtx.numRxReqs);

    for (i = 0; i < ucxCtx.numRxReqs; i++) {
        ucxCtx.rxReqs[i] = UcxPostRxReq(UCX_MSG_TAG_EAGER, ucxCtx.eagerSize, NULL);
        ucxCtx.rxReqs[i]->idx = i;
    }
    UCX_LOG(3, "UCX: preposted %d rx requests", ucxCtx.numRxReqs);
}

void UcxTxReqCompleted(void *request, ucs_status_t status)
{
    UcxRequest *req = (UcxRequest*)request;

    CmiEnforce(status == UCS_OK);
    CmiEnforce(req->msgBuf);

    UCX_LOG(3, "TX req %p completed, free msg %p", req, req->msgBuf);
    CmiFree(req->msgBuf);
    UCX_REQUEST_FREE(req);
}

// tag may carry RMA tag
inline void* UcxSendMsg(int destNode, int destPE, int size, char *msg,
                        ucp_tag_t tag, ucp_send_callback_t cb)
{
    ucp_tag_t sTag;

    // Combine tag and sTag: sTag defines msg protocol, tag may indicate RMA requests
    sTag  = (size > ucxCtx.eagerSize) ? UCX_MSG_TAG_PROBE : UCX_MSG_TAG_EAGER;

    // Auxilliary messages (which add bits to the tag) should use eager.
    CmiEnforce((tag == 0ul) || (sTag == UCX_MSG_TAG_EAGER));

    sTag |= tag;

    UCX_LOG(3, "destNode=%i destPE=%i size=%i msg=%p, tag=%" PRIu64,
            destNode, destPE, size, msg, tag);
#if CMK_SMP
    UcxPendingRequest *req = (UcxPendingRequest*)CmiAlloc(sizeof(UcxPendingRequest));
    req->msgBuf = msg;
    req->size   = size;
    req->tag    = sTag;
    req->dNode  = destNode;
    req->cb     = cb;
    req->op     = UCX_SEND_OP;   // Mark this request as a regular message (UCX_SEND_OP)

    UCX_LOG(3, " --> (PE=%i) enq msg (queue depth=%i), dNode %i, size %i",
            CmiMyPe(), PCQueueLength(ucxCtx.txQueue), destNode, size);
    PCQueuePush(ucxCtx.txQueue, (char *)req);
#else
    UcxRequest *req;

    req = (UcxRequest*)ucp_tag_send_nb(ucxCtx.eps[destNode], msg, size,
                                       ucp_dt_make_contig(1), sTag, cb);
    if (!UCS_PTR_IS_PTR(req)) {
        CmiEnforce(!UCS_PTR_IS_ERR(req));
        return NULL;
    }

    req->msgBuf = msg;
#endif

    return req;
}

/**
 * In non-SMP mode, this is used to send a message.
 * In CMK_SMP mode, this is called by a worker thread to send a message.
 */
CmiCommHandle LrtsSendFunc(int destNode, int destPE, int size, char *msg, int mode)
{

    void *req;

    CmiSetMsgSize(msg, size);

    req = UcxSendMsg(destNode, destPE, size, msg, 0ul, UcxTxReqCompleted);
    if (req == NULL) {
        /* Request completed in place or error occured */
        UCX_LOG(3, "Sent msg %p (len %d) inline", msg, size);
        CmiFree(msg);
        return NULL;
    }

    return (CmiCommHandle)req;
}

void LrtsPreCommonInit(int everReturn)
{
    UCX_LOG(2, "LrtsPreCommonInit");
}

void LrtsPostCommonInit(int everReturn)
{
    UCX_LOG(2, "LrtsPostCommonInit");
}

#if CMK_SMP
static inline int ProcessTxQueue()
{
    UcxPendingRequest *req;

    req = (UcxPendingRequest*)PCQueuePop(ucxCtx.txQueue);
    if (req)
    {
        if(req->op == UCX_SEND_OP) { // Regular Message
            ucs_status_ptr_t status_ptr;
            status_ptr = ucp_tag_send_nb(ucxCtx.eps[req->dNode], req->msgBuf,
                                         req->size, ucp_dt_make_contig(1),
                                         req->tag, req->cb);

            if (!UCS_PTR_IS_PTR(status_ptr)) {
                CmiEnforce(!UCS_PTR_IS_ERR(status_ptr));

                if(req->tag & UCX_RMA_TAG_MASK) {
                    NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)(req->msgBuf);
                    if(ncpyOpInfo->freeMe == CMK_FREE_NCPYOPINFO)
                        CmiFree(ncpyOpInfo);
                } else {
                    CmiFree(req->msgBuf);
                }
            } else {
                ((UcxRequest*)status_ptr)->msgBuf = req->msgBuf;
            }
        }
#if CMK_ONESIDED_IMPL
        else if(req->op == UCX_RMA_OP_GET || req->op == UCX_RMA_OP_PUT) { // RMA Get or Put

            // Post the GET or PUT operation from the comm thread
            UcxRmaOp((NcpyOperationInfo *)(req->msgBuf), req->op);
        }
#endif
        else {
          CmiAbort("[%d][%d][%d] UCX:ProcessTxQueue req->op(%d) is Invalid\n", CmiMyPe(), CmiMyNode(), CmiMyRank(), req->op);
        }
        CmiFree(req);
        return 1;
    }
    return 0;
}
#endif

void LrtsAdvanceCommunication(int whileidle)
{
    ucp_tag_message_h msg;
    ucp_tag_recv_info_t info;
    int cnt;

    do {
       cnt = ucp_worker_progress(ucxCtx.worker);

       // Probe with full tag mask to avoid long traversing thru unexpected
       // queue of eager messages (messages with non-full mask added to the
       // same unexpected queue)
       msg = ucp_tag_probe_nb(ucxCtx.worker, UCX_MSG_TAG_PROBE,
                              UCX_MSG_TAG_MASK_FULL, 1, &info);
       if (msg != NULL) {
           UCX_LOG(3, "Got msg %p, len %zu\n", msg, info.length);
           UcxPostRxReq(UCX_MSG_TAG_PROBE, info.length, msg);
       }

#if CMK_SMP
       cnt += ProcessTxQueue();
#endif
    } while (cnt);
}

void LrtsDrainResources()
{
    int ret;
    LrtsAdvanceCommunication(0);
    ret = runtime_barrier();
    UCX_CHECK_PMI_RET(ret, "runtime_barrier");
}

void LrtsExit(int exitcode)
{
    int ret;
    int i;
    UcxRequest *req;
    ucs_status_t status;

    UCX_LOG(4, "LrtsExit");

    LrtsAdvanceCommunication(0);

    for (i = 0; i < ucxCtx.numRxReqs; ++i) {
        req = ucxCtx.rxReqs[i];
        CmiFree(req->msgBuf);
        ucp_request_cancel(ucxCtx.worker, req);
        ucp_request_free(req);
    }

    ucp_worker_destroy(ucxCtx.worker);
    ucp_cleanup(ucxCtx.context);

    CmiFree(ucxCtx.eps);
    CmiFree(ucxCtx.rxReqs);
#if CMK_SMP
    PCQueueDestroy(ucxCtx.txQueue);
#endif

    if(!CharmLibInterOperate || userDrivenMode) {
        ret = runtime_barrier();
        UCX_CHECK_PMI_RET(ret, "runtime_barrier");

        ret = runtime_fini();
        UCX_CHECK_PMI_RET(ret, "runtime_fini");
        if (!userDrivenMode) {
          exit(exitcode);
        }
    }
}

#if CMK_MACHINE_PROGRESS_DEFINED
void CmiMachineProgressImpl()
{
    if (CmiMyRank() == CmiMyNodeSize()) {
        CommunicationServerThread(0);
    }
}
#endif

// In CMK_SMP, this is called by worker thread
void LrtsPostNonLocal()
{
    UCX_LOG(2, "LrtsPostNonLocal");
}

void LrtsAbort(const char *message)
{
    UCX_LOG(2, "LrtsAbort '%s'", message);
    exit(1);
    CMI_NORETURN_FUNCTION_END
}

void  LrtsNotifyIdle()
{
    UCX_LOG(2, "LrtsNotifyIdle");
}

void  LrtsBeginIdle()
{
    UCX_LOG(2, "LrtsBeginIdle");
}

void  LrtsStillIdle()
{
    UCX_LOG(2, "LrtsStillIdle");
}

void  LrtsBarrier()
{
    int ret;
    ret = runtime_barrier();
    UCX_CHECK_PMI_RET(ret, "runtime_barrier");
}

#if CMK_ONESIDED_IMPL
#include "machine-onesided.C"
#endif
