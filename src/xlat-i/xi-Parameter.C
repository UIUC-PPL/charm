#include "xi-Entry.h"
#include "xi-Parameter.h"
#include "xi-Type.h"
#include "xi-Value.h"
#include "xi-Chare.h"

namespace xi {

/******************* C/C++ Parameter Marshalling ******************
For entry methods like:
        entry void foo(int nx,double xarr[nx],complex<float> yarr[ny],long ny);

We generate code on the call-side (in the proxy entry method) to
create a message and copy the user's parameters into it.  Scalar
fields are PUP'd, arrays are just memcpy'd.

The message looks like this:

messagestart>--------- PUP'd data ----------------
        |  PUP'd nx
        |  PUP'd offset-to-xarr (from array start, int byte count)
        |  PUP'd length-of-xarr (in elements)
        |  PUP'd offset-to-yarr
        |  PUP'd length-of-yarr (in elements)
        |  PUP'd ny
        +-------------------------------------------
        |  alignment gap (to multiple of 16 bytes)
arraystart>------- xarr data ----------
        | xarr[0]
        | xarr[1]
        | ...
        | xarr[nx-1]
        +------------------------------
        |  alignment gap (for yarr-elements)
        +--------- yarr data ----------
        | yarr[0]
        | yarr[1]
        | ...
        | yarr[ny-1]
        +------------------------------

On the recieve side, all the scalar fields are PUP'd to fresh
stack copies, and the arrays are passed to the user as pointers
into the message data-- so there's no copy on the receive side.

The message is freed after the user entry returns.
*/
Parameter::Parameter(int Nline, Type* Ntype, const char* Nname, const char* NarrLen,
                     Value* Nvalue)
    : type(Ntype),
      name(Nname),
      arrLen(NarrLen),
      val(Nvalue),
      line(Nline),
      byConst(false),
      conditional(0),
      given_name(Nname),
      podType(false),
      rdma(CMK_REG_NO_ZC_MSG),
      firstRdma(false),
      firstDeviceRdma(false) {
  if (isMessage()) {
    name = "impl_msg";
  }
  if (name == NULL && !isVoid()) { /*Fabricate a unique name for this marshalled param.*/
    static int unnamedCount = 0;
    name = new char[50];
    sprintf((char*)name, "impl_noname_%x", unnamedCount++);
  }
  byReference = false;
  declaredReference = false;
  if ((arrLen == NULL) && (val == NULL)) { /* Consider passing type by reference: */
    if (type->isNamed()) {                 /* Some user-defined type: pass by reference */
      byReference = true;
    }
    if (type->isReference()) {
      byReference = true;
      declaredReference = true;
      /* Clip off the ampersand--we'll add
         it back ourselves in Parameter::print. */
      type = type->deref();
    }
    if (type->isConst()) {
      byConst = true;
      type = type->deref();
    }
  }
}

ParamList::ParamList(ParamList* pl)
    : manyPointers(false), param(pl->param), next(pl->next) {}

int ParamList::print(XStr& str, int withDefaultValues, int useConst, int fwdNum) {
  fwdNum = param->print(str, withDefaultValues, useConst, fwdNum);
  if (next) {
    str << ", ";
    fwdNum = next->print(str, withDefaultValues, useConst, fwdNum);
  }
  return fwdNum;
}

void ParamList::printTypes(XStr& str, int withDefaultValues, int useConst) {
  XStr typeStr;
  param->getType()->print(typeStr);
  str << typeStr;
  if (next) {
    str << ", ";
    next->printTypes(str, withDefaultValues, useConst);
  }
}

int Parameter::print(XStr& str, int withDefaultValues, int useConst, int fwdNum) {
  if (isRdma()) {
    if (isDevice()) {
      str << "CkDeviceBuffer deviceBuffer_" << name;
    } else {
      str << "CkNcpyBuffer ncpyBuffer_" << name;
    }
  } else if (arrLen != NULL) {  // Passing arrays by const pointer-reference
    if (useConst) str << "const ";
    str << type << " *";
    if (name != NULL) str << name;
  } else {
    if (conditional) {
      str << type << " *" << name;
    } else if (byReference) {  // Pass named types by const C++ reference
      if (fwdNum) {
        str << "Fwd" << fwdNum++ << " &&";
      } else {
        if (useConst || byConst) str << "const ";
        str << type << " &";
      }
      if (name != NULL) str << name;
    } else {  // Pass everything else by value
              // @TODO uncommenting this requires that PUP work on const types
              // if (byConst) str << "const ";
      //"void" shouldn't be typed in the param list
      // to have CkEntryOptions as the last argument
      if (!type->isVoid()) str << type;
      if (name != NULL) str << " " << name;
      if (withDefaultValues && val != NULL) {
        str << " = ";
        val->print(str);
      }
    }
  }
  return fwdNum;
}

void ParamList::printAddress(XStr& str) {
  param->printAddress(str);
  if (next) {
    str << ", ";
    next->printAddress(str);
  }
}

void Parameter::printAddress(XStr& str) {
  type->print(str);
  str << "*";
  if (name != NULL) str << " " << name;
}

void ParamList::printValue(XStr& str) {
  param->printValue(str);
  if (next) {
    str << ", ";
    next->printValue(str);
  }
}

void Parameter::printValue(XStr& str) {
  if (arrLen == NULL) str << "*";
  if (name != NULL) str << name;
}

int ParamList::orEach(pred_t f) {
  ParamList* cur = this;
  int ret = 0;
  do {
    ret |= ((cur->param)->*f)();
  } while (NULL != (cur = cur->next));
  return ret;
}

void ParamList::callEach(fn_t f, XStr& str) {
  ParamList* cur = this;
  do {
    ((cur->param)->*f)(str);
  } while (NULL != (cur = cur->next));
}

void ParamList::callEach(rdmafn_t f, XStr& str, bool isArray) {
  ParamList* cur = this;
  do {
    ((cur->param)->*f)(str, isArray);
  } while (NULL != (cur = cur->next));
}

void ParamList::callEach(rdmarecvfn_t f, XStr& str, bool genRdma, bool isSDAGGen) {
  ParamList* cur = this;
  int count = 0; // Used for the index of buffPtrs for Zcpy Post API
  do {
    ((cur->param)->*f)(str, genRdma, isSDAGGen, count);
  } while (NULL != (cur = cur->next));
}

void ParamList::callEach(rdmarecvfn_t f, XStr& str, bool genRdma, bool isSDAGGen, int &count) {
  ParamList* cur = this;
  do {
    ((cur->param)->*f)(str, genRdma, isSDAGGen, count);
  } while (NULL != (cur = cur->next));
}

void ParamList::callEach(rdmaheterofn_t f, XStr& str, bool genRdma, bool device) {
  ParamList* cur = this;
  do {
    ((cur->param)->*f)(str, genRdma, device);
  } while (NULL != (cur = cur->next));
}

void ParamList::callEach(rdmaheterocountfn_t f, XStr& str, bool genRdma, bool isSDAGGen, bool device) {
  ParamList* cur = this;
  int count = 0; // Used for the index of buffPtrs for Zcpy Post API
  do {
    ((cur->param)->*f)(str, genRdma, isSDAGGen, device, count);
  } while (NULL != (cur = cur->next));
}

void ParamList::callEach(rdmaheterocountfn_t f, XStr& str, bool genRdma, bool isSDAGGen, bool device, int& count) {
  ParamList* cur = this;
  do {
    ((cur->param)->*f)(str, genRdma, isSDAGGen, device, count);
  } while (NULL != (cur = cur->next));
}

void ParamList::callEach(rdmadevicefn_t f, XStr& str, int& index) {
  ParamList* cur = this;
  do {
    ((cur->param)->*f)(str, index);
  } while (NULL != (cur = cur->next));
}

int ParamList::hasConditional() { return orEach(&Parameter::isConditional); }

void ParamList::size(XStr& str)
{
  str << "  int impl_off=0;\n";
  int hasArrays = orEach(&Parameter::isArray);
  if (hasArrays)
  {
    str << "  int impl_arrstart=0;\n";
    callEach(&Parameter::marshallRegArraySizes, str);
  }

  bool hasrdma = hasRdma();
  bool hasrecvrdma = hasRecvRdma();

  Chare* container = entry->getContainer();
  bool isP2P = (container->isChare() || container->isForElement());

  // Generate device-related code only when this condition is met
  bool deviceRdmaSupported = (hasDevice() && isP2P);

  if (hasrdma)
  {
    if (deviceRdmaSupported)
    {
      str << "  int impl_num_device_rdma_fields = " << entry->numRdmaDeviceParams
          << ";\n";
      str << "  int dest_pe;\n";
      if (container->isChare())
      {
        // TODO: Following code doesn't work, don't support singleton chares for now
        // str << "  dest_pe = CkRdmaGetDestPEChare(ckGetChareID().onPE,
        // ckGetChareID().objPtr);\n";
        str << "  CkAbort(\"Singleton chares are not supported\");\n";
      }
      else if (container->isArray())
      {
        str << "  dest_pe = ckLocMgr()->lastKnown(ckGetIndex());\n";
      }
      else if (container->isGroup() || container->isNodeGroup())
      {
        str << "  dest_pe = ckGetGroupPe();\n";
      }
      else
      {
        str << "  CkAbort(\"Unknown container type\");\n";
      }
      str << "  CkDeviceBuffer* device_buffers[" << entry->numRdmaDeviceParams << "];\n";
      int device_rdma_index = 0;
      callEach(&Parameter::marshallDeviceRdmaParameters, str, device_rdma_index);
      str << "  CkRdmaDeviceOnSender(dest_pe, impl_num_device_rdma_fields, "
             "device_buffers);\n";
    }
    else if (hasDevice())
    {
      // Abort ASAP if broadcast or has host-side zero-copy
      if (!isP2P)
      {
        str << "  CkAbort(\"Broadcast not supported with device buffers\");\n";
      }
      if (hasSendRdma() || hasRecvRdma())
      {
        str << "  CkAbort(\"Host-side RDMA cannot be used along with device RDMA\");\n";
      }
    }
    else
    {
      str << "  int impl_num_rdma_fields = "
          << entry->numRdmaSendParams + entry->numRdmaRecvParams << ";\n";
      // Root node is pupped on the source as it is required for ZC Bcast when the source
      // is non-zero
      str << "  int impl_num_root_node = CkMyNode();\n";
      callEach(&Parameter::marshallRdmaParameters, str, true);
    }
  }
  str << "  { //Find the size of the PUP'd data\n";
  str << "    PUP::sizer implP;\n";
  callEach(&Parameter::pup, str);
  if (hasrdma)
  {
    if (deviceRdmaSupported)
    {
      str << "    implP|impl_num_device_rdma_fields;\n";
      callEach(&Parameter::pupRdma, str, true, true);
    }
    else if (!hasDevice())
    {
      str << "    implP|impl_num_rdma_fields;\n";
      str << "    implP|impl_num_root_node;\n";
      // All rdma parameters have to be pupped at the start
      callEach(&Parameter::pupRdma, str, true, false);
    }
  }
  if (hasArrays)
  { /*round up pup'd data length--that's the first array*/
    str << "    impl_arrstart=CK_ALIGN(implP.size(),16);\n";
    str << "    impl_off+=impl_arrstart;\n";
  }
  else /*No arrays--no padding*/
    str << "    impl_off+=implP.size();\n";
  str << "  }\n";
}

/** marshalling: pack fields into flat byte buffer **/
void ParamList::marshall(XStr& str, XStr& entry_str) {
  if (isVoid())
    str << "  void *impl_msg = CkAllocSysMsg(impl_e_opts);\n";
  else if (isMarshalled()) {
    str << "  //Marshall: ";
    print(str, 0);
    str << "\n";
    // First pass: find sizes
    size(str);
    // Now that we know the size, allocate the packing buffer
    int hasArrays = orEach(&Parameter::isArray);

    bool hasrdma = hasRdma();
    bool hasrecvrdma = hasRecvRdma();

    Chare *container = entry->getContainer();
    bool isP2P = (container->isChare() || container->isForElement());

    // Generate device-related code only when this condition is met
    bool deviceRdmaSupported = (hasDevice() && isP2P);
    if (hasConditional())
      str << "  MarshallMsg_" << entry_str << " *impl_msg=CkAllocateMarshallMsgT<MarshallMsg_"
          << entry_str << ">(impl_off,impl_e_opts);\n";
    else
      str << "  CkMarshallMsg *impl_msg=CkAllocateMarshallMsg(impl_off,impl_e_opts);\n";
    // Second pass: write the data
    str << "  { //Copy over the PUP'd data\n";
    str << "    PUP::toMem implP((void *)impl_msg->msgBuf);\n";
    if (hasRdma()) {
      if (deviceRdmaSupported) {
        str << "    implP|impl_num_device_rdma_fields;\n";
        callEach(&Parameter::pupRdma, str, true, true);
      } else if (!hasDevice()) {
        str << "    implP|impl_num_rdma_fields;\n";
        str << "    implP|impl_num_root_node;\n";
        callEach(&Parameter::pupRdma, str, true, false);
      }
    }
    callEach(&Parameter::pup, str);
    callEach(&Parameter::copyPtr, str);
    str << "  }\n";
    if (hasArrays) {  // Marshall each array
      str << "  char *impl_buf=impl_msg->msgBuf+impl_arrstart;\n";
      callEach(&Parameter::marshallArrayData, str);
    }
    if (hasrdma) {
      if (deviceRdmaSupported) {
        str << "  CMI_ZC_MSGTYPE((char *)UsrToEnv(impl_msg)) = CMK_ZC_DEVICE_MSG;\n";
      } else if (!hasDevice()) {
        // Only need to set message header when there is no device buffer involved
        if (isP2P) {
          if(hasSendRdma())
            str << "  CMI_ZC_MSGTYPE((char *)UsrToEnv(impl_msg)) = CMK_ZC_P2P_SEND_MSG;\n";
          else if(hasrecvrdma)
            str << "  CMI_ZC_MSGTYPE((char *)UsrToEnv(impl_msg)) = CMK_ZC_P2P_RECV_MSG;\n";
        } else { // Mark a Ncpy Bcast message to intercept it in the send code path
          if(hasSendRdma())
            str << "  CMI_ZC_MSGTYPE((char *)UsrToEnv(impl_msg)) = CMK_ZC_BCAST_SEND_MSG;\n";
          else if(hasrecvrdma)
            str << "  CMI_ZC_MSGTYPE((char *)UsrToEnv(impl_msg)) = CMK_ZC_BCAST_RECV_MSG;\n";
        }
      }
    }
  }
}

void Parameter::check() {
  Type* dt = type->deref();
  checkPointer(dt);
}

void Parameter::checkPointer(Type* dt) {
  if (dt->isPointer())
    XLAT_ERROR_NOCOL(
        "can't pass pointers across processors--\n"
        "Indicate the array length with []'s, or pass a reference",
        line);
}

void Parameter::marshallArraySizes(XStr& str, Type* dt) {
  str << "  int impl_off_" << name << ", impl_cnt_" << name << ";\n";
  str << "  impl_off_" << name << "=impl_off=CK_ALIGN(impl_off,sizeof(" << dt << "));\n";
  str << "  impl_off+=(impl_cnt_" << name << "=sizeof(" << dt << ")*(" << arrLen
      << "));\n";
}

void Parameter::marshallRegArraySizes(XStr& str) {
  Type* dt = type->deref();
  if (isArray()) marshallArraySizes(str, dt);
}

void Parameter::marshallRdmaParameters(XStr& str, bool genRdma) {
  if (isRdma() && !isDevice()) {
    Type* dt = type->deref();  // Type, without &
    if (genRdma) {
      str << "  ncpyBuffer_" << name << ".cnt=sizeof(" << dt << ")*(" << arrLen
          << ");\n";
      str << "  ncpyBuffer_" << name << ".registerMem()" << ";\n";
    } else {
      marshallArraySizes(str, dt);
    }
  }
}

void Parameter::marshallDeviceRdmaParameters(XStr& str, int& index) {
  if (isRdma() && isDevice()) {
    Type* dt = type->deref();
    str << "  deviceBuffer_" << name << ".cnt = sizeof(" << dt << ")*(" << arrLen
      << ");\n";
    str << "  device_buffers[" << index << "] = &deviceBuffer_" << name << ";\n";
    index++;
  }
}

void Parameter::pupRdma(XStr& str, bool genRdma, bool device) {
  if (isRdma()) {
    bool hostPath = !device && !isDevice();
    bool devicePath = device && isDevice();

    if (hostPath) {
      if (genRdma) {
        str << "    implP|ncpyBuffer_" << name << ";\n";
      } else {
        pupArray(str);
      }
    } else if (devicePath) {
      str << "    implP|deviceBuffer_" << name << ";\n";
    }
  }
}

void Parameter::pupArray(XStr& str) {
  str << "    implP|impl_off_" << name << ";\n";
  str << "    implP|impl_cnt_" << name << ";\n";
}

void Parameter::pup(XStr& str) {
  if (!name)
    return;
  if (isArray()) {
    pupArray(str);
  } else if (!conditional) {
    if (byReference) {
      str << "    //Have to cast away const-ness to get pup routine\n";
      str << "    implP|(typename std::remove_cv<typename std::remove_reference<" << type << ">::type>::type &)" << name << ";\n";
    } else if (!isRdma())
      str << "    implP|" << name << ";\n";
  }
}

void Parameter::marshallRdmaArrayData(XStr& str) {
  if (isRdma() && !isDevice()) {
    str << "  memcpy(impl_buf+impl_off_" << name << ","
        << "ncpyBuffer_" << name << ".ptr"
        << ",impl_cnt_" << name << ");\n";
    str << "  ncpyBuffer_" << name << ".cb.send("
        << "sizeof(CkNcpyBuffer)"
        << ","
        << "&ncpyBuffer_" << name
        << ");\n";
  }
}

void Parameter::marshallArrayData(XStr& str) {
  if (isArray())
    str << "  memcpy(impl_buf+impl_off_" << name << "," << name << ",impl_cnt_" << name
        << ");\n";
}

void Parameter::copyPtr(XStr& str) {
  if (isConditional()) {
    str << "    impl_msg->" << name << "=" << name << ";\n";
  }
}

void ParamList::beginRednWrapperUnmarshall(XStr& str, bool needsClosure) {
  if (needsClosure) {
    str << *entry->genClosureTypeNameProxyTemp << "*"
        << " genClosure = new " << *entry->genClosureTypeNameProxyTemp << "()"
        << ";\n";
  }

  if (isMarshalled()) {
    str << "  /*Unmarshall pup'd fields: ";
    print(str, 0);
    str << "*/\n";
    str << "  PUP::fromMem implP(impl_buf);\n";
    if (next != NULL && next->next == NULL) {
      // 2 argument case - special cases for an array and its length, in either order
      if (isArray() && !next->isArray()) {
        if (!needsClosure) {
          Type* dtLen = next->param->type->deref();
          str << "  PUP::detail::TemporaryObjectHolder<" << dtLen << "> "
              << next->param->name << "; " << next->param->name << ".t = "
              << "((CkReductionMsg*)impl_msg)->getLength() / sizeof("
              << param->type->deref() << ");\n";
          Type* dt = param->type->deref();
          str << "  " << dt << "* " << param->name << "; " << param->name << " = (" << dt
              << "*)impl_buf;\n";
        } else {
          str << "  genClosure->" << next->param->name << " = "
              << "((CkReductionMsg*)impl_msg)->getLength() / sizeof("
              << param->type->deref() << ");\n";
          Type* dt = param->type->deref();
          str << "  genClosure->" << param->name << " = (" << dt << "*)impl_buf;\n";
        }
      } else if (!isArray() && next->isArray()) {
        if (!needsClosure) {
          Type* dt = param->type->deref();
          str << "  PUP::detail::TemporaryObjectHolder<" << dt << "> " << param->name
              << "; " << param->name << ".t = "
              << "((CkReductionMsg*)impl_msg)->getLength() / sizeof("
              << next->param->type->deref() << ");\n";
          dt = next->param->type->deref();
          str << "  " << dt << "* " << next->param->name << "; " << next->param->name
              << " = (" << dt << "*)impl_buf;\n";
        } else {
          str << "  genClosure->" << param->name << " = "
              << "((CkReductionMsg*)impl_msg)->getLength() / sizeof("
              << next->param->type->deref() << ");\n";
          Type* dt = next->param->type->deref();
          str << "  genClosure->" << next->param->name << " = (" << dt << "*)impl_buf;\n";
        }
      } else {
        if (!needsClosure) {
          if (hasRdma()) {
            if (hasDevice()) {
              str << "  int impl_num_device_rdma_fields; implP|impl_num_device_rdma_fields;\n";
              callEach(&Parameter::beginUnmarshallRdma, str, true, true);
            } else {
              str << "  char *impl_buf_begin = impl_buf;\n";
              str << "  int impl_num_rdma_fields; implP|impl_num_rdma_fields;\n";
              str << "  int impl_num_root_node; implP|impl_num_root_node;\n";
              callEach(&Parameter::beginUnmarshallRdma, str, true, false);
            }
          }
          callEach(&Parameter::beginUnmarshall, str);
        } else {
          if (hasRdma()) {
            if (hasDevice()) {
              callEach(&Parameter::beginUnmarshallSDAGCallRdma, str, true, true);
            } else {
              str << "  char *impl_buf_begin = impl_buf;\n";
              callEach(&Parameter::beginUnmarshallSDAGCallRdma, str, true, false);
            }
          }
          callEach(&Parameter::beginUnmarshallSDAGCall, str);
        }
      }
    } else if (next == NULL && isArray()) {
      // 1 argument case - special case for a standalone array
      Type* dt = param->type->deref();
      if (!needsClosure) {
        str << "  " << dt << "* " << param->name << "; " << param->name << " = (" << dt
            << "*)impl_buf;\n";
      } else {
        str << "  genClosure->" << param->name << " = (" << dt << "*)impl_buf;\n";
      }
    } else {
      str << "  /* non two-param case */\n";
      if (!needsClosure) {
        if (hasRdma()) {
          if (hasDevice()) {
            str << "  int impl_num_device_rdma_fields; implP|impl_num_device_rdma_fields;\n";
            callEach(&Parameter::beginUnmarshallRdma, str, true, true);
          } else {
            str << "  char *impl_buf_begin = impl_buf;\n";
            str << "  int impl_num_rdma_fields; implP|impl_num_rdma_fields;\n";
            str << "  int impl_num_root_node; implP|impl_num_root_node;\n";
            callEach(&Parameter::beginUnmarshallRdma, str, true, false);
          }
        }
        callEach(&Parameter::beginUnmarshall, str);
      } else
        callEach(&Parameter::beginUnmarshallSDAGCall, str);
      str << "  impl_buf+=CK_ALIGN(implP.size(),16);\n";
      str << "  /*Unmarshall arrays:*/\n";
      if (!needsClosure)
        callEach(&Parameter::unmarshallRegArrayData, str);
      else
        callEach(&Parameter::unmarshallRegArrayDataSDAGCall, str);
    }
  }
  if (needsClosure) {
    str << "  genClosure->setRefnum(CkGetRefNum((CkReductionMsg*)impl_msg));\n";
  }
}

/** unmarshalling: unpack fields from flat buffer **/
void ParamList::beginUnmarshall(XStr& str) {
  if (isMarshalled()) {
    str << "  /*Unmarshall pup'd fields: ";
    print(str, 0);
    str << "*/\n";
    str << "  PUP::fromMem implP(impl_buf);\n";
    if (hasRdma()) {
      if (hasDevice()) {
        str << "  int impl_num_device_rdma_fields; implP|impl_num_device_rdma_fields;\n";
        callEach(&Parameter::beginUnmarshallRdma, str, true, true);
        str << "  CkDeviceBufferPost devicePost[" << entry->numRdmaDeviceParams << "];\n";
      } else {
        str << "  char *impl_buf_begin = impl_buf;\n";
        str << "  int impl_num_rdma_fields; implP|impl_num_rdma_fields;\n";
        str << "  int impl_num_root_node; implP|impl_num_root_node;\n";
        callEach(&Parameter::beginUnmarshallRdma, str, true, false);
        if (hasRecvRdma()) {
          str << "  CkNcpyBufferPost ncpyPost[" << entry->numRdmaRecvParams << "];\n";
          str << "  int numPostAsync=0;\n";
          for (int index = 0; index < entry->numRdmaRecvParams; index++) {
            str << "  initPostStruct(ncpyPost, " << index << " );\n";
          }
        }
      }
    }
    callEach(&Parameter::beginUnmarshall, str);
    str << "  impl_buf+=CK_ALIGN(implP.size(),16);\n";
    str << "  /*Unmarshall arrays:*/\n";
    callEach(&Parameter::unmarshallRegArrayData, str);
  }
}

void ParamList::copyFromPostedPtrs(XStr& str, bool isSDAGGen) {
  int count = 0;
  callEach(&Parameter::copyFromPostedPtrs, str, true, isSDAGGen, false, count);
}

void ParamList::setupPostedPtrs(XStr& str, bool isSDAGGen) {
  int count = 0;
  callEach(&Parameter::setupPostedPtrs, str, true, isSDAGGen, false, count);
}

void ParamList::storePostedRdmaPtrs(XStr& str, bool isSDAGGen) {
  if (hasDevice()) {
    int count = 0; // Used to keep track of indices
    callEach(&Parameter::storePostedRdmaPtrs, str, true, isSDAGGen, true, count);
  } else {
    callEach(&Parameter::storePostedRdmaPtrs, str, true, isSDAGGen, false);
  }
}

void ParamList::extractPostedPtrs(XStr& str, bool isSDAGGen) {
    int count = 0;
    callEach(&Parameter::extractPostedPtrs, str, true, isSDAGGen, false, count);
}

void ParamList::printPeerAckInfo(XStr& str, bool isSDAGGen) {
    int count = 0;
    callEach(&Parameter::printPeerAckInfo, str, true, isSDAGGen, false, count);
}

void Parameter::printPeerAckInfo(XStr& str, bool genRdma, bool isSDAGGen, bool device, int &count) {
  Type* dt = type->deref();  // Type, without &
  if (isRdma() && count == 0) {
    str << "    NcpyEmInfo *ncpyEmInfo = (";
    if(isSDAGGen)
      str << "genClosure->";
    str << "ncpyBuffer_" << name << ".ncpyEmInfo);\n";
    str << "    NcpyBcastRecvPeerAckInfo *peerAckInfo = (";
    if(isSDAGGen)
      str << "genClosure->";
    str << "ncpyBuffer_" << name << ".ncpyEmInfo->peerAckInfo);\n";

    str << "    std::vector<int> *tagArray = ";
    if(isSDAGGen)
      str << "genClosure->";
    str << "ncpyBuffer_" << name << ".ncpyEmInfo->tagArray;\n";
    count++;
  }
}

void Parameter::extractPostedPtrs(XStr& str, bool genRdma, bool isSDAGGen, bool device, int &count) {
  Type* dt = type->deref();  // Type, without &
  if (isRdma()) {
    str << "      ";
    if(isSDAGGen)
      str << "genClosure->" << arrLen;
    else
      str << arrLen << ".t";
    str << " = extractStoredBuffer(";
    if(isSDAGGen)
      str << "genClosure->";
    str << "ncpyBuffer_" << name << ".ncpyEmInfo->tagArray, env, myIndex,";
    if(isSDAGGen)
      str << "genClosure->num_rdma_fields,";
    else
      str << "impl_num_rdma_fields, ";
    str << count++ << ", (void *&)(";
    if(isSDAGGen)
      str << "genClosure->";
    str << "ncpyBuffer_" << name << ".ptr));\n";
  }
}


void Parameter::setupPostedPtrs(XStr& str, bool genRdma, bool isSDAGGen, bool device, int &count) {
  Type* dt = type->deref();  // Type, without &

  if (isRdma()) {
    bool hostPath = !device && !isDevice();
    bool devicePath = device && isDevice();

    if (hostPath) {
      if (genRdma) {
        str << "      setPostStruct(ncpyPost, " << count++ << ",";
        if(isSDAGGen) str << "genClosure->";
        str << "ncpyBuffer_" << name << ",";
        str << "myIndex);\n";
      } else {
        str << "  if(ncpyPost[" << count << "].postAsync == false) { \n";
        // Error checking if posted buffer is larger than the source buffer
        str << "    if(impl_cnt_" << name << " < " ;
        if(isSDAGGen)
           str << " sizeof(" << dt << ") * genClosure->"<< arrLen << ")\n";
        else
          str << " sizeof(" << dt << ") * "<< arrLen << ".t)\n";

        str << "      CkAbort(\"Size of the posted buffer > Size of the source buffer \");\n";

        // memcpy the pointer into the user passed buffer
        str << "    memcpy(" << "ncpyBuffer_" << name << "_ptr,";
        if(isSDAGGen)
          str << "genClosure->";
        str << name << ",";

        if(isSDAGGen)
          str << " sizeof(" << dt << ") * genClosure->"<< arrLen << ");\n";
        else
          str << " sizeof(" << dt << ") * "<< arrLen << ".t);\n";
        str << "  } else {\n";
        str << "    ncpyPost[" << count << "].srcBuffer =";
        if(isSDAGGen)
          str << "genClosure->";
        str << name << ";\n";
        str << "    ncpyPost[" << count++  << "].srcSize = impl_cnt_" << name << ";\n";
        str << "  }\n";
      }
    }
  }
}



void Parameter::copyFromPostedPtrs(XStr& str, bool genRdma, bool isSDAGGen, bool device, int &count) {
  Type* dt = type->deref();  // Type, without &

  if (isRdma()) {
    bool hostPath = !device && !isDevice();
    bool devicePath = device && isDevice();

    if (hostPath) {
      if (genRdma) {
        //TODO: Uncomment this later
        str << "      if(ncpyPost[" << count << "].postAsync == false ) {\n";
        // Error checking if posted buffer is larger than the source buffer
        str << "        if( ";
        if(isSDAGGen)
          str << "genClosure->";
        str << "ncpyBuffer_" << name << ".cnt < " ;
        if(isSDAGGen)
           str << " sizeof(" << dt << ") * genClosure->"<< arrLen << ")\n";
        else
          str << " sizeof(" << dt << ") * "<< arrLen << ".t)\n";
        str << "          CkAbort(\"Size of the posted buffer > Size of the source buffer \");\n";

        str << "        memcpy(" << "ncpyBuffer_" << name << "_ptr,";
        if(isSDAGGen)
          str << "genClosure->";
        str << "ncpyBuffer_" << name << ".ptr,";
        if(isSDAGGen)
          str << " sizeof(" << dt << ") * genClosure->"<< arrLen << ");\n";
        else
          str << " sizeof(" << dt << ") * "<< arrLen << ".t);\n";
        str << "        ncpyPost[" << count << "].ncpyEmInfo->counter++;\n";
        str << "        setPosted(tagArray, env, myIndex, ";

        if(isSDAGGen)
          str << " genClosure->num_rdma_fields,";
        else
          str << " impl_num_rdma_fields,";
        str << count++ << ");\n";
        str << "      }\n";
      } else {
        str << "  if(ncpyPost[" << count << "].postAsync == false) { \n";
        // Error checking if posted buffer is larger than the source buffer
        str << "    if(impl_cnt_" << name << " < " ;
        if(isSDAGGen)
           str << " sizeof(" << dt << ") * genClosure->"<< arrLen << ")\n";
        else
          str << " sizeof(" << dt << ") * "<< arrLen << ".t)\n";

        str << "      CkAbort(\"Size of the posted buffer > Size of the source buffer \");\n";

        // memcpy the pointer into the user passed buffer
        str << "    memcpy(" << "ncpyBuffer_" << name << "_ptr,";
        if(isSDAGGen)
          str << "genClosure->";
        str << name << ",";

        if(isSDAGGen)
          str << " sizeof(" << dt << ") * genClosure->"<< arrLen << ");\n";
        else
          str << " sizeof(" << dt << ") * "<< arrLen << ".t);\n";
        str << "  } else {\n";
        str << "    ncpyPost[" << count << "].srcBuffer =";
        if(isSDAGGen)
          str << "genClosure->";
        str << name << ";\n";
        str << "    ncpyPost[" << count++  << "].srcSize = impl_cnt_" << name << ";\n";
        str << "  }\n";
      }
    }
  }
}



void Parameter::storePostedRdmaPtrs(XStr& str, bool genRdma, bool isSDAGGen, bool device, int &count) {
  Type* dt = type->deref();  // Type, without &

  if (isRdma()) {
    bool hostPath = !device && !isDevice();
    bool devicePath = device && isDevice();

    if (hostPath) {
      if (genRdma) {
        str << "    buffPtrs[" << count << "] = (void *)" << "ncpyBuffer_";
        str << name << "_ptr;\n";
        if(isSDAGGen)
          str << "    buffSizes[" << count++ << "] = sizeof(" << dt << ") * genClosure->"<< arrLen << ";\n";
        else
          str << "    buffSizes[" << count++ << "] = sizeof(" << dt << ") * " << arrLen << ".t;\n";
      }
    } else if (devicePath) {
      str << "  if(CMI_IS_ZC_DEVICE(env)) {\n";
      str << "    buffPtrs[" << count << "] = (void *)" << "deviceBuffer_";
      str << name << "_ptr;\n";
      if(isSDAGGen)
        str << "    buffSizes[" << count++ << "] = sizeof(" << dt << ") * genClosure->"<< arrLen << ";\n";
      else
        str << "    buffSizes[" << count++ << "] = sizeof(" << dt << ") * " << arrLen << ".t;\n";
      str <<  "  }\n";
    }
  }
}

void Parameter::beginUnmarshallArray(XStr& str) {
  str << "  int impl_off_" << name << ", impl_cnt_" << name << ";\n";
  str << "  implP|impl_off_" << name << ";\n";
  str << "  implP|impl_cnt_" << name << ";\n";

  if(isRecvRdma()) {
    Type* dt = type->deref();                          // Type, without &
    str << "  " << dt << " *ncpyBuffer_" << name <<"_ptr = NULL;\n";
  }
}

// First pass: unpack pup'd entries
void Parameter::beginUnmarshallRdma(XStr& str, bool genRdma, bool device) {
  Type* dt = type->deref(); // Type, without &

  if (isRdma()) {
    bool hostPath = !device && !isDevice();
    bool devicePath = device && isDevice();

    if (hostPath) {
      if (genRdma) {
        str << "  CkNcpyBuffer ncpyBuffer_" << name << ";\n";
        str << "  implP|ncpyBuffer_" << name << ";\n";

        str << "  " << dt << " *ncpyBuffer_" << name << "_ptr = ";
        str << "(" << dt << " *)" << " ncpyBuffer_" << name << ".ptr;\n";
      } else {
        beginUnmarshallArray(str);
      }
    } else if (devicePath) {
      str << "  CkDeviceBuffer deviceBuffer_" << name << ";\n";
      str << "  implP|deviceBuffer_" << name << ";\n";

      str << "  " << dt << " *deviceBuffer_" << name << "_ptr = ";
      str << "(" << dt << " *)" << " deviceBuffer_" << name << ".ptr;\n";
    }
  }
}

void Parameter::beginUnmarshall(XStr& str) {  // First pass: unpack pup'd entries
  Type* dt = type->deref();                   // Type, without &
  if (isArray())
    beginUnmarshallArray(str);
  else if (isConditional())
    str << "  " << dt << " *" << name << "=impl_msg_typed->" << name << ";\n";
  else if (!isRdma())
    str << "  PUP::detail::TemporaryObjectHolder<" << dt << "> " << name << ";\n"
        << "  "
        << "implP|" << name << ";\n";
}

void Parameter::beginUnmarshallSDAGCallRdma(XStr& str, bool genRdma, bool device) {
  if (isRdma()) {
    bool hostPath = !device && !isDevice();
    bool devicePath = device && isDevice();

    if (hostPath) {
      if (genRdma) {
        if (isFirstRdma()) {
          str << "  implP|genClosure->num_rdma_fields;\n";
          str << "  implP|genClosure->num_root_node;\n";
        }
        str << "  implP|genClosure->ncpyBuffer_" << name << ";\n";
        if (isRecvRdma()) {
          Type* dt = type->deref();
          str << "  " << dt << " *ncpyBuffer_" << name << "_ptr = ";
          str << "(" << dt << " *)" << " (genClosure->ncpyBuffer_" << name << ").ptr;\n";
        }
      } else {
        beginUnmarshallArray(str);
      }
    } else if (devicePath) {
      if (isFirstDeviceRdma()) {
        str << "  implP|genClosure->num_device_rdma_fields;\n";
      }
      str << "  implP|genClosure->deviceBuffer_" << name << ";\n";
      Type* dt = type->deref();
      str << "  " << dt << " *deviceBuffer_" << name << "_ptr = ";
      str << "(" << dt << " *)" << " (genClosure->deviceBuffer_" << name << ").ptr;\n";
    }
  }
}

void Parameter::beginUnmarshallSDAGCall(XStr& str) {
  Type* dt = type->deref();
  if (isArray()) {
    beginUnmarshallArray(str);
  } else if (isRdma()) {
    // unmarshalled before regular parameters
  } else {
    str << "  implP|" << (podType ? "" : "*") << "genClosure->" << name << ";\n";
  }
}

/** unmarshalling: unpack fields from flat buffer **/
void ParamList::beginUnmarshallSDAGCall(XStr& str, bool usesImplBuf) {
  bool hasArray = false;
  for (ParamList* pl = this; pl != NULL; pl = pl->next) {
    hasArray = hasArray || pl->param->isArray();
  }

  if (isMarshalled()) {
    str << "  PUP::fromMem implP(impl_buf);\n";
    str << "  " << *entry->genClosureTypeNameProxyTemp << "*"
        << " genClosure = new " << *entry->genClosureTypeNameProxyTemp << "()"
        << ";\n";
    if (hasRdma()) {
      if (hasDevice()) {
        str << "  CkDeviceBufferPost devicePost[" << entry->numRdmaDeviceParams << "];\n";
        callEach(&Parameter::beginUnmarshallSDAGCallRdma, str, true, true);
      } else {
        if (hasRecvRdma()) {
          str << "  CkNcpyBufferPost ncpyPost[" << entry->numRdmaRecvParams << "];\n";
          str << "    int numPostAsync=0;\n";
          for (int index = 0; index < entry->numRdmaRecvParams; index++) {
            str << "  initPostStruct(ncpyPost, " << index << " );\n";
          }
        }
        str << "  char *impl_buf_begin = impl_buf;\n";
        callEach(&Parameter::beginUnmarshallSDAGCallRdma, str, true, false);
      }
    }
    callEach(&Parameter::beginUnmarshallSDAGCall, str);
    str << "  impl_buf+=CK_ALIGN(implP.size(),16);\n";
    callEach(&Parameter::unmarshallRegArrayDataSDAGCall, str);
    if (hasArray || hasRdma()) {
      if (!usesImplBuf) {
        str << "  genClosure->_impl_marshall = impl_msg_typed;\n";
        str << "  CkReferenceMsg(genClosure->_impl_marshall);\n";
      }
    }
  }
}
void ParamList::beginUnmarshallSDAG(XStr& str) {
  if (isMarshalled()) {
    str << "          PUP::fromMem implP(impl_buf);\n";
    if (hasRdma()) {
      if (hasDevice()) {
        callEach(&Parameter::adjustUnmarshalledRdmaPtrsSDAG, str, true, true);
        str << "  implP|num_device_rdma_fields;\n";
        callEach(&Parameter::beginUnmarshallRdma, str, true, true);
      } else {
        /* Before migration of the closure structure, Rdmawrapper pointers
         * store the offset to the actual buffer from the msgBuf
         * After migration, the Rdmawrapper pointer needs to be adjusted
         * to point to the msgBuf + offset. As the actual buffer is within
         * the message, the adjusting should happen after the message is
         * unpacked. (see code in Entry::genClosure)
         */
        callEach(&Parameter::adjustUnmarshalledRdmaPtrsSDAG, str, true, false);
        str << "  implP|num_rdma_fields;\n";
        str << "  implP|num_root_node;\n";
        callEach(&Parameter::beginUnmarshallRdma, str, true, false);
      }
    }
    callEach(&Parameter::beginUnmarshall, str);
    str << "          impl_buf+=CK_ALIGN(implP.size(),16);\n";
    // If there's no rdma support, unmarshall as a regular array
    callEach(&Parameter::unmarshallRegArrayDataSDAG, str);
  }
}

void Parameter::unmarshallRegArrayDataSDAG(XStr& str) {
  if (isArray()) {
    Type* dt = type->deref();  // Type, without &
    str << "          " << name << " = (" << dt << " *)(impl_buf+impl_off_" << name
        << ");\n";
  }
}

void Parameter::adjustUnmarshalledRdmaPtrsSDAG(XStr& str, bool genRdma, bool device) {
  (void)genRdma; // Unused, needed to use this as a rdmaheterofn_t (otherwise same signature as rdmafn_t)
  if (isRdma()) {
    if (!device && !isDevice()) {
      str << "  ncpyBuffer_" << name << ".ptr = ";
      str << "(void *)(impl_buf + (size_t)(ncpyBuffer_" << name << ".ptr));\n";
    } else if (device && isDevice()) {
      str << "  deviceBuffer_" << name << ".ptr = ";
      str << "(void *)(impl_buf + (size_t)(deviceBuffer_" << name << ".ptr));\n";
    }
  }
}

void Parameter::unmarshallRdmaArrayDataSDAG(XStr& str) {
  if (isRdma() && !isDevice()) {
    Type* dt = type->deref();  // Type, without &
    str << "          " << name << " = (" << dt << " *)(impl_buf+impl_off_" << name
        << ");\n";
  }
}

void Parameter::unmarshallRegArrayDataSDAGCall(XStr& str) {
  if (isArray()) {
    Type* dt = type->deref();  // Type, without &
    str << "  genClosure->" << name << " = (" << dt << " *)(impl_buf+impl_off_" << name
        << ");\n";
  }
}

void Parameter::unmarshallRdmaArrayDataSDAGCall(XStr& str) {
  if (isRdma() && !isDevice()) {
    Type* dt = type->deref();  // Type, without &
    str << "  genClosure->" << name << " = (" << dt << " *)(impl_buf+impl_off_" << name
        << ");\n";
  }
}

void ParamList::unmarshallSDAGCall(XStr& str, int isFirst) {
  if (isFirst && isMessage())
    str << "(" << param->type << ")impl_msg";
  else if (!isVoid()) {
    str << "genClosure->";
    str << param->getName();
    if (next) {
      str << ", ";
      next->unmarshallSDAGCall(str, 0);
    }
  }
}

void Parameter::unmarshallArrayData(XStr& str) {
  Type* dt = type->deref();  // Type, without &
  str << "  " << dt << " *" << name << "=(" << dt << " *)(impl_buf+impl_off_" << name
      << ");\n";
}

void Parameter::unmarshallRdmaArrayData(XStr& str, bool genRegArray) {
  if (isRdma() && genRegArray && !isDevice()) unmarshallArrayData(str);
}

void Parameter::unmarshallRegArrayData(
    XStr& str) {  // Second pass: unpack pointed-to arrays
  if (isArray()) unmarshallArrayData(str);
}

void ParamList::unmarshall(XStr& str, bool isInline, bool isFirst, bool isRdmaPost)  // Pass-by-value
{
  if (isFirst && isMessage())
    str << "(" << param->type << ")impl_msg";
  else if (!isVoid()) {
    bool isSDAGGen = entry->sdagCon || entry->isWhenEntry;
    if (param->isRdma()) {
      if (param->isDevice()) {
        str << "deviceBuffer_" << param->getName() << "_ptr";
      } else {
        str << "ncpyBuffer_" << param->getName() << "_ptr";
      }
    } else if (param->isArray() || isInline) {
      if(isRdmaPost && isSDAGGen) str << "genClosure->";
      str << param->getName();
    } else {
      if(isRdmaPost) {
        if(isSDAGGen)
          str << "genClosure->" << param->getName();
        else
          str << param->getName() << ".t ";
      }
      else
        str << "std::move(" << param->getName() << ".t)";
    }

    if (next) {
      str << ", ";
      next->unmarshall(str, isInline, false, isRdmaPost);
    }
  }
}

// Do forwarding for rvalue references, used for inline and local entry methods
void ParamList::unmarshallForward(XStr& str,
                                  bool isInline,
                                  bool isFirst,
                                  bool isRdmaPost,
                                  int fwdNum)
{
  if (!isInline)
    unmarshall(str, isInline, isFirst, isRdmaPost);
  if (isReference()) {
    str << "std::forward<Fwd" << fwdNum++ << ">(" << param->getName() << ")";
    if (next) {
      str << ", ";
      next->unmarshallForward(str, isInline, false, isRdmaPost, fwdNum);
    }
  } else {
    unmarshall(str, isInline, isFirst, isRdmaPost);
  }
}

void ParamList::unmarshallAddress(XStr& str,
                                  int isFirst)  // Pass-by-reference, for Fortran
{
  if (isFirst && isMessage())
    str << "(" << param->type << ")impl_msg";
  else if (!isVoid()) {
    //@TODO : Case for RDMA
    if (param->isArray())
      str << param->getName();  // Arrays are already pointers
    else
      str << "& " << param->getName() << ".t";  // Take address of simple types and structs
    if (next) {
      str << ", ";
      next->unmarshallAddress(str, 0);
    }
  }
}

void ParamList::pupAllValues(XStr& str) {
  if (isMarshalled()) callEach(&Parameter::pupAllValues, str);
}

void Parameter::pupAllValues(XStr& str) {
  str << "  if (implDestP.hasComments()) implDestP.comment(\"" << name << "\");\n";
  if (isArray()) {
    str << "  implDestP.synchronize(PUP::sync_begin_array);\n"
           "  for (int impl_i=0;impl_i*(sizeof(*"
        << name << "))<impl_cnt_" << name
        << ";impl_i++) {\n"
           "    implDestP.synchronize(PUP::sync_item);\n"
           "    implDestP|"
        << name
        << "[impl_i];\n"
           "  }\n"
           "  implDestP.synchronize(PUP::sync_end_array);\n";
  } else if (isRdma()) {
    if (isDevice()) {
      str << "  implDestP|deviceBuffer_" << name << ";\n";
    } else {
      str << "  implDestP|ncpyBuffer_" << name << ";\n";
    }
  } else /* not an array */ {
    if (isConditional())
      str << "  pup_pointer(&implDestP, (void**)&" << name << ");\n";
    else
      str << "  implDestP|" << name << ";\n";
  }
}

void ParamList::endUnmarshall(XStr&) {
  /* Marshalled entry points now have the "SNOKEEP" attribute...
  if (isMarshalled()) {
          str<<"  delete (CkMarshallMsg *)impl_msg;\n";
  }
  */
}

void ParamList::printMsg(XStr& str) {
  ParamList* pl;
  param->printMsg(str);
  pl = next;
  while (pl != NULL) {
    str << ", ";
    pl->param->printMsg(str);
    pl = pl->next;
  }
}

void Parameter::printMsg(XStr& str) {
  type->print(str);
  if (given_name != 0) str << given_name;
}

int Parameter::isMessage(void) const { return type->isMessage(); }
int Parameter::isVoid(void) const { return type->isVoid(); }
int Parameter::isCkArgMsgPtr(void) const { return type->isCkArgMsgPtr(); }
int Parameter::isCkMigMsgPtr(void) const { return type->isCkMigMsgPtr(); }
int Parameter::isArray(void) const { return (arrLen != NULL && !isRdma()); }
int Parameter::isConditional(void) const { return conditional; }
int Parameter::isRdma(void) const { return (rdma != CMK_REG_NO_ZC_MSG); }
int Parameter::isSendRdma(void) const { return (rdma == CMK_ZC_P2P_SEND_MSG); }
int Parameter::isRecvRdma(void) const { return (rdma == CMK_ZC_P2P_RECV_MSG); }
int Parameter::isDevice(void) const { return (rdma == CMK_ZC_DEVICE_MSG); }
int Parameter::getRdma(void) const { return rdma; }
int Parameter::isFirstRdma(void) const { return firstRdma; }
int Parameter::isFirstDeviceRdma(void) const { return firstDeviceRdma; }

int Parameter::operator==(const Parameter& parm) const { return *type == *parm.type; }

void Parameter::setConditional(int c) {
  conditional = c;
  if (c) byReference = false;
}

void Parameter::setRdma(int r) { rdma = r; }

void Parameter::setFirstRdma(bool fr) { firstRdma = fr; }

void Parameter::setFirstDeviceRdma(bool fr) { firstDeviceRdma = fr; }

void Parameter::setAccelBufferType(int abt) {
  accelBufferType = ((abt < ACCEL_BUFFER_TYPE_MIN || abt > ACCEL_BUFFER_TYPE_MAX)
                         ? (ACCEL_BUFFER_TYPE_UNKNOWN)
                         : (abt));
}

int Parameter::getAccelBufferType() { return accelBufferType; }
void Parameter::setAccelInstName(XStr* ain) { accelInstName = ain; }
XStr* Parameter::getAccelInstName(void) { return accelInstName; }

ParamList::ParamList(Parameter* Nparam, ParamList* Nnext) : param(Nparam), next(Nnext) {
  manyPointers = false;
  if (next != NULL && (param->isMessage() || next->isMessage())) {
    manyPointers = true;
  }
}

int ParamList::isNamed(void) const { return param->type->isNamed(); }
int ParamList::isBuiltin(void) const { return param->type->isBuiltin(); }
int ParamList::isMessage(void) const { return (next == NULL) && param->isMessage(); }
int ParamList::hasRdma(void) { return orEach(&Parameter::isRdma); }
int ParamList::hasSendRdma(void) { return orEach(&Parameter::isSendRdma); }
int ParamList::hasRecvRdma(void) { return orEach(&Parameter::isRecvRdma); }
int ParamList::hasDevice(void) { return orEach(&Parameter::isDevice); }
int ParamList::isRdma(void) { return param->isRdma(); }
int ParamList::isDevice(void) { return param->isDevice(); }
int ParamList::getRdma(void) { return param->getRdma(); }
int ParamList::isFirstRdma(void) { return param->isFirstRdma(); }
int ParamList::isFirstDeviceRdma(void) { return param->isFirstDeviceRdma(); }
int ParamList::isRecvRdma(void) { return param->isRecvRdma(); }
const char* ParamList::getArrayLen(void) const { return param->getArrayLen(); }
int ParamList::isArray(void) const { return param->isArray(); }
int ParamList::isReference(void) const {
  return param->type->isReference() || param->byReference;
}
int ParamList::declaredReference(void) const {
  return param->type->isReference() || param->declaredReference;
}
bool ParamList::isConst(void) const { return param->type->isConst() || param->byConst; }
int ParamList::isVoid(void) const { return (next == NULL) && param->isVoid(); }
int ParamList::isPointer(void) const { return param->type->isPointer(); }
const char* ParamList::getGivenName(void) const { return param->getGivenName(); }
void ParamList::setGivenName(const char* s) { param->setGivenName(s); }
const char* ParamList::getName(void) const { return param->getName(); }
int ParamList::isMarshalled(void) const { return !isVoid() && !isMessage(); }
int ParamList::isCkArgMsgPtr(void) const {
  return (next == NULL) && param->isCkArgMsgPtr();
}
int ParamList::isCkMigMsgPtr(void) const {
  return (next == NULL) && param->isCkMigMsgPtr();
}
int ParamList::getNumStars(void) const { return param->type->getNumStars(); }
const char* ParamList::getBaseName(void) { return param->type->getBaseName(); }
void ParamList::genMsgProxyName(XStr& str) { param->type->genMsgProxyName(str); }

void ParamList::checkParamList() {
  if (manyPointers) {
    XLAT_ERROR_NOCOL(
        "multiple pointers passed to a non-local entry method\n"
        "You may pass only a single pointer to it, which should point to a message.",
        param->line);
  }
}

int ParamList::operator==(ParamList& plist) {
  if (!(*param == *(plist.param))) return 0;
  if (!next && !plist.next) return 1;
  if (!next || !plist.next) return 0;
  return *next == *plist.next;
}

}  // namespace xi
