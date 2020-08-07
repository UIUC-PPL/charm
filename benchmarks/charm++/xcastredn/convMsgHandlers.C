#include "reductionBenchmark.h"
#include "convMsgHandlers.h"
#include "myChareArray.h"
#include "testController.h"

/// Converse broadcast and reduction handler function IDs. Global vars.
int bcastHandlerID, rednHandlerID, bcastConverterID;
/// Array elements local to each pe. Used by the conv to array bcast converter
CpvDeclare(std::vector<MyChareArray*>, localElems);
/// Pointer to the mainchare on pe 0 used by the converse redn handler
TestController *mainChare;



/// Converse reduction merge function triggered at each vertex along the reduction spanning tree
void* convRedn_sum (size_t *size, void *local, void **remote, int count)
{
    CkReductionMsg *localMsg = CkReductionMsg::unpack( EnvToUsr( (envelope*)local ) );
    double *dataBuf          = reinterpret_cast<double*>( localMsg->getData() );
    int msgSize              = localMsg->getSize()/sizeof(double);
    // Reduce all the remote msgs from children in the tree
    for (int i=0; i < count; i++)
    {
        envelope *aEnv       = (envelope*)( remote[i] );
        CkReductionMsg *aMsg = CkReductionMsg::unpack( EnvToUsr(aEnv) );
        CkAssert( localMsg->getSize() == aMsg->getSize() );
        double *anotherBuf   = reinterpret_cast<double*>( aMsg->getData() );
        for (int j=0; j< msgSize; j++)
            dataBuf[j] += anotherBuf[j];
    }
    // Repack the local msg
    CkReductionMsg::pack(localMsg);
    // Return a handle to the final reduced msg
    return local;
}



// Converse Reduction msg handler triggered at the root of the converse reduction
void convRednHandler(void *env)
{
    CkReductionMsg *msg = CkReductionMsg::unpack( EnvToUsr((envelope*)env) );
    #ifdef VERBOSE_OPERATION
        CkPrintf("\n[%d] Converse reduction handler triggered",CkMyPe());
    #endif
    mainChare->receiveReduction(msg);
}



/// Converse message handler that translates a converse broadcast to a charm array bcast
void convBcastHandler(void *env)
{
    #ifdef VERBOSE_OPERATION
        CkPrintf("\n[%d] Received converse bcast. Gonna deliver to local array elements",CkMyPe());
    #endif
    /// Get an unpacked charm msg from the envelope
    DataMsg *msg = DataMsg::unpack( EnvToUsr((envelope*)env) );
    /// Deliver to each local element
    int numElems = CpvAccess(localElems).size();
    for (int i=0; i < numElems; i++)
    {
        DataMsg *aMsgPtr = (numElems - i > 1) ? (DataMsg*)CkCopyMsg((void**)&msg) : msg;
        CpvAccess(localElems)[i]->crunchData(aMsgPtr);
    }
    /// If there are no local elements, delete the incoming msg
    if (numElems == 0)
        delete msg;
}



// Register the converse msg handlers used for the converse bcast/redn test
void registerHandlers()
{
    bcastHandlerID   = CmiRegisterHandler(convBcastHandler);
    rednHandlerID    = CmiRegisterHandler(convRednHandler);
    CpvInitialize(std::vector<MyChareArray*>, localElems);
}

#include "reductionBenchmark.def.h"

