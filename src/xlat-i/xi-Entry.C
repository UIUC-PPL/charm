#include "xi-Chare.h"
#include "xi-Entry.h"
#include "xi-Parameter.h"
#include "xi-SdagCollection.h"
#include "xi-Value.h"

#include "sdag/constructs/When.h"

#include <list>
using std::list;

namespace xi {

extern int fortranMode;
extern int internalMode;
const char* python_doc;

XStr Entry::proxyName(void) { return container->proxyName(); }
XStr Entry::indexName(void) { return container->indexName(); }

void Entry::print(XStr& str) {
  if (isThreaded()) str << "threaded ";
  if (isSync()) str << "sync ";
  if (retType) {
    retType->print(str);
    str << " ";
  }
  str << name << "(";
  if (param) param->print(str);
  str << ")";
  if (stacksize) {
    str << " stacksize = ";
    stacksize->print(str);
  }
  str << ";\n";
}

void Entry::check() {
  if (!external) {
    if (isConstructor() && retType && !retType->isVoid())
      XLAT_ERROR_NOCOL("constructors cannot return a value", first_line_);

    if (!isConstructor() && !retType)
      XLAT_ERROR_NOCOL(
          "non-constructor entry methods must specify a return type (probably void)",
          first_line_);

    if (isConstructor() && (isSync() || isIget())) {
      XLAT_ERROR_NOCOL("constructors cannot have the 'sync' attribute", first_line_);
      removeAttribute(SSYNC);
    }

    if (param->isCkArgMsgPtr() && (!isConstructor() || !container->isMainChare()))
      XLAT_ERROR_NOCOL("CkArgMsg can only be used in mainchare's constructor",
                       first_line_);

    if (isExclusive() && isConstructor())
      XLAT_ERROR_NOCOL("constructors cannot be 'exclusive'", first_line_);

    if (isImmediate() && !container->isNodeGroup())
      XLAT_ERROR_NOCOL("[immediate] entry methods are only allowed on 'nodegroup' types",
                       first_line_);

    if (isLocal() && (container->isChare() || container->isNodeGroup()))
      XLAT_ERROR_NOCOL(
          "[local] entry methods are only allowed on 'array' and 'group' types",
          first_line_);
  }

  if (!isThreaded() && stacksize)
    XLAT_ERROR_NOCOL(
        "the 'stacksize' attribute is only applicable to methods declared 'threaded'",
        first_line_);

  if (retType && !isSync() && !isIget() && !isLocal() && !retType->isVoid())
    XLAT_ERROR_NOCOL(
        "non-void return type in a non-sync/non-local entry method\n"
        "To return non-void, you need to declare the method as [sync], which means it "
        "has blocking semantics,"
        " or [local].",
        first_line_);

  if (!isLocal() && param) param->checkParamList();

  if (isPython() && !container->isPython())
    XLAT_ERROR_NOCOL("python entry method declared in non-python chare", first_line_);

  // check the parameter passed to the function, it must be only an integer
  if (isPython() && (!param || param->next || !param->param->getType()->isBuiltin() ||
                     !((BuiltinType*)param->param->getType())->isInt()))
    XLAT_ERROR_NOCOL(
        "python entry methods take only one parameter, which is of type 'int'",
        first_line_);

  if (isExclusive() && !container->isNodeGroup())
    XLAT_ERROR_NOCOL("only nodegroup methods can be 'exclusive'", first_line_);

  // (?) Check that every when statement has a corresponding entry method
  // declaration. Otherwise, print all candidates tested (as in clang, gcc.)
  if (isSdag()) {
    list<CEntry*> whenEntryList;
    sdagCon->generateEntryList(whenEntryList, NULL);
    // containsWhenConstruct is used to prepend sdag entry method names with "_sdag_fnc_" when it contains one or more when clauses
    containsWhenConstruct = !whenEntryList.empty();

    for (list<CEntry*>::iterator en = whenEntryList.begin(); en != whenEntryList.end();
         ++en) {
      container->lookforCEntry(*en);
      (*en)->check();
    }
  }

  if (isTramTarget()) {
    if (param && (/*!param->isMarshalled() ||*/ param->isVoid() || param->next != NULL))
      XLAT_ERROR_NOCOL(
          "'aggregate' entry methods must be parameter-marshalled "
          "and take a single argument",
          first_line_);

    if (!external && !((container->isGroup() && !container->isNodeGroup()) || container->isArray()))
      XLAT_ERROR_NOCOL(
          "'aggregate' entry methods can only be used in regular groups and chare arrays",
          first_line_);
  }

  if (isWhenIdle()) {
    if (!retType || strcmp(retType->getBaseName(), "bool")) {
      XLAT_ERROR_NOCOL(
        "whenidle functions must return 'bool'",
        first_line_);
    }

    if (!param || param->next || strcmp(param->param->getType()->getBaseName(), "double")) {
      XLAT_ERROR_NOCOL(
        "whenidle functions must accept a single parameter of type 'double'",
        first_line_);
    }
  }
}

void Entry::lookforCEntry(CEntry* centry) {
  // compare name
  if (strcmp(name, *centry->entry) != 0) return;

  centry->addCandidate(this);

  // compare param
  if (param && !centry->paramlist) return;
  if (!param && centry->paramlist) return;
  if (param && !(*param == *centry->paramlist)) return;

  isWhenEntry = 1;
  centry->decl_entry = this;
}

Entry::Entry(int l, Attribute* a, Type* r, const char* n, ParamList* p, Value* sz,
             SdagConstruct* sc, const char* e, int fl, int ll)
    : attribs(a),
      retType(r),
      stacksize(sz),
      sdagCon(sc),
      name((char*)n),
      targs(0),
      intExpr(e),
      param(p),
      genClosureTypeName(0),
      genClosureTypeNameProxy(0),
      genClosureTypeNameProxyTemp(0),
      entryPtr(0),
      first_line_(fl),
      last_line_(ll),
      numRdmaSendParams(0),
      numRdmaRecvParams(0),
      numRdmaDeviceParams(0) {
  line = l;
  container = NULL;
  entryCount = -1;
  isWhenEntry = 0;
  containsWhenConstruct = false;
  if (param && param->isMarshalled() && !isThreaded()) addAttribute(SNOKEEP);
  if (isPython()) pythonDoc = python_doc;
  ParamList* plist = p;
  while (plist != NULL) {
    plist->entry = this;
    if (plist->param) {
      plist->param->entry = this;
      if (plist->param->getRdma() == CMK_ZC_P2P_SEND_MSG)
        numRdmaSendParams++; // increment send 'rdma' param count
      if (plist->param->getRdma() == CMK_ZC_P2P_RECV_MSG)
        numRdmaRecvParams++; // increment recv 'rdma' param count
      if (plist->param->isDevice())
        numRdmaDeviceParams++; // increment device 'rdma' param count
    }
    plist = plist->next;
  }
  if (getAttribute(SWHENIDLE)) {
    addAttribute(SLOCAL);
  }
}

void Entry::setChare(Chare* c) {
  Member::setChare(c);
  // mainchare constructor parameter is not allowed
  /* ****************** REMOVED 10/8/2002 ************************
  if (isConstructor()&&container->isMainChare() && param != NULL)
    if (!param->isCkArgMsgPtr())
     die("MainChare Constructor doesn't allow parameter!", line);
  Removed old treatment for CkArgMsg to allow argc, argv or void
  constructors for mainchares.
  * **************************************************************/
  if (isConstructor() && param->isVoid()) {
    if (container->isMainChare()) {
      // Main chare always magically takes CkArgMsg
      Type* t = new PtrType(new NamedType("CkArgMsg"));
      param = new ParamList(new Parameter(line, t));
      std::cerr << "Charmxi> " << line
                << ": Deprecation warning: mainchare constructors should explicitly take "
                   "CkArgMsg* if that's how they're implemented.\n";
    }
    if (container->isArray()) {
      Array* a = dynamic_cast<Array*>(c);
      a->hasVoidConstructor = true;
    }
  }

  entryCount = c->nextEntry();

  // Make a special "callmarshall" method, for communication optimizations to use:
  hasCallMarshall = param->isMarshalled() && !isThreaded() && !isSync() &&
                    !isExclusive() && !fortranMode;
  if (isSdag()) {
    container->setSdag(1);

    list<CEntry*> whenEntryList;
    sdagCon->generateEntryList(whenEntryList, NULL);

    for (list<CEntry*>::iterator i = whenEntryList.begin(); i != whenEntryList.end();
         ++i) {
      container->lookforCEntry(*i);
    }
  }

  if (isWhenIdle()) {
    container->setWhenIdle(1);
  }
}

void Entry::preprocessSDAG() {
  if (isSdag() || isWhenEntry) {
    if (container->isNodeGroup()) {
      addAttribute(SLOCKED); // Make the method [exclusive] to preclude races on SDAG
                             // control structures
    }
  }
}

// "parameterType *msg" or "void".
// Suitable for use as the only parameter
XStr Entry::paramType(int withDefaultVals, int withEO, int useConst, int rValue) {
  XStr str;
  param->print(str, withDefaultVals, useConst, rValue);
  if (withEO) str << eo(withDefaultVals, !param->isVoid());
  return str;
}

// "parameterType *msg," if there is a non-void parameter,
// else empty.  Suitable for use with another parameter following.
XStr Entry::paramComma(int withDefaultVals, int withEO) {
  XStr str;
  if (!param->isVoid()) {
    str << paramType(withDefaultVals, withEO);
    str << ", ";
  }
  return str;
}
XStr Entry::eo(int withDefaultVals, int priorComma) {
  XStr str;
  // Add CkEntryOptions for all non message params
  // for param->isMarshalled() and param->isVoid()
  if (!param->isMessage()) {
    if (priorComma) str << ", ";
    str << "const CkEntryOptions *impl_e_opts";
    if (withDefaultVals) str << "=NULL";
  }
  return str;
}

void Entry::collectSdagCode(SdagCollection* sc) {
  if (isSdag()) {
    sc->addNode(this);
  }
}

XStr Entry::marshallMsg(void) {
  XStr ret;
  XStr epName = epStr();
  param->marshall(ret, epName);
  return ret;
}

XStr Entry::epStr(bool isForRedn, bool templateCall) {
  XStr str;
  if (isForRedn) str << "redn_wrapper_";
  str << name << "_";

  if (param->isMessage()) {
    str << param->getBaseName();
    str.replace(':', '_');
  } else if (param->isVoid())
    str << "void";
  else
    str << "marshall" << entryCount;

  if (tspec && templateCall) {
    str << "<";
    tspec->genShort(str);
    str << ">";
  }

  return str;
}

XStr Entry::epIdx(int fromProxy, bool isForRedn) {
  XStr str;
  if (fromProxy) {
    str << indexName() << "::";
    // If the chare is also templated, then we must avoid a parsing ambiguity
    if (tspec) str << "template ";
  }
  str << "idx_" << epStr(isForRedn, true) << "()";
  return str;
}

XStr Entry::epRegFn(int fromProxy, bool isForRedn) {
  XStr str;
  if (fromProxy) str << indexName() << "::";
  str << "reg_" << epStr(isForRedn, true) << "()";
  return str;
}

XStr Entry::chareIdx(int fromProxy) {
  XStr str;
  if (fromProxy) str << indexName() << "::";
  str << "__idx";
  return str;
}

XStr Entry::syncPreCall(void) {
  XStr str;
  if (retType->isVoid())
    str << "  void *impl_msg_typed_ret = ";
  else if (retType->isMessage())
    str << "  " << retType << " impl_msg_typed_ret = (" << retType << ")";
  else
    str << "  CkMarshallMsg *impl_msg_typed_ret = (CkMarshallMsg *)";
  return str;
}

XStr Entry::syncPostCall(void) {
  XStr str;
  if (retType->isVoid())
    str << "  CkFreeSysMsg(impl_msg_typed_ret); \n";
  else if (!retType->isMessage()) {
    str << "  char *impl_buf_ret=impl_msg_typed_ret->msgBuf; \n";
    str << "  PUP::fromMem implPS(impl_buf_ret); \n";
    str << "  " << retType << " retval; implPS|retval; \n";
    str << "  CkFreeMsg(impl_msg_typed_ret); \n";
    str << "  return retval; \n";
  } else {
    str << "  return impl_msg_typed_ret;\n";
  }
  return str;
}

/*************************** Chare Entry Points ******************************/

void Entry::genChareDecl(XStr& str) {
  if (isConstructor()) {
    genChareStaticConstructorDecl(str);
  } else {
    // entry method declaration
    str << "    " << generateTemplateSpec(tspec) << "\n"
        << "    " << retType << " " << name << "(" << paramType(1, 1) << ");\n";
  }
}

void Entry::genChareDefs(XStr& str) {
  if (isConstructor()) {
    genChareStaticConstructorDefs(str);
  } else {
    XStr params;
    params << epIdx() << ", impl_msg, &ckGetChareID()";
    // entry method definition
    XStr retStr;
    retStr << retType;
    str << makeDecl(retStr, 1) << "::" << name << "(" << paramType(0, 1) << ")\n";
    str << "{\n  ckCheck();\n";
    str << marshallMsg();
    if (isSync()) {
      str << syncPreCall() << "CkRemoteCall(" << params << ");\n";
      str << syncPostCall();
    } else {  // Regular, non-sync message
      str << "  if (ckIsDelegated()) {\n";
      if (param->hasRdma()) {
        str << "  CkAbort(\"Entry methods with nocopy parameters not supported when "
               "called with delegation managers\");\n";
      } else {
        str << "    int destPE=CkChareMsgPrep(" << params << ");\n";
        str << "    if (destPE!=-1) ckDelegatedTo()->ChareSend(ckDelegatedPtr(),"
            << params << ",destPE);\n";
      }
      str << "  } else {\n";
      XStr opts;
      opts << ",0";
      if (isSkipscheduler()) opts << "+CK_MSG_EXPEDITED";
      if (isInline()) opts << "+CK_MSG_INLINE";
      str << "    CkSendMsg(" << params << opts << ");\n";
      str << "  }\n";
    }
    str << "}\n";
  }
}

void Entry::genChareStaticConstructorDecl(XStr& str) {
  str << "    static CkChareID ckNew(" << paramComma(1) << "int onPE=CK_PE_ANY" << eo(1)
      << ");\n";
  str << "    static void ckNew(" << paramComma(1)
      << "CkChareID* pcid, int onPE=CK_PE_ANY" << eo(1) << ");\n";
}

void Entry::genChareStaticConstructorDefs(XStr& str) {
  str << makeDecl("CkChareID", 1) << "::ckNew(" << paramComma(0) << "int impl_onPE"
      << eo(0) << ")\n";
  str << "{\n";
  str << marshallMsg();
  str << "  CkChareID impl_ret;\n";
  str << "  CkCreateChare(" << chareIdx() << ", " << epIdx()
      << ", impl_msg, &impl_ret, impl_onPE);\n";
  str << "  return impl_ret;\n";
  str << "}\n";

  str << makeDecl("void", 1) << "::ckNew(" << paramComma(0)
      << "CkChareID* pcid, int impl_onPE" << eo(0) << ")\n";
  str << "{\n";
  str << marshallMsg();
  str << "  CkCreateChare(" << chareIdx() << ", " << epIdx()
      << ", impl_msg, pcid, impl_onPE);\n";
  str << "}\n";
}

/***************************** Array Entry Points **************************/

void Entry::genArrayDecl(XStr& str) {
  if (isConstructor()) {
    str << "    " << generateTemplateSpec(tspec) << "\n";
    genArrayStaticConstructorDecl(str);
  } else {
    if ((isSync() || isLocal()) && !container->isForElement())
      return;  // No sync broadcast
    if (isIget()) {
      str << "    " << generateTemplateSpec(tspec) << "\n";
      str << "    "
          << "CkFutureID"
          << " " << name << "(" << paramType(1, 1) << ") ;\n";  // no const
    } else if ((isLocal() || isInline()) && container->isForElement()) {
      XStr fwdStr;
      int fwdNum = 1;
      ParamList *pl = param;
      while (pl) {
        Parameter *p = pl->param;
        if (!p->isRdma() && p->arrLen == NULL && !p->conditional && p->byReference) {
          if (fwdNum > 1)
            fwdStr << ", ";
          fwdStr << "typename Fwd" << fwdNum++ << " = " << p->type;
        }
        pl = pl->next;
      }
      const bool doFwd = fwdNum > 1;
      if (tspec || doFwd) {
        str << "    template <";
        if (tspec) {
          tspec->genLong(str);
          if (doFwd)
            str << ", ";
        }
        if (doFwd)
          str << fwdStr;
        str << ">\n";
      }
      str << "    " << retType << " " << name << "(" << paramType(1, 1, 0, 1) << ") ;\n";
    } else if (isLocal()) {
      str << "    " << generateTemplateSpec(tspec) << "\n";
      str << "    " << retType << " " << name << "(" << paramType(1, 1, 0) << ") ;\n";
    } else if (isTramTarget() && container->isForElement()) {
      str << "    " << generateTemplateSpec(tspec) << "\n";
      str << "    " << retType << " " << name << "(" << paramType(0, 1) << ") = delete;\n";
      str << "    " << retType << " " << name << "(" << paramType(1, 0) << ") ;\n";
    } else {
      str << "    " << generateTemplateSpec(tspec) << "\n";
      str << "    " << retType << " " << name << "(" << paramType(1, 1)
          << ") ;\n";  // no const
    }
  }
}

void Entry::genArrayDefs(XStr& str) {
  if (isIget() && !container->isForElement()) return;

  if (isConstructor())
    genArrayStaticConstructorDefs(str);
  else {  // Define array entry method
    const char* ifNot = "CkArray_IfNotThere_buffer";
    if (isCreateHere()) ifNot = "CkArray_IfNotThere_createhere";
    if (isCreateHome()) ifNot = "CkArray_IfNotThere_createhome";

    if ((isSync() || isLocal()) && !container->isForElement())
      return;  // No sync broadcast

    XStr retStr;
    retStr << retType;
    if (isIget())
      str << makeDecl("CkFutureID ", 1) << "::" << name << "(" << paramType(0, 1)
          << ") \n";  // no const
    else if ((isLocal() || isInline()) && container->isForElement()) {
      XStr fwdStr;
      int fwdNum = 1;
      ParamList *pl = param;
      while (pl) {
        Parameter *p = pl->param;
        if (!p->isRdma() && p->arrLen == NULL && !p->conditional && p->byReference) {
          if (fwdNum > 1)
            fwdStr << ", ";
          fwdStr << "typename Fwd" << fwdNum++; // << " = " << p->type;
        }
        pl = pl->next;
      }
      str << makeDecl(retStr, 1, false, fwdStr) << "::" << name << "(" << paramType(0, 1, 0, 1) << ") \n";
    } else if (isLocal())
      str << makeDecl(retStr, 1) << "::" << name << "(" << paramType(0, 1, 0) << ") \n";
    else
      str << makeDecl(retStr, 1) << "::" << name << "(" << paramType(0, 1)
          << ") \n";  // no const
    str << "{\n";
    // regular broadcast and section broadcast for an entry method with rdma
    str << "  ckCheck();\n";
    XStr inlineCall;
    if (!isNoTrace())
      inlineCall
          << "    _TRACE_BEGIN_EXECUTE_DETAILED(0,ForArrayEltMsg,(" << epIdx()
          << "),CkMyPe(), 0, ((CkArrayIndex&)ckGetIndex()).getProjectionID(), obj);\n";
    if (isAppWork()) inlineCall << "    _TRACE_BEGIN_APPWORK();\n";
    inlineCall << "#if CMK_LBDB_ON\n";
    if (isInline())
    {
      inlineCall << "    const auto id = obj->ckGetID().getElementID();\n"
                 << "    const CkArrayIndex* idx = &ckGetIndex();\n";
      param->size(inlineCall); // Puts size of parameters in bytes into impl_off
      inlineCall << "    impl_off += sizeof(envelope);\n"
                 << "    ckLocMgr()->recordSend(idx, id, impl_off);\n";
    }
    inlineCall << "#endif\n";
    inlineCall << "#if CMK_CHARMDEBUG\n"
                  "    CpdBeforeEp("
               << epIdx()
               << ", obj, NULL);\n"
                  "#endif\n";
    inlineCall << "    CkCallstackPush(obj);\n";
    inlineCall << "    ";
    if (!retType->isVoid()) inlineCall << retType << " retValue = ";
    inlineCall << "obj->" << (tspec ? "template " : "") << name;
    if (tspec) {
      inlineCall << "<";
      tspec->genShort(inlineCall);
      inlineCall << ">";
    }
    inlineCall << "(";
    param->unmarshallForward(inlineCall, true);
    inlineCall << ");\n";
    inlineCall << "    CkCallstackPop(obj);\n";
    inlineCall << "#if CMK_CHARMDEBUG\n"
                  "    CpdAfterEp("
               << epIdx()
               << ");\n"
                  "#endif\n";
    if (isAppWork()) inlineCall << "    _TRACE_END_APPWORK();\n";
    if (!isNoTrace()) inlineCall << "    _TRACE_END_EXECUTE();\n";
    if (!retType->isVoid()) {
      inlineCall << "    return retValue;\n";
    } else {
      inlineCall << "    return;\n";
    }

    XStr prepareMsg;
    prepareMsg << marshallMsg();
    prepareMsg << "  UsrToEnv(impl_msg)->setMsgtype(ForArrayEltMsg);\n";
    prepareMsg << "  CkArrayMessage *impl_amsg=(CkArrayMessage *)impl_msg;\n";
    prepareMsg << "  impl_amsg->array_setIfNotThere(" << ifNot << ");\n";

    if (!isLocal()) {
      if (isInline() && container->isForElement()) {
        str << "  " << container->baseName() << " *obj = ckLocal();\n";
        str << "  if (obj) {\n" << inlineCall << "  }\n";
      }
      str << prepareMsg;
    } else {
      str << "  " << container->baseName() << " *obj = ckLocal();\n";
      str << "#if CMK_ERROR_CHECKING\n";
      str << "  if (obj==NULL) CkAbort(\"Trying to call a LOCAL entry method on a "
             "non-local element\");\n";
      str << "#endif\n";
      str << inlineCall;
    }
    if (isIget()) {
      str << "  CkFutureID f=CkCreateAttachedFutureSend(impl_amsg," << epIdx()
          << ",ckGetArrayID(),ckGetIndex(),&CProxyElement_ArrayBase::ckSendWrapper);"
          << "\n";
    }

    if (isSync()) {
      str << syncPreCall() << "ckSendSync(impl_amsg, " << epIdx() << ");\n";
      str << syncPostCall();
    } else if (!isLocal()) {
      XStr opts;
      opts << ",0";
      if (isSkipscheduler()) opts << "+CK_MSG_EXPEDITED";
      if (isInline()) opts << "+CK_MSG_INLINE";
      if (!isIget()) {
        if (container->isForElement() || container->isForSection()) {
          str << "  ckSend(impl_amsg, " << epIdx() << opts << ");\n";
        } else
          str << "  ckBroadcast(impl_amsg, " << epIdx() << opts << ");\n";
      }
    }
    if (isIget()) {
      str << "  return f;\n";
    }
    str << "}\n";

    if (!tspec && !container->isTemplated() && !isIget() && (isLocal() || isInline()) && container->isForElement()) {
      XStr fwdStr;
      int fwdNum = 1;
      ParamList *pl = param;
      while (pl) {
        Parameter *p = pl->param;
        if (!p->isRdma() && p->arrLen == NULL && !p->conditional && p->byReference) {
          if (fwdNum > 1)
            fwdStr << ", ";
          ++fwdNum;
          if (p->byConst) fwdStr << "const ";
          fwdStr << p->type << " &";
        }
        pl = pl->next;
      }
      const bool doFwd = fwdNum > 1;
      if (doFwd) {
        str << "// explicit instantiation for compatibility\n";
        str << "template " << makeDecl(retStr, 1) << "::" << name << "<" << fwdStr << ">(" << paramType(0, 1, 0) << ");\n";
      }
    }
  }
}

void Entry::genArrayStaticConstructorDecl(XStr& str) {
  if (!container->isArray())
    die("Internal error - array declarations called for on non-array Chare type");

  if (container->getForWhom() == forIndividual)
    str <<  // Element insertion routine
        "    void insert(" << paramComma(1, 0) << "int onPE=-1" << eo(1) << ");";
  else if (container->getForWhom() == forAll) {
    // With options to specify size (including potentially empty, covering the
    // param->isVoid() case)
    str << "    static CkArrayID ckNew(" << paramComma(1, 0)
        << "const CkArrayOptions &opts = CkArrayOptions()" << eo(1) << ");\n";
    str << "    static void      ckNew(" << paramComma(1, 0)
        << "const CkArrayOptions &opts, CkCallback _ck_array_creation_cb" << eo(1)
        << ");\n";

    XStr dim = ((Array*)container)->dim();
    if (dim == (const char*)"1D") {
      str << "    static CkArrayID ckNew(" << paramComma(1, 0) << "const int s1" << eo(1)
          << ");\n";
      str << "    static void ckNew(" << paramComma(1, 0)
          << "const int s1, CkCallback _ck_array_creation_cb" << eo(1) << ");\n";
    } else if (dim == (const char*)"2D") {
      str << "    static CkArrayID ckNew(" << paramComma(1, 0)
          << "const int s1, const int s2" << eo(1) << ");\n";
      str << "    static void ckNew(" << paramComma(1, 0)
          << "const int s1, const int s2, CkCallback _ck_array_creation_cb" << eo(1)
          << ");\n";
    } else if (dim == (const char*)"3D") {
      str << "    static CkArrayID ckNew(" << paramComma(1, 0)
          << "const int s1, const int s2, const int s3" << eo(1) << ");\n";
      str << "    static void ckNew(" << paramComma(1, 0)
          << "const int s1, const int s2, const int s3, CkCallback _ck_array_creation_cb"
          << eo(1) << ");\n";
      /*} else if (dim==(const char*)"4D") {
        str<<"    static CkArrayID ckNew("<<paramComma(1,0)<<"const short s1, const short
      s2, const short s3, const short s4"<<eo(1)<<");\n"; } else if (dim==(const
      char*)"5D") { str<<"    static CkArrayID ckNew("<<paramComma(1,0)<<"const short s1,
      const short s2, const short s3, const short s4, const short s5"<<eo(1)<<");\n"; }
      else if (dim==(const char*)"6D") { str<<"    static CkArrayID
      ckNew("<<paramComma(1,0)<<"const short s1, const short s2, const short s3, const
      short s4, const short s5, const short s6"<<eo(1)<<");\n"; */
    }
  } else if (container->getForWhom() == forSection) {
  }
}

void Entry::genArrayStaticConstructorDefs(XStr& str) {
  if (!container->isArray())
    die("Internal error - array definitions called for on non-array Chare type");

  if (container->getForWhom() == forIndividual)
    str << makeDecl("void", 1) << "::insert(" << paramComma(0, 0) << "int onPE" << eo(0)
        << ")\n"
           "{ \n "
        << marshallMsg()
        << "   UsrToEnv(impl_msg)->setMsgtype(ArrayEltInitMsg);\n"
           "   ckInsert((CkArrayMessage *)impl_msg,"
        << epIdx() << ",onPE);\n}\n";
  else if (container->getForWhom() == forAll) {
    XStr syncPrototype, asyncPrototype, head, syncTail, asyncTail;
    syncPrototype << makeDecl("CkArrayID", 1) << "::ckNew";
    asyncPrototype << makeDecl("void", 1) << "::ckNew";

    head << "{\n" << marshallMsg();

    syncTail << "  UsrToEnv(impl_msg)->setMsgtype(ArrayEltInitMsg);\n"
             << "  CkArrayID gId = ckCreateArray((CkArrayMessage *)impl_msg, " << epIdx()
             << ", opts);\n";

    genTramInstantiation(syncTail);
    syncTail << "  return gId;\n}\n";

    asyncTail << "  UsrToEnv(impl_msg)->setMsgtype(ArrayEltInitMsg);\n"
              << "  CkSendAsyncCreateArray(" << epIdx()
              << ", _ck_array_creation_cb, opts, impl_msg);\n"
              << "}\n";

    str << syncPrototype << "(" << paramComma(0) << "const CkArrayOptions &opts" << eo(0)
        << ")\n"
        << head << syncTail;
    str << asyncPrototype << "(" << paramComma(0)
        << "const CkArrayOptions &opts, CkCallback _ck_array_creation_cb" << eo(0)
        << ")\n"
        << head << asyncTail;

    XStr dim = ((Array*)container)->dim();
    XStr sizeParams, sizeArgs;
    bool emit = true;

    if (dim == (const char*)"1D") {
      sizeParams << "const int s1";
      sizeArgs << "s1";
    } else if (dim == (const char*)"2D") {
      sizeParams << "const int s1, const int s2";
      sizeArgs << "s1, s2";
    } else if (dim == (const char*)"3D") {
      sizeParams << "const int s1, const int s2, const int s3";
      sizeArgs << "s1, s2, s3";
    }
#if 0
    else if (dim==(const char*)"4D") {
      sizeParams << "const short s1, const short s2, const short s3, const short s4";
      sizeArgs << "s1, s2, s3, s4";
    } else if (dim==(const char*)"5D") {
      sizeParams << "const short s1, const short s2, const short s3, const short s4, "
                 << "const short s5";
      sizeArgs << "s1, s2, s3, s4, s5";
    } else if (dim==(const char*)"6D") {
      sizeParams << "const short s1, const short s2, const short s3, const short s4, "
                 << "const short s5, const short s6";
      sizeArgs << "s1, s2, s3, s4, s5, s6";
    }
#endif
    else {
      emit = false;
    }

    if (emit) {
      str << syncPrototype << "(" << paramComma(0) << sizeParams << eo(0) << ")\n"
          << head << "  CkArrayOptions opts(" << sizeArgs << ");\n"
          << syncTail;
      str << asyncPrototype << "(" << paramComma(0) << sizeParams
          << ", CkCallback _ck_array_creation_cb" << eo(0) << ")\n"
          << head << "  CkArrayOptions opts(" << sizeArgs << ");\n"
          << asyncTail;
    }
  }
}

/******************************** Group Entry Points *********************************/

void Entry::genGroupDecl(XStr& str) {
  if (isConstructor()) {
    str << "    " << generateTemplateSpec(tspec) << "\n";
    genGroupStaticConstructorDecl(str);
  } else {
    if ((isSync() || isLocal()) && !container->isForElement())
      return;  // No sync broadcast
    str << "    " << generateTemplateSpec(tspec) << "\n";
    if (isLocal()) {
      str << "    " << retType << " " << name << "(" << paramType(1, 1, 0) << ");\n";
    } else if (isTramTarget() && container->isForElement()) {
      str << "    " << retType << " " << name << "(" << paramType(0, 1) << ") = delete;\n";
      str << "    " << retType << " " << name << "(" << paramType(1, 0) << ");\n";
    } else {
      str << "    " << retType << " " << name << "(" << paramType(1, 1) << ");\n";
    }
    // entry method on multiple PEs declaration
    if (!container->isForElement() && !container->isForSection() && !isSync() &&
        !isLocal() && !container->isNodeGroup()) {
      str << "    " << generateTemplateSpec(tspec) << "\n";
      str << "    " << retType << " " << name << "(" << paramComma(0, 0)
          << "int npes, int *pes" << eo(1) << ");\n";
      str << "    " << generateTemplateSpec(tspec) << "\n";
      str << "    " << retType << " " << name << "(" << paramComma(0, 0)
          << "CmiGroup &grp" << eo(1) << ");\n";
    }
  }
}

void Entry::genGroupDefs(XStr& str) {
  if(isConstructor()) {
    genGroupStaticConstructorDefs(str);
    return;
  }

  // Selects between NodeGroup and Group
  char* node = (char*)(container->isNodeGroup() ? "Node" : "");

  int forElement = container->isForElement();
  XStr params;
  params << epIdx() << ", impl_msg";
  XStr paramg;
  paramg << epIdx() << ", impl_msg, ckGetGroupID()";
  XStr parampg;
  parampg << epIdx() << ", impl_msg, ckGetGroupPe(), ckGetGroupID()";
  // append options parameter
  XStr opts;
  opts << ",0";
  if (isImmediate()) opts << "+CK_MSG_IMMEDIATE";
  if (isInline()) opts << "+CK_MSG_INLINE";
  if (isSkipscheduler()) opts << "+CK_MSG_EXPEDITED";

  if ((isSync() || isLocal()) && !container->isForElement())
    return;  // No sync broadcast

  XStr retStr;
  retStr << retType;
  XStr msgTypeStr;
  if (isLocal())
    msgTypeStr << paramType(0, 1, 0);
  else
    msgTypeStr << paramType(0, 1);
  str << makeDecl(retStr, 1) << "::" << name << "(" << msgTypeStr << ")\n";
  str << "{\n";
  // regular broadcast and section broadcast for an entry method with rdma
    str << "  ckCheck();\n";
    if (!isLocal()) str << marshallMsg();

    if (isLocal()) {
      XStr unmarshallStr;
      param->unmarshall(unmarshallStr, true);
      str << "  " << container->baseName() << " *obj = ckLocalBranch();\n";
      str << "  CkAssert(obj);\n";
      if (!isNoTrace())
        // Create a dummy envelope to represent the "message send" to the local/inline method
        // so that Projections can trace the method back to its caller
        str << "  envelope env;\n"
            << "  env.setMsgtype(ForBocMsg);\n"
            << "  env.setTotalsize(0);\n"
            << "  _TRACE_CREATION_DETAILED(&env, " << epIdx() << ");\n"
            << "  _TRACE_CREATION_DONE(1);\n"
            << "  _TRACE_BEGIN_EXECUTE_DETAILED(CpvAccess(curPeEvent),ForBocMsg,(" << epIdx()
            << "),CkMyPe(),0,NULL, NULL);\n";
      if (isAppWork()) str << " _TRACE_BEGIN_APPWORK();\n";
      str << "#if CMK_CHARMDEBUG\n"
             "  CpdBeforeEp("
          << epIdx()
          << ", obj, NULL);\n"
             "#endif\n  ";
      str << "CkCallstackPush(obj);\n  ";
      if (!retType->isVoid()) str << retType << " retValue = ";
      str << "obj->" << name << "(" << unmarshallStr << ");\n  ";
      str << "CkCallstackPop(obj);\n";
      str << "#if CMK_CHARMDEBUG\n"
             "  CpdAfterEp("
          << epIdx()
          << ");\n"
             "#endif\n";
      if (isAppWork()) str << " _TRACE_END_APPWORK();\n";
      if (!isNoTrace()) str << "  _TRACE_END_EXECUTE();\n";
      if (!retType->isVoid()) str << "  return retValue;\n";
    } else if (isSync()) {
      str << syncPreCall() << "CkRemote" << node << "BranchCall(" << paramg
          << ", ckGetGroupPe());\n";
      str << syncPostCall();
    } else {             // Non-sync, non-local entry method
      if (forElement) {  // Send
        str << "  if (ckIsDelegated()) {\n";
        if (param->hasRdma()) {
          str << "    CkAbort(\"Entry methods with nocopy parameters not supported "
                 "when called with delegation managers\");\n";
        } else {
          str << "     Ck" << node << "GroupMsgPrep(" << paramg << ");\n";
          str << "     ckDelegatedTo()->" << node << "GroupSend(ckDelegatedPtr(),"
              << parampg << ");\n";
        }
        str << "  } else {\n";
        str << "    CkSendMsg" << node << "Branch"
            << "(" << parampg << opts << ");\n";
        str << "  }\n";
      } else if (container->isForSection()) {  // Multicast
        str << "  if (ckIsDelegated()) {\n";
        str << "     ckDelegatedTo()->" << node << "GroupSectionSend(ckDelegatedPtr(),"
            << params << ", ckGetNumSections(), ckGetSectionIDs());\n";
        str << "  } else {\n";
        str << "    void *impl_msg_tmp;\n";
        str << "    for (int i=0; i<ckGetNumSections(); ++i) {\n";
        str << "       impl_msg_tmp= (i<ckGetNumSections()-1) ? CkCopyMsg((void **) "
               "&impl_msg):impl_msg;\n";
        str << "       CkSendMsg" << node << "BranchMulti(" << epIdx()
            << ", impl_msg_tmp, ckGetGroupIDn(i), ckGetNumElements(i), ckGetElements(i)"
            << opts << ");\n";
        str << "    }\n";
        str << "  }\n";
      } else {  // Broadcast
        str << "  if (ckIsDelegated()) {\n";
        str << "     Ck" << node << "GroupMsgPrep(" << paramg << ");\n";
        str << "     ckDelegatedTo()->" << node << "GroupBroadcast(ckDelegatedPtr(),"
            << paramg << ");\n";
        str << "  } else CkBroadcastMsg" << node << "Branch(" << paramg << opts
            << ");\n";
      }
    }
  str << "}\n";

  // entry method on multiple PEs declaration
  if (!forElement && !container->isForSection() && !isSync() && !isLocal() &&
      !container->isNodeGroup()) {
    str << "" << makeDecl(retStr, 1) << "::" << name << "(" << paramComma(0, 0)
        << "int npes, int *pes" << eo(0) << ") {\n";
    str << marshallMsg();
    str << "  CkSendMsg" << node << "BranchMulti(" << paramg << ", npes, pes" << opts
        << ");\n";
    str << "}\n";
    str << "" << makeDecl(retStr, 1) << "::" << name << "(" << paramComma(0, 0)
        << "CmiGroup &grp" << eo(0) << ") {\n";
    str << marshallMsg();
    str << "  CkSendMsg" << node << "BranchGroup(" << paramg << ", grp" << opts
        << ");\n";
    str << "}\n";
  }
}

XStr Entry::aggregatorIndexType() {
  XStr indexType;
  if (container->isGroup()) {
    indexType << "int";
  } else if (container->isArray()) {
    XStr dim, arrayIndexType;
    dim << ((Array*)container)->dim();
     indexType << "CkArrayIndex";
  }
  return indexType;
}

XStr Entry::dataItemType() {
  XStr itemType;
  if (container->isGroup()) {
    itemType << param->param->type;
  } else if (container->isArray()) {
    itemType << param->param->type;
  }
  return itemType;
}

XStr Entry::aggregatorType() {
  XStr groupType;
  if (container->isGroup()) {
    groupType << "GroupMeshStreamer<" << param->param->type << ", "
              << container->baseName() << ", SimpleMeshRouter"
              << ", " << container->indexName() << "::_callmarshall_" << epStr() << ">";
  } else if (container->isArray()) {
    groupType << "ArrayMeshStreamer<" << param->param->type << ", "
              << container->baseName() << ", "
              << "SimpleMeshRouter, " << container->indexName() << "::_callmarshall_"
              << epStr() << ">";
  }
  return groupType;
}

XStr Entry::aggregatorGlobalType(XStr& scope) {
  XStr groupType;
  if (container->isGroup()) {
    groupType << "GroupMeshStreamer<" << param->param->type << ", " << scope
              << container->baseName() << ", SimpleMeshRouter"
              << ", " << scope << container->indexName() << "::_callmarshall_" << epStr()
              << ">";
  } else if (container->isArray()) {
    groupType << "ArrayMeshStreamer<" << param->param->type << ", "
              << scope << container->baseName() << ", "
              << "SimpleMeshRouter, " << scope << container->indexName()
              << "::_callmarshall_" << epStr() << ">";
  }
  return groupType;
}

XStr Entry::aggregatorName() {
  XStr aggregatorName;
  aggregatorName << epStr() << "TramAggregator";
  return aggregatorName;
}

const char *tramArgBufferSize = "bufferSize";
const int tramDefaultBufferSize = 16384;

const char *tramArgNumDimensions = "numDimensions";
const int tramDefaultNumDimensions = 2;

const char *tramArgThresholdFractionNumerator = "thresholdNumer";
const int tramDefaultThresholdFractionNumerator = 1;

const char *tramArgThresholdFractionDenominator = "thresholdDenom";
const int tramDefaultThresholdFractionDenominator = 2;

const char *tramArgCutoffFractionNumerator = "cutoffNumer";
const int tramDefaultCutoffFractionNumerator = 1;

const char *tramArgCutoffFractionDenominator = "cutoffDenom";
const int tramDefaultCutoffFractionDenominator = 2;

const char *tramMaxItemsBuffered = "maxItems";
const int tramDefaultMaxItemsBuffered = 1000;

void Entry::genTramTypes() {
  Attribute* aggregate = getAttribute(SAGGREGATE);

  if (aggregate) {
    XStr typeString, nameString, itemTypeString;
    typeString << aggregatorType();
    nameString << aggregatorName();
    itemTypeString << dataItemType();
    int bufferSize = aggregate->getArgument(tramArgBufferSize, tramDefaultBufferSize);
    int numDimensions = aggregate->getArgument(tramArgNumDimensions, tramDefaultNumDimensions);
    int thresholdFractionNumerator = aggregate->getArgument(tramArgThresholdFractionNumerator, tramDefaultThresholdFractionNumerator);
    int thresholdFractionDenominator = aggregate->getArgument(tramArgThresholdFractionDenominator, tramDefaultThresholdFractionDenominator);
    int cutoffFractionNumerator = aggregate->getArgument(tramArgCutoffFractionNumerator, tramDefaultCutoffFractionNumerator);
    int cutoffFractionDenominator = aggregate->getArgument(tramArgCutoffFractionDenominator, tramDefaultCutoffFractionDenominator);
    int maxItemsBuffered = aggregate->getArgument(tramMaxItemsBuffered, tramDefaultMaxItemsBuffered);

    Attribute::Argument *arg = aggregate->getArgs();
    while (arg) {
      if (strcmp(arg->name, tramArgBufferSize) && strcmp(arg->name, tramArgNumDimensions)
          && strcmp(arg->name, tramArgThresholdFractionNumerator)
          && strcmp(arg->name, tramArgThresholdFractionDenominator)
          && strcmp(arg->name, tramArgCutoffFractionNumerator)
          && strcmp(arg->name, tramArgCutoffFractionDenominator)
          && strcmp(arg->name, tramMaxItemsBuffered)) {
        XLAT_ERROR_NOCOL("unsupported argument to aggregate attribute",
                         first_line_);
      }
      arg = arg->next;
    }

    if (numDimensions != 1 && numDimensions != 2) {
      XLAT_ERROR_NOCOL("aggregate currently only supports generating 1D or 2D streamers",
                       first_line_);
      numDimensions = tramDefaultNumDimensions;
    }

    container->tramInstances.push_back(TramInfo(typeString.get_string(),
          nameString.get_string(), itemTypeString.get_string(), numDimensions,
          bufferSize, maxItemsBuffered, thresholdFractionNumerator,
          thresholdFractionDenominator, cutoffFractionNumerator,
          cutoffFractionDenominator));
    tramInstanceIndex = container->tramInstances.size();
  }
}

void Entry::genTramDefs(XStr& str) {
  XStr retStr;
  retStr << retType;
  XStr msgTypeStr;

  if (isLocal())
    msgTypeStr << paramType(0, 0, 0);
  else
    msgTypeStr << paramType(0, 0);
  str << makeDecl(retStr, 1) << "::" << name << "(" << msgTypeStr << ") {\n"
      << "  if (" << aggregatorName() << " == NULL) {\n";

  if (container->isGroup()) {
    str << "    CkGroupID gId = ckGetGroupID();\n";
  } else if (container->isArray()) {
    str << "    CkArray *aMgr = ckLocalBranch();\n"
        << "    CkGroupID gId = aMgr->getGroupID();\n";
  }

  str << "    CkGroupID tramGid;\n"
      << "    tramGid.idx = gId.idx + " << tramInstanceIndex << ";\n"
      << "    " << aggregatorName() << " = (" << aggregatorType() << "*)"
      << " CkLocalBranch(tramGid);\n  }\n";

  if (container->isGroup()) {
    str << "  " << aggregatorName() << "->insertData(" << param->param->name << ", "
        << "ckGetGroupPe());\n}\n";
  } else if (container->isArray()) {
    XStr dim;
    dim << ((Array*)container)->dim();
    str << "  const CkArrayIndex &myIndex = ckGetIndex();\n"
        << "  " << aggregatorName() << "->insertData<" << (isInline() ? "true" : "false")
        << ">(" << param->param->name;
    str << ", " << "myIndex);\n}\n";
  }
}

void Entry::genTramInstantiation(XStr& str) {
  if (!container->tramInstances.empty()) {
    for (int i = 0; i < container->tramInstances.size(); i++) {
      int bufferSize = container->tramInstances[i].bufferSize;
      int maxItemsBuffered = container->tramInstances[i].maxItemsBuffered;
      int numDimensions = container->tramInstances[i].numDimensions;
      int thresholdFractionNum = container->tramInstances[i].thresholdFractionNumerator;
      int thresholdFractionDen = container->tramInstances[i].thresholdFractionDenominator;
      int cutoffFractionNum = container->tramInstances[i].cutoffFractionNumerator;
      int cutoffFractionDen = container->tramInstances[i].cutoffFractionDenominator;

      str << "  {\n    const int nDims = " << numDimensions << ";\n";

      if (numDimensions == 1) {
        str << "    int dims[] = { CkNumPes() };\n";
      } else { // Has to be 2 by the previous "assertion"
        str << "    int pesPerNode = CkMyNodeSize();\n"
            << "    if (pesPerNode == 1) {\n"
            << "      pesPerNode = CmiNumCores();\n"
            << "    }\n"
            << "    int dims[nDims];\n"
            << "    dims[0] = CkNumPes() / pesPerNode;\n"
            << "    dims[1] = pesPerNode;\n"
            << "    if (dims[0] * dims[1] != CkNumPes()) {\n"
            << "      dims[0] = CkNumPes();\n"
            << "      dims[1] = 1;\n"
            << "    }\n";
      }

      str << "    int tramBufferSize = " << bufferSize <<";\n"
          << "    int maxItemsBuffered = " << maxItemsBuffered <<";\n"
          << "    int thresholdFractionNum = " << thresholdFractionNum <<";\n"
          << "    int thresholdFractionDen = " << thresholdFractionDen <<";\n"
          << "    int cutoffFractionNum = " << cutoffFractionNum <<";\n"
          << "    int cutoffFractionDen = " << cutoffFractionDen <<";\n"
          << "    int itemsPerBuffer = tramBufferSize / sizeof("
          << container->tramInstances[i].itemType.c_str() << ");\n"
          << "    if (itemsPerBuffer == 0) {\n"
          << "      itemsPerBuffer = 1;\n"
          << "    }\n"
          << "    CProxy_" << container->tramInstances[i].type.c_str()
          << " tramProxy =\n"
          << "    CProxy_" << container->tramInstances[i].type.c_str()
          << "::ckNew(nDims, dims, gId, tramBufferSize, false, 0.01, "
          << "maxItemsBuffered, thresholdFractionNum, thresholdFractionDen, "
          << "cutoffFractionNum, cutoffFractionDen);\n"
          << "    tramProxy.enablePeriodicFlushing();\n"
          << "  }\n";
    }
  }
}

XStr Entry::tramBaseType() {
  XStr baseTypeString;
  baseTypeString << "MeshStreamer<" << dataItemType() << ", SimpleMeshRouter>";

  return baseTypeString;
}

void Entry::genTramRegs(XStr& str) {
  if (isTramTarget()) {
    XStr messageTypeString;
    messageTypeString << "MeshStreamerMessageV";

    XStr baseTypeString = tramBaseType();

    NamedType messageType(messageTypeString.get_string());
    Message helper(-1, &messageType);
    helper.genReg(str);

    str << "\n  /* REG: group " << aggregatorType() << ": IrrGroup;\n  */\n"
        << "  CkIndex_" << aggregatorType() << "::__register(\"" << aggregatorType()
        << "\", sizeof(" << aggregatorType() << "));\n"
        << "  /* REG: group " << baseTypeString << ": IrrGroup;\n  */\n"
        << "  CkIndex_" << baseTypeString << "::__register(\"" << baseTypeString
        << "\", sizeof(" << baseTypeString << "));\n";
  }
}

void Entry::genTramPups(XStr& scope, XStr& decls, XStr& defs) {
  if (isTramTarget()) {
    XStr aggregatorTypeString = aggregatorGlobalType(scope);
    container->genRecursivePup(aggregatorTypeString, "template <>\n", decls, defs);
  }
}

void Entry::genGroupStaticConstructorDecl(XStr& str) {
  if (container->isForElement()) return;
  if (container->isForSection()) return;

  str << "    static CkGroupID ckNew(" << paramType(1, 1) << ");\n";
  if (!param->isVoid()) {
    str << "    " << container->proxyName(0) << "(" << paramType(1, 1) << ");\n";
  }
}

void Entry::genGroupStaticConstructorDefs(XStr& str) {
  if (container->isForElement()) return;
  if (container->isForSection()) return;

  // Selects between NodeGroup and Group
  char* node = (char*)(container->isNodeGroup() ? "Node" : "");
  str << makeDecl("CkGroupID", 1) << "::ckNew(" << paramType(0, 1) << ")\n";
  str << "{\n";
  str << marshallMsg();
  str << "  UsrToEnv(impl_msg)->setMsgtype(" << node << "BocInitMsg);\n";
  str << "  CkGroupID gId = CkCreate" << node << "Group(" << chareIdx() << ", " << epIdx()
      << ", impl_msg);\n";

  genTramInstantiation(str);

  str << "  return gId;\n";
  str << "}\n";

  if (!param->isVoid()) {
    str << makeDecl(" ", 1) << "::" << container->proxyName(0) << "(" << paramType(0, 1)
        << ")\n";
    str << "{\n";
    str << marshallMsg();
    str << "  UsrToEnv(impl_msg)->setMsgtype(" << node << "BocInitMsg);\n";
    str << "  ckSetGroupID(CkCreate" << node << "Group(" << chareIdx() << ", " << epIdx()
        << ", impl_msg));\n";
    str << "}\n";
  }
}

/******************* Python Entry Point Code **************************/
void Entry::genPythonDecls(XStr& str) {
  str << "/* STATIC DECLS: ";
  print(str);
  str << " */\n";
  if (isPython()) {
    str << "PyObject *_Python_" << container->baseName() << "_" << name
        << "(PyObject *self, PyObject *arg);\n";
  }
}

void Entry::genPythonDefs(XStr& str) {
  str << "/* DEFS: ";
  print(str);
  str << " */\n";
  if (isPython()) {
    str << "PyObject *_Python_" << container->baseName() << "_" << name
        << "(PyObject *self, PyObject *arg) {\n";
    str << "  PyObject *dict = PyModule_GetDict(PyImport_AddModule(\"__main__\"));\n";
    str << "  int pyNumber = "
           "PyInt_AsLong(PyDict_GetItemString(dict,\"__charmNumber__\"));\n";
    str << "  PythonObject *pythonObj = (PythonObject "
           "*)PyLong_AsVoidPtr(PyDict_GetItemString(dict,\"__charmObject__\"));\n";
    str << "  " << container->baseName() << " *object = static_cast<"
        << container->baseName() << "*>(pythonObj);\n";
    str << "  object->pyWorkers[pyNumber].arg=arg;\n";
    str << "  object->pyWorkers[pyNumber].result=&CtvAccess(pythonReturnValue);\n";
    str << "  object->pyWorkers[pyNumber].pythread=PyThreadState_Get();\n";
    str << "  CtvAccess(pythonReturnValue) = 0;\n";

    str << "  //pyWorker->thisProxy." << name << "(pyNumber);\n";
    str << "  object->" << name << "(pyNumber);\n";

    str << "  //CthSuspend();\n";

    str << "  if (CtvAccess(pythonReturnValue)) {\n";
    str << "    return CtvAccess(pythonReturnValue);\n";
    str << "  } else {\n";
    str << "    Py_INCREF(Py_None); return Py_None;\n";
    str << "  }\n";
    str << "}\n";
  }
}

void Entry::genPythonStaticDefs(XStr& str) {
  if (isPython()) {
    str << "  {\"" << name << "\",_Python_" << container->baseName() << "_" << name
        << ",METH_VARARGS},\n";
  }
}

void Entry::genPythonStaticDocs(XStr& str) {
  if (isPython()) {
    str << "\n  \"" << name << " -- \"";
    if (pythonDoc) str << (char*)pythonDoc;
    str << "\"\\\\n\"";
  }
}

/******************* Accelerator (Accel) Entry Point Code ********************/

void Entry::genAccelFullParamList(XStr& str, int makeRefs) {
  if (!isAccel()) return;

  ParamList* curParam = NULL;
  int isFirst = 1;

  // Parameters (which are read only by default)
  curParam = param;
  if ((curParam->param->getType()->isVoid()) && (curParam->param->getName() == NULL)) {
    curParam = curParam->next;
  }
  while (curParam != NULL) {
    if (!isFirst) {
      str << ", ";
    }

    Parameter* param = curParam->param;

    if (param->isArray()) {
      str << param->getType()->getBaseName() << "* " << param->getName();
    } else {
      str << param->getType()->getBaseName() << " " << param->getName();
    }

    isFirst = 0;
    curParam = curParam->next;
  }

  // Accel parameters
  curParam = accelParam;
  while (curParam != NULL) {
    if (!isFirst) {
      str << ", ";
    }

    Parameter* param = curParam->param;
    int bufType = param->getAccelBufferType();
    int needWrite = makeRefs && ((bufType == Parameter::ACCEL_BUFFER_TYPE_READWRITE) ||
                                 (bufType == Parameter::ACCEL_BUFFER_TYPE_WRITEONLY));
    if (param->isArray()) {
      str << param->getType()->getBaseName() << "* " << param->getName();
    } else {
      str << param->getType()->getBaseName() << ((needWrite) ? (" &") : (" "))
          << param->getName();
    }

    isFirst = 0;
    curParam = curParam->next;
  }

  // Implied object pointer
  if (!isFirst) {
    str << ", ";
  }
  str << container->baseName() << "* impl_obj";
}

void Entry::genAccelFullCallList(XStr& str) {
  if (!isAccel()) return;

  int isFirstFlag = 1;

  // Marshalled parameters to entry method
  ParamList* curParam = param;
  if ((curParam->param->getType()->isVoid()) && (curParam->param->getName() == NULL)) {
    curParam = curParam->next;
  }
  while (curParam != NULL) {
    if (!isFirstFlag) str << ", ";
    isFirstFlag = 0;
    str << curParam->param->getName();
    curParam = curParam->next;
  }

  // General variables (prefix with "impl_obj->" for member variables of the current
  // object)
  curParam = accelParam;
  while (curParam != NULL) {
    if (!isFirstFlag) str << ", ";
    isFirstFlag = 0;
    str << (*(curParam->param->getAccelInstName()));
    curParam = curParam->next;
  }

  // Implied object
  if (!isFirstFlag) str << ", ";
  isFirstFlag = 0;
  str << "impl_obj";
}

void Entry::genAccelIndexWrapperDecl_general(XStr& str) {
  str << "    static void _accelCall_general_" << epStr() << "(";
  genAccelFullParamList(str, 1);
  str << ");\n";
}

void Entry::genAccelIndexWrapperDef_general(XStr& str) {
  str << makeDecl("void") << "::_accelCall_general_" << epStr() << "(";
  genAccelFullParamList(str, 1);
  str << ") {\n\n";

  //// DMK - DEBUG
  // str << "  // DMK - DEBUG\n";
  // str << "  CkPrintf(\"[DEBUG-ACCEL] :: [PPE] - "
  //    << makeDecl("void") << "::_accelCall_general_" << epStr()
  //    << "(...) - Called...\\n\");\n\n";

  str << (*accelCodeBody);

  str << "\n\n";
  str << "  impl_obj->" << (*accelCallbackName) << "();\n";
  str << "}\n";
}

/******************* Shared Entry Point Code **************************/
void Entry::genIndexDecls(XStr& str) {
  str << "    /* DECLS: ";
  print(str);
  str << "     */";

  XStr templateSpecLine;
  templateSpecLine << "\n    " << generateTemplateSpec(tspec);

  // Entry point index storage
  str << "\n    // Entry point registration at startup" << templateSpecLine
      << "\n    static int reg_" << epStr()
      << "();"  ///< @note: Should this be generated as private?
      << "\n    // Entry point index lookup" << templateSpecLine
      << "\n    inline static int idx_" << epStr() << "() {"
      << "\n      static int epidx = " << epRegFn(0) << ";"
      << "\n      return epidx;"
      << "\n    }\n";

  if (!isConstructor()) {
    str << templateSpecLine << "\n    inline static int idx_" << name << "(" << retType
        << " (" << container->baseName() << "::*)(";
    if (param) param->print(str);
    str << ") ) {"
        << "\n      return " << epIdx(0) << ";"
        << "\n    }\n\n";
  }

  // Index function, so user can find the entry point number
  str << templateSpecLine << "\n    static int ";
  if (isConstructor())
    str << "ckNew";
  else
    str << name;
  str << "(" << paramType(1, 0) << ") { return " << epIdx(0) << "; }";

  // DMK - Accel Support
  if (isAccel()) {
    genAccelIndexWrapperDecl_general(str);
  }

  if (isReductionTarget()) {
    str << "\n    // Entry point registration at startup" << templateSpecLine
        << "\n    static int reg_" << epStr(true)
        << "();"  ///< @note: Should this be generated as private?
        << "\n    // Entry point index lookup" << templateSpecLine
        << "\n    inline static int idx_" << epStr(true) << "() {"
        << "\n      static int epidx = " << epRegFn(0, true) << ";"
        << "\n      return epidx;"
        << "\n    }" << templateSpecLine << "\n    static int "
        << "redn_wrapper_" << name << "(CkReductionMsg* impl_msg) { return "
        << epIdx(0, true) << "; }" << templateSpecLine << "\n    static void _call_"
        << epStr(true) << "(void* impl_msg, void* impl_obj_void);";
  }

  // call function declaration
  if (!isWhenIdle()) {
    str << templateSpecLine << "\n    static void _call_" << epStr()
        << "(void* impl_msg, void* impl_obj);";
  } else {
    str << templateSpecLine << "\n    static void _call_" << epStr()
        << "(void* impl_obj, double time);";
  }
  str << templateSpecLine << "\n    static void _call_sdag_" << epStr()
      << "(void* impl_msg, void* impl_obj);";
  if (isThreaded()) {
    str << templateSpecLine << "\n    static void _callthr_" << epStr()
        << "(CkThrCallArg *);";
  }
  if (hasCallMarshall) {
    str << templateSpecLine << "\n    static int _callmarshall_" << epStr()
        << "(char* impl_buf, void* impl_obj_void);";
  }
  if (param->isMarshalled()) {
    str << templateSpecLine << "\n    static void _marshallmessagepup_" << epStr()
        << "(PUP::er &p,void *msg);";
  }
  str << "\n";
}

void Entry::genDecls(XStr& str) {
  if (external) return;

  str << "/* DECLS: ";
  print(str);
  str << " */\n";

  if (isMigrationConstructor()) {
  }  // User cannot call the migration constructor
  else if (container->isGroup()) {
    genGroupDecl(str);
  } else if (container->isArray()) {
    if (!isIget())
      genArrayDecl(str);
    else if (container->isForElement())
      genArrayDecl(str);
  } else {  // chare or mainchare
    genChareDecl(str);
  }
}

void Entry::genClosureEntryDecls(XStr& str) { genClosure(str, false); }

void Entry::genClosureEntryDefs(XStr& str) {
  templateGuardBegin(tspec || container->isTemplated(), str);
  genClosure(str, true);
  templateGuardEnd(str);
}

void Entry::genClosure(XStr& decls, bool isDef) {
  if (isConstructor() || (isLocal() && !sdagCon)) return;

  bool hasArray = false, isMessage = false, hasRdma = false;
  XStr messageType;
  int i = 0, addNumRdmaFields = 1;
  XStr structure, toPup, alloc, getter, dealloc;
  for (ParamList *pl = param; pl != NULL; pl = pl->next, i++) {
    Parameter* sv = pl->param;

    if (XStr(sv->type->getBaseName()) == "CkArrayOptions") continue;

    structure << "      ";
    getter << "      ";

    if ((sv->isMessage() != 1) && (sv->isVoid() != 1)) {
      if (sv->isRdma()) {
        hasRdma = hasRdma || true;
        if (sv->isDevice()) {
          // Device RDMA
          if (sv->isFirstDeviceRdma()) {
            structure << "int num_device_rdma_fields;\n";
            getter << "int & getP" << i++ << "() { return "
                   << "num_device_rdma_fields; }\n";
            toPup << "        char *impl_buf_device = _impl_marshall ? "
                  << "_impl_marshall->msgBuf : _impl_buf_in;\n";
            toPup << "        __p | num_device_rdma_fields;\n";
          }
          structure << "      "
                    << "CkDeviceBuffer "
                    << "deviceBuffer_" << sv->name << ";\n";
          getter << "      "
                 << "CkDeviceBuffer & getP" << i << "() { return "
                 << "deviceBuffer_" << sv->name << "; }\n";
          toPup << "        if (__p.isPacking()) {\n"
                << "          deviceBuffer_" << sv->name << ".ptr = "
                << "(void *)((char *)(deviceBuffer_" << sv->name << ".ptr) "
                << "- impl_buf_device);\n"
                << "        }\n"
                << "        __p | deviceBuffer_" << sv->name << ";\n";
        } else {
          // CPU RDMA
          if (sv->isFirstRdma()) {
            structure << "      "
                      << "int num_rdma_fields;\n";
            structure << "      "
                      << "int num_root_node;\n";
            getter << "      "
                   << "int "
                   << "& "
                   << "getP" << i << "() { return "
                   << " num_rdma_fields;}\n";
            i++;
            getter << "      "
                   << "int "
                   << "& "
                   << "getP" << i << "() { return "
                   << " num_root_node;}\n";
            i++;
          }
          structure << "      "
                    << "CkNcpyBuffer "
                    << "ncpyBuffer_" << sv->name << ";\n";
          getter << "      "
                 << "CkNcpyBuffer "
                 << "& "
                 << "getP" << i << "() { return "
                 << "ncpyBuffer_" << sv->name << ";}\n";
          if (sv->isFirstRdma()) {
            toPup << "        char *impl_buf = _impl_marshall ? _impl_marshall->msgBuf : "
                     "_impl_buf_in;\n";
            toPup << "        "
                  << "__p | "
                  << "num_rdma_fields;\n";
            toPup << "        "
                  << "__p | "
                  << "num_root_node;\n";
          }
          /* The Rdmawrapper's pointer stores the offset to the actual buffer
           * from the beginning of the msgBuf while packing (as the pointer itself is
           * invalid upon migration). During unpacking (after migration), the offset is used
           * to adjust the pointer back to the actual buffer that exists within the message.
           */
          toPup << "        if (__p.isPacking()) {\n";
          toPup << "          ncpyBuffer_" << sv->name << ".ptr = ";
          toPup << "(void *)((char *)(ncpyBuffer_" << sv->name << ".ptr) - impl_buf);\n";
          toPup << "        }\n";
          toPup << "        "
                << "__p | "
                << "ncpyBuffer_" << sv->name << ";\n";
        }
      } else {
        structure << "      ";
        getter << "      ";
        structure << sv->type << " ";
        getter << sv->type << " ";
      }

      if (sv->isArray() != 0) {
        structure << "*";
        getter << "*";
      }
      if (sv->isArray() != 0) {
        hasArray = hasArray || true;
      } else if (!sv->isRdma()) {
        toPup << "        "
              << "__p | " << sv->name << ";\n";
        sv->podType = true;
      }

      if (sv->name != 0) {
        if (!sv->isRdma()) {
          structure << sv->name << ";\n";
          getter << "& "
                 << "getP" << i << "() { return " << sv->name << ";}\n";
        }
      }

    } else if (sv->isVoid() != 1) {
      if (sv->isMessage()) {
        isMessage = true;
        if (sv->isRdma()) {
          structure << "CkNcpyBuffer"
                    << " "
                    << "ncpyBuffer_" << sv->name << ";\n";
        } else {
          structure << sv->type << " " << sv->name << ";\n";
        }
        toPup << "        "
              << "CkPupMessage(__p, (void**)&" << sv->name << ");\n";
        messageType << sv->type->deref();
      }
    }
  }

  structure << "\n";

  toPup << "        packClosure(__p);\n";

  XStr initCode;
  initCode << "        init();\n";

  if (hasArray || hasRdma) {
    structure << "      "
              << "CkMarshallMsg* _impl_marshall;\n";
    structure << "      "
              << "char* _impl_buf_in;\n";
    structure << "      "
              << "int _impl_buf_size;\n";
    dealloc << "        if (_impl_marshall) CmiFree(UsrToEnv(_impl_marshall));\n";
    initCode << "        _impl_marshall = 0;\n";
    initCode << "        _impl_buf_in = 0;\n";
    initCode << "        _impl_buf_size = 0;\n";

    toPup << "        __p | _impl_buf_size;\n";
    toPup << "        bool hasMsg = (_impl_marshall != 0); __p | hasMsg;\n";
    toPup << "        "
          << "if (hasMsg) CkPupMessage(__p, (void**)&"
          << "_impl_marshall"
          << ");\n";
    toPup << "        "
          << "else PUParray(__p, _impl_buf_in, _impl_buf_size);\n";
    toPup << "        if (__p.isUnpacking()) {\n";
    toPup << "          char *impl_buf = _impl_marshall ? _impl_marshall->msgBuf : "
             "_impl_buf_in;\n";
    param->beginUnmarshallSDAG(toPup);
    toPup << "        }\n";
  }

  // Generate code for ensuring we don't migrate active local closures
  if (isLocal()) {
    toPup.clear();
    toPup
        << "        CkAbort(\"Can\'t migrate while a local SDAG method is active.\");\n";
  }

  if (!isMessage) {
    genClosureTypeName = new XStr();
    genClosureTypeNameProxy = new XStr();
    *genClosureTypeNameProxy << "Closure_" << container->baseName() << "::";
    *genClosureTypeNameProxy << name << "_" << entryCount << "_closure";
    *genClosureTypeName << name << "_" << entryCount << "_closure";

    container->sdagPUPReg << "  PUPable_reg(SINGLE_ARG(" << *genClosureTypeNameProxy
                          << "));\n";

    if (isDef) {
      if (container->isTemplated()) {
        decls << container->tspec(false) << "\n";
      }
      decls << generateTemplateSpec(tspec) << "\n";
      decls << "    struct " << *genClosureTypeNameProxy << " : public SDAG::Closure"
            << " {\n";
      decls << structure << "\n";
      decls << "      " << *genClosureTypeName << "() {\n";
      decls << initCode;
      decls << "      }\n";
      decls << "      " << *genClosureTypeName << "(CkMigrateMessage*) {\n";
      decls << initCode;
      decls << "      }\n";
      decls << getter;
      decls << "      void pup(PUP::er& __p) {\n";
      decls << toPup;
      decls << "      }\n";
      decls << "      virtual ~" << *genClosureTypeName << "() {\n";
      decls << dealloc;
      decls << "      }\n";
      decls << "      "
            << ((container->isTemplated() || tspec) ? "PUPable_decl_template"
                                                    : "PUPable_decl")
            << "(SINGLE_ARG(" << *genClosureTypeName;
      if (tspec) {
        decls << "<";
        tspec->genShort(decls);
        decls << ">";
      }
      decls << "));\n";
      decls << "    };\n";
    } else {
      decls << generateTemplateSpec(tspec) << "\n";
      decls << "    struct " << *genClosureTypeName << ";\n";
    }
  } else {
    genClosureTypeName = new XStr();
    genClosureTypeNameProxy = new XStr();
    *genClosureTypeNameProxy << messageType;
    *genClosureTypeName << messageType;
  }

  genClosureTypeNameProxyTemp = new XStr();
  *genClosureTypeNameProxyTemp << (container->isTemplated() ? "typename " : "")
                               << genClosureTypeNameProxy;
}

// This routine is only used in Entry::genDefs.
// It ends the current procedure with a call to awaken another thread,
// and defines the thread function to handle that call.
XStr Entry::callThread(const XStr& procName, int prependEntryName) {
  if (isConstructor() || isMigrationConstructor())
    die("Constructors may not be 'threaded'", first_line_);

  XStr str, procFull;
  procFull << "_callthr_";
  if (prependEntryName) procFull << name << "_";
  procFull << procName;

  str << "  CthThread tid = CthCreate((CthVoidFn)" << procFull
      << ", new CkThrCallArg(impl_msg,impl_obj), " << getStackSize() << ");\n";
  str << "  ((Chare *)impl_obj)->CkAddThreadListeners(tid,impl_msg);\n";
// str << "  CkpvAccess(_traces)->CkAddThreadListeners(tid);\n";
  str << "  CthTraceResume(tid);\n"
      << "  CthResume(tid);\n"
      << "}\n";

  str << makeDecl("void") << "::" << procFull << "(CkThrCallArg *impl_arg)\n";
  str << "{\n";
  str << "  void *impl_msg = impl_arg->msg;\n";
  str << "  void *impl_obj_void = impl_arg->obj;\n";
  str << "  " << container->baseName() << " *impl_obj = static_cast<"
      << container->baseName() << " *>(impl_obj_void);\n";
  str << "  delete impl_arg;\n";
  return str;
}

/*
  Generate the code to actually unmarshall the parameters and call
  the entry method.
*/
void Entry::genCall(XStr& str, const XStr& preCall, bool redn_wrapper, bool usesImplBuf) {
  bool isArgcArgv = false;
  bool isMigMain = false;
  bool isSDAGGen = sdagCon || isWhenEntry;
  bool needsClosure = isSDAGGen && (param->isMarshalled() ||
                                    (param->isVoid() && isWhenEntry && redn_wrapper));

  if (isConstructor() && container->isMainChare() && (!param->isVoid()) &&
      (!param->isCkArgMsgPtr())) {
    if (param->isCkMigMsgPtr())
      isMigMain = true;
    else
      isArgcArgv = true;
  } else {
    // Normal case: Unmarshall variables
    if (redn_wrapper)
      param->beginRednWrapperUnmarshall(str, needsClosure);
    else {
      if (isSDAGGen)
        param->beginUnmarshallSDAGCall(str, usesImplBuf);
      else
        param->beginUnmarshall(str);
    }
  }

  str << preCall;
  if (!isConstructor() && fortranMode && !isWhenEntry && !sdagCon) {
    if (!container->isArray()) {  // Currently, only arrays are supported
      cerr << (char*)container->baseName()
           << ": only chare arrays are currently supported\n";
      exit(1);
    }
    str << "/* FORTRAN */\n";
    XStr dim;
    dim << ((Array*)container)->dim();
    if (dim == (const char*)"1D")
      str << "  int index1 = impl_obj->thisIndex;\n";
    else if (dim == (const char*)"2D") {
      str << "  int index1 = impl_obj->thisIndex.x;\n";
      str << "  int index2 = impl_obj->thisIndex.y;\n";
    } else if (dim == (const char*)"3D") {
      str << "  int index1 = impl_obj->thisIndex.x;\n";
      str << "  int index2 = impl_obj->thisIndex.y;\n";
      str << "  int index3 = impl_obj->thisIndex.z;\n";
    }
    str << "  ::";
    str << fortranify(container->baseName(), "_Entry_", name);
    str << "((char **)(impl_obj->user_data)";
    str << ", &index1";
    if (dim == (const char*)"2D" || dim == (const char*)"3D") str << ", &index2";
    if (dim == (const char*)"3D") str << ", &index3";
    if (!param->isVoid()) {
      str << ", ";
      param->unmarshallAddress(str);
    }
    str << ");\n";
    str << "/* FORTRAN END */\n";
  }

  // DMK : Accel Support
  else if (isAccel()) {
    str << "  _accelCall_general_" << epStr() << "(";
    genAccelFullCallList(str);
    str << ");\n";
  }

  else {  // Normal case: call regular method
    if(param->hasRecvRdma()) {
      str << "  if(CMI_IS_ZC_RECV(env) || CMI_ZC_MSGTYPE(env) == CMK_ZC_BCAST_RECV_DONE_MSG) {\n";
      genRegularCall(str, preCall, redn_wrapper, usesImplBuf, true);
      str << "  else if(CMI_ZC_MSGTYPE(env) == CMK_ZC_BCAST_RECV_DONE_MSG) {\n";
      genRegularCall(str, preCall, redn_wrapper, usesImplBuf, false);
      str << "    }\n";
      str << "  } else {\n";
    } else if (param->hasDevice()) {
      str << "  bool is_inline = true;\n";
      str << "  if (CMI_ZC_MSGTYPE(env) == CMK_ZC_DEVICE_MSG) {\n";
      genRegularCall(str, preCall, redn_wrapper, usesImplBuf, true);
      str << "  }\n";
      str << "  if (is_inline) {\n";
    }
    genRegularCall(str, preCall, redn_wrapper, usesImplBuf, false);
    if(param->hasRecvRdma()) {
      str << "  }\n";
    } else if (param->hasDevice()) {
      str << "  }\n";
    }
  }
}

void Entry::genRegularCall(XStr& str, const XStr& preCall, bool redn_wrapper, bool usesImplBuf, bool isRdmaPost) {
    bool isArgcArgv = false;
    bool isMigMain = false;
    bool isSDAGGen = sdagCon || isWhenEntry;
    bool needsClosure = isSDAGGen && (param->isMarshalled() ||
                                      (param->isVoid() && isWhenEntry && redn_wrapper));



    if (isArgcArgv) str << "  CkArgMsg *m=(CkArgMsg *)impl_msg;\n";  // Hack!

    if (isMigrationConstructor() && container->isArray()) {
      // Make definition of CkMigrateMessage constructor optional for chare
      // array elements, but not for chare/group/nodegroup types that are
      // explicitly marked [migratable]
      str << "  call_migration_constructor<" << container->baseName()
          << "> c = impl_obj_void;\n"
          << "  c";
    } else if (isConstructor()) {  // Constructor: call "new (obj) foo(parameters)"
      str << "  new (impl_obj_void) " << container->baseName();
    } else {  // Regular entry method: call "obj->bar(parameters)"
      str << "  impl_obj->" << (tspec ? "template " : "") << (containsWhenConstruct ? "_sdag_fnc_" : "" ) << name;
      if (tspec) {
        str << "<";
        tspec->genShort(str);
        str << ">";
      }
    }

    if (isArgcArgv) {  // Extract parameters from CkArgMsg (should be parameter
                       // marshalled)
      str << "(m->argc,m->argv);\n";
      str << "  delete m;\n";
    } else if (isMigMain) {
      str << "((CkMigrateMessage*)impl_msg);\n";
    } else {  // Normal case: unmarshall parameters (or just pass message)
      if (isSDAGGen) {
        str << "(";
        if (param->isMessage()) {
          param->unmarshall(str, false, true, isRdmaPost);
        } else if (needsClosure) {
          if(isRdmaPost)
            param->unmarshall(str, false, true, isRdmaPost);
          else
            str << "genClosure";
        }
        // Add CkNcpyBufferPost as the last parameter
        if(isRdmaPost) {
          if (param->hasDevice()) {
            str << ", devicePost";
          } else {
            str << ",ncpyPost";
          }
        }
        str << ");\n";
        if (needsClosure) {
          if(!isRdmaPost)
            str << "  genClosure->deref();\n";
        }
      } else {
        str << "(";
        param->unmarshall(str, false, true, isRdmaPost);
        // Add CkNcpyBufferPost as the last parameter
        if(isRdmaPost) {
          if (param->hasDevice()) {
            str << ", devicePost";
          } else {
            str << ",ncpyPost";
          }
        }
        str << ");\n";
      }
      if(isRdmaPost) {
        // Allocate an array of rdma pointers
        if (param->hasDevice()) {
          str << "  void *buffPtrs["<< numRdmaDeviceParams <<"];\n";
          str << "  int buffSizes["<< numRdmaDeviceParams <<"];\n";
        } else {
          str << "  void *buffPtrs["<< numRdmaRecvParams <<"];\n";
          str << "  int buffSizes["<< numRdmaRecvParams <<"];\n";
        }
        param->storePostedRdmaPtrs(str, isSDAGGen);
        if (param->hasDevice()) {
          // is_inline determines if the regular entry method should be invoked
          // right after the post entry method or if it should be invoked later
          // with a message
          str << "  if(CMI_IS_ZC_DEVICE(env))\n";
          str << "    is_inline = CkRdmaDeviceIssueRgets(env, ";
          if (isSDAGGen)
            str << "genClosure->num_device_rdma_fields, ";
          else
            str << "impl_num_device_rdma_fields, ";
          str << "buffPtrs, buffSizes, devicePost);\n";
        } else {
          str << "  if(CMI_IS_ZC_RECV(env)) \n";
          str << "    CkRdmaIssueRgets(env, ((CMI_ZC_MSGTYPE(env) == CMK_ZC_BCAST_RECV_MSG) ? ncpyEmApiMode::BCAST_RECV : ncpyEmApiMode::P2P_RECV), ";
          if(isSDAGGen)
            str << "genClosure->num_rdma_fields, genClosure->num_root_node, ";
          else
            str << "impl_num_rdma_fields, impl_num_root_node, ";
          str << "buffPtrs, buffSizes, ncpyPost);\n";
        }
      }
      // pack pointers if it's a broadcast message
      if(param->hasRdma() && !container->isForElement() && !isRdmaPost) {
        // pack rdma pointers for broadcast unmarshall
        // this is done to support broadcasts before all chare array elements are
        // finished with their EM execution using the same msg
      }
    }
}

void Entry::genDefs(XStr& str) {
  if (external) return;
  XStr containerType = container->baseName();
  XStr preMarshall, preCall, postCall;

  templateGuardBegin(tspec || container->isTemplated(), str);
  str << "/* DEFS: ";
  print(str);
  str << " */\n";

  if (isMigrationConstructor()) {
  }  // User cannot call the migration constructor
  else if (isTramTarget() && container->isForElement()) {
    genTramDefs(str);
  } else if (container->isGroup()) {
    genGroupDefs(str);
  } else if (container->isArray()) {
    genArrayDefs(str);
  } else
    genChareDefs(str);

  if (!isConstructor() && fortranMode) {
    str << "/* FORTRAN SECTION */\n";

    // Declare the Fortran Entry Function
    // This is called from C++
    str << "extern \"C\" ";
    str << "void ";
    str << fortranify(container->baseName(), "_Entry_", name);
    str << "(char **";
    str << ", " << container->indexList();
    if (!param->isVoid()) {
      str << ", ";
      param->printAddress(str);
    }
    str << ");\n";

    str << "/* FORTRAN SECTION END */\n";
  }

  if (container->isMainChare() || container->isChare() || container->isForElement()) {
    if (isReductionTarget()) {
      XStr retStr;
      retStr << retType;
      str << makeDecl(retStr);
      // str << retType << " " << indexName(); //makeDecl(retStr, 1)
      str << "::_call_" << epStr(true) << "(void* impl_msg, void* impl_obj_void)"
          << "\n{"
          << "\n  " << container->baseName() << "* impl_obj = static_cast<"
          << container->baseName() << "*> (impl_obj_void);\n"
          << "  char* impl_buf = (char*)((CkReductionMsg*)impl_msg)->getData();\n";
      XStr precall;
      genCall(str, precall, true, false);
      if (!(sdagCon || isWhenEntry))
        str << "  delete (CkReductionMsg*)impl_msg;\n}\n\n";
      else
        str << "  \n}\n\n";
    }
  }

  // Prevents repeated call and __idx definitions:
  if (container->getForWhom() != forAll) {
    templateGuardEnd(str);
    return;
  }

  // Define the entry point registration functions
  str << "\n// Entry point registration function"
      << "\n"
      << makeDecl("int") << "::reg_" << epStr() << "() {"
      << "\n  int epidx = " << genRegEp() << ";";
  if (hasCallMarshall)
    str << "\n  CkRegisterMarshallUnpackFn(epidx, "
        << "_callmarshall_" << epStr(false, true) << ");";
  if (param->isMarshalled()) {
    str << "\n  CkRegisterMessagePupFn(epidx, "
        << "_marshallmessagepup_" << epStr(false, true) << ");\n";
  } else if (param->isMessage() && !isMigrationConstructor()) {
    str << "\n  CkRegisterMessagePupFn(epidx, (CkMessagePupFn)";
    param->param->getType()->deref()->print(str);
    str << "::ckDebugPup);";
  }
  str << "\n  return epidx;"
      << "\n}\n\n";

  if (isReductionTarget()) {
    str << "\n// Redn wrapper registration function"
        << "\n"
        << makeDecl("int") << "::reg_" << epStr(true) << "() {"
        << "\n  return " << genRegEp(true) << ";"
        << "\n}\n\n";
  }

  // Add special pre- and post- call code
  if (isSync() || isIget()) {
    // A synchronous method can return a value, and must finish before
    // the caller can proceed.
    preMarshall
        << "  int impl_ref = CkGetRefNum(impl_msg), impl_src = CkGetSrcPe(impl_msg);\n";
    if (retType->isVoid() || retType->isMessage()) preCall << "  void *impl_retMsg=";
    if (retType->isVoid()) {
      preCall << "CkAllocSysMsg();\n  ";
    } else if (retType->isMessage()) {
      preCall << "(void *) ";
    } else {
      preCall << "  " << retType << " impl_ret_val= ";
      postCall << "  //Marshall: impl_ret_val\n";
      postCall << "  int impl_ret_size=0;\n";
      postCall << "  { //Find the size of the PUP'd data\n";
      postCall << "    PUP::sizer implPS;\n";
      postCall << "    implPS|impl_ret_val;\n";
      postCall << "    impl_ret_size+=implPS.size();\n";
      postCall << "  };\n";
      postCall
          << "  CkMarshallMsg *impl_retMsg=CkAllocateMarshallMsg(impl_ret_size, NULL);\n";
      postCall << "  { //Copy over the PUP'd data;\n";
      postCall << "    PUP::toMem implPS((void *)impl_retMsg->msgBuf);\n";
      postCall << "    implPS|impl_ret_val;\n";
      postCall << "  };\n";
    }
    postCall << "  CkSendToFutureID(impl_ref, impl_retMsg, impl_src);\n";
  } else if (isExclusive()) {
    // An exclusive method
    preMarshall
        << "  if(CmiTryLock(impl_obj->__nodelock)) {\n"; /*Resend msg. if lock busy*/
    /******* DANGER-- RESEND CODE UNTESTED **********/
    if (param->isMarshalled()) {
      preMarshall << "    impl_msg = CkCopyMsg(&impl_msg);\n";
    }
    preMarshall << "    CkSendMsgNodeBranch(" << epIdx()
                << ",impl_msg,CkMyNode(),impl_obj->CkGetNodeGroupID());\n";
    preMarshall << "    return;\n";
    preMarshall << "  }\n";

    postCall << "  CmiUnlock(impl_obj->__nodelock);\n";
  }

  if (param->isVoid() && !isNoKeep()) {
    /* Reuse the message using CkFreeSysMsg by putting it in the msgpool if it is a fixed
     * sized message. The message is a fixed sized message if it has no priority bytes. A
     * message with priority bytes will not be reused and will be deallocated similar to
     * other marshalled messages.
     */
    postCall << "  if(UsrToEnv(impl_msg)->isVarSysMsg() == 0)\n";
    postCall << "    CkFreeSysMsg(impl_msg);\n";
  }

  if (!isConstructor() && fortranMode) {  // Fortran90
    str << "/* FORTRAN SECTION */\n";

    XStr dim;
    dim << ((Array*)container)->dim();

    // Define the Fortran interface function
    // This is called from Fortran to send the message to a chare.
    str << "extern \"C\" ";
    str << "void ";
    str << fortranify(container->baseName(), "_Invoke_", name);
    str << "(void** aindex";
    str << ", " << container->indexList();
    if (!param->isVoid()) {
      str << ", ";
      param->printAddress(str);
    }
    str << ")\n";
    str << "{\n";
    str << "  CkArrayID *aid = (CkArrayID *)*aindex;\n";
    str << "\n";
    str << "  " << container->proxyName() << " h(*aid);\n";
    if (dim == (const char*)"1D")
      str << "  h[*index1]." << name << "(";
    else if (dim == (const char*)"2D")
      str << "  h[CkArrayIndex2D(*index1, *index2)]." << name << "(";
    else if (dim == (const char*)"3D")
      str << "  h[CkArrayIndex3D(*index1, *index2, *index3)]." << name << "(";
    if (!param->isVoid()) param->printValue(str);
    str << ");\n";
    str << "}\n";

    if (container->isArray()) {
      str << "extern \"C\" ";
      str << "void ";
      str << fortranify(container->baseName(), "_Broadcast_", name);
      str << "(void** aindex";
      if (!param->isVoid()) {
        str << ", ";
        param->printAddress(str);
      }
      str << ")\n";
      str << "{\n";
      str << "  CkArrayID *aid = (CkArrayID *)*aindex;\n";
      str << "\n";
      str << "  " << container->proxyName() << " h(*aid);\n";
        str << "  h." << name << "(";
      if (!param->isVoid()) param->printValue(str);
      str << ");\n";
      str << "}\n";
    }

    if (isReductionTarget()) {
      str << "extern \"C\" ";
      str << "int ";
      str << fortranify(container->baseName(), "_ReductionTarget_", name);
      str << "(void)\n";
      str << "{\n";
      str << "  return CkReductionTarget(" << container->baseName() << ", " << name << ");\n";
      str << "}\n";
    }

    str << "/* FORTRAN SECTION END */\n";
  }

  // DMK - Accel Support
  //   Create the wrapper function for the acceleration call
  //   TODO : FIXME : For now, just use the standard C++ code... create OffloadAPI
  //   wrappers later
  if (isAccel()) {
    genAccelIndexWrapperDef_general(str);
  }

  // Generate the call-method body
  if (isWhenIdle()) {
    str << makeDecl("void") << "::_call_" << epStr()
        << "(void* impl_obj_void, double time)\n";
    str << "{\n";
  } else {
    str << makeDecl("void") << "::_call_" << epStr()
        << "(void* impl_msg, void* impl_obj_void)\n";
    str << "{\n";
  }
  // Do not create impl_obj for migration constructor as compiler throws an unused
  // variable warning otherwise
  if (!isMigrationConstructor()) {
    str << "  " << container->baseName() << "* impl_obj = static_cast<"
        << container->baseName() << "*>(impl_obj_void);\n";
  }
  if (!isLocal()) {
    if (isThreaded()) str << callThread(epStr());
    str << preMarshall;
    if (param->isMarshalled()) {
      if (param->hasConditional())
        str << "  MarshallMsg_" << epStr() << " *impl_msg_typed=(MarshallMsg_" << epStr()
            << " *)impl_msg;\n";
      else
        str << "  CkMarshallMsg *impl_msg_typed=(CkMarshallMsg *)impl_msg;\n";
      str << "  char *impl_buf=impl_msg_typed->msgBuf;\n";
      str << "  envelope *env = UsrToEnv(impl_msg_typed);\n";
    }
    genCall(str, preCall, false, false);
    param->endUnmarshall(str);
    str << postCall;
    if (isThreaded() && param->isMarshalled()) str << "  delete impl_msg_typed;\n";
  } else if (isWhenIdle()) {
    str << "  bool res = impl_obj->" << name << "(time);\n";
    str << "  if (res) CkCallWhenIdle(idx_" << epStr() << "(), impl_obj);\n";
  } else {
    str << "  CkAbort(\"This method should never be called as it refers to a LOCAL entry "
           "method!\");\n";
  }
  str << "}\n";

  if (hasCallMarshall) {
    str << makeDecl("int") << "::_callmarshall_" << epStr()
        << "(char* impl_buf, void* impl_obj_void) {\n";
    str << "  " << containerType << "* impl_obj = static_cast<" << containerType
        << "*>(impl_obj_void);\n";
    str << "  envelope *env = UsrToEnv(impl_buf);\n";
    if (!isLocal()) {
      if (!param->hasConditional()) {
        genCall(str, preCall, false, true);
        /*FIXME: implP.size() is wrong if the parameter list contains arrays--
        need to add in the size of the arrays.
         */
        str << "  return implP.size();\n";
      } else {
        str << "  CkAbort(\"This method is not implemented for EPs using conditional "
               "packing\");\n";
        str << "  return 0;\n";
      }
    } else {
      str << "  CkAbort(\"This method should never be called as it refers to a LOCAL "
             "entry method!\");\n";
      str << "  return 0;\n";
    }
    str << "}\n";
  }
  if (param->isMarshalled()) {
    str << makeDecl("void") << "::_marshallmessagepup_" << epStr()
        << "(PUP::er &implDestP,void *impl_msg) {\n";
    if (!isLocal()) {
      if (param->hasConditional())
        str << "  MarshallMsg_" << epStr() << " *impl_msg_typed=(MarshallMsg_" << epStr()
            << " *)impl_msg;\n";
      else
        str << "  CkMarshallMsg *impl_msg_typed=(CkMarshallMsg *)impl_msg;\n";
      str << "  char *impl_buf=impl_msg_typed->msgBuf;\n";
      str << "  envelope *env = UsrToEnv(impl_msg_typed);\n";
      param->beginUnmarshall(str);
      param->pupAllValues(str);
    } else {
      str << "  /*Fake pupping since we don't really have a message */\n";
      str << "  int n=0;\n";
      str << "  if (implDestP.hasComments()) implDestP.comment(\"LOCAL message\");\n";
      str << "  implDestP|n;\n";
    }
    str << "}\n";
  }

  // to match the registry, generate register call even if there is no SDAG code
  // if ((param->isMarshalled() || param->isVoid()) /* && (sdagCon || isWhenEntry) */)
  if ((param->isMarshalled() || param->isVoid()) && genClosureTypeNameProxy) {
    if (container->isTemplated()) str << container->tspec(false);
    if (tspec) {
      str << "template <";
      tspec->genLong(str, false);
      str << "> ";
    }

    str << ((container->isTemplated() || tspec) ? "PUPable_def_template_nonInst"
                                                : "PUPable_def")
        << "(SINGLE_ARG(" << *genClosureTypeNameProxy;
    if (tspec) {
      str << "<";
      tspec->genShort(str);
      str << ">";
    }
    str << "))\n";
  }

  templateGuardEnd(str);
}

XStr Entry::genRegEp(bool isForRedn) {
  XStr str;
  str << "CkRegisterEp";
  if (tspec) {
    str << "<";
    tspec->genShort(str);
    str << ">";
  }
  str << "(\"";
  if (isForRedn)
    str << "redn_wrapper_" << name << "(CkReductionMsg *impl_msg)\",\n";
  else
    str << name << "(" << paramType(0) << ")\",\n";
  str << "      reinterpret_cast<CkCallFnPtr>(_call_" << epStr(isForRedn, true);
  str << "), ";
  /* messageIdx: */
  if (param->isMarshalled()) {
    if (param->hasConditional())
      str << "MarshallMsg_" << epStr() << "::__idx";
    else
      str << "CkMarshallMsg::__idx";
  } else if (!param->isVoid() && !isMigrationConstructor()) {
    param->genMsgProxyName(str);
    str << "::__idx";
  } else if (isForRedn) {
    str << "CMessage_CkReductionMsg::__idx";
  } else {
    str << "0";
  }
  /* chareIdx */
  str << ", __idx";
  /* attributes */
  str << ", 0";
  // reductiontarget variants should not be nokeep. The actual ep will be
  // parameter marshalled (and hence flagged as nokeep), but we'll delete the
  // CkReductionMsg in generated code, not runtime code. (so that we can cast
  // it to CkReductionMsg not CkMarshallMsg)
  if (!isForRedn && hasAttribute(SNOKEEP)) str << "+CK_EP_NOKEEP";
  if (hasAttribute(SNOTRACE)) str << "+CK_EP_TRACEDISABLE";
  if (hasAttribute(SIMMEDIATE)) {
    str << "+CK_EP_TRACEDISABLE";
    str << "+CK_EP_IMMEDIATE";
  }
  if (hasAttribute(SINLINE)) str << "+CK_EP_INLINE";
  if (hasAttribute(SAPPWORK)) str << "+CK_EP_APPWORK";

  /*MEICHAO*/
  if (hasAttribute(SMEM)) str << "+CK_EP_MEMCRITICAL";

  if (internalMode) str << "+CK_EP_INTRINSIC";
  str << ")";
  return str;
}

void Entry::genReg(XStr& str) {
  if (tspec) return;

  if (external) {
    str << "  CkIndex_" << label << "::idx_" << name;
    if (targs) str << "<" << targs << ">";
    str << "( static_cast<" << retType << " (" << label << "::*)(" << paramType(0, 0)
        << ")>(NULL) );\n";
    return;
  }

  str << "  // REG: " << *this;
  str << "  " << epIdx(0) << ";\n";
  if (isReductionTarget()) str << "  " << epIdx(0, true) << ";\n";
  if (isConstructor()) {
    if (container->isMainChare() && !isMigrationConstructor())
      str << "  CkRegisterMainChare(__idx, " << epIdx(0) << ");\n";
    if (param->isVoid()) str << "  CkRegisterDefaultCtor(__idx, " << epIdx(0) << ");\n";
    if (isMigrationConstructor())
      str << "  CkRegisterMigCtor(__idx, " << epIdx(0) << ");\n";
  }
}

void Entry::preprocess() {
  ParamList* pl = param;
  if (pl != NULL && pl->hasConditional()) {
    XStr str;
    str << "MarshallMsg_" << epStr();
    NamedType* nt = new NamedType(strdup(str));
    MsgVar* var = new MsgVar(new BuiltinType("char"), "msgBuf", 0, 1);
    MsgVarList* list = new MsgVarList(var);
    do {
      if (pl->param->isConditional()) {
        var = new MsgVar(pl->param->getType(), pl->param->getName(), 1, 0);
        list = new MsgVarList(var, list);
      }
    } while (NULL != (pl = pl->next));
    Message* m = new Message(-1, nt, list);
    m->setModule(container->containerModule);
    container->containerModule->prependConstruct(m);
  }

  // DMK - Accel Support
  // Count the total number of scalar and array parameters if this is an accelerated entry
  accel_numScalars = 0;
  accel_numArrays = 0;
  accel_dmaList_numReadOnly = 0;
  accel_dmaList_numReadWrite = 0;
  accel_dmaList_numWriteOnly = 0;
  accel_dmaList_scalarNeedsWrite = 0;
  if (isAccel()) {
    ParamList* curParam = param;
    if ((curParam->param->getType()->isVoid()) && (curParam->param->getName() == NULL)) {
      curParam = curParam->next;
    }
    while (curParam != NULL) {
      if (curParam->param->isArray()) {
        accel_numArrays++;
        accel_dmaList_numReadOnly++;
      } else {
        accel_numScalars++;
      }
      curParam = curParam->next;
    }
    curParam = accelParam;
    while (curParam != NULL) {
      if (curParam->param->isArray()) {
        accel_numArrays++;
        switch (curParam->param->getAccelBufferType()) {
          case Parameter::ACCEL_BUFFER_TYPE_READWRITE:
            accel_dmaList_numReadWrite++;
            break;
          case Parameter::ACCEL_BUFFER_TYPE_READONLY:
            accel_dmaList_numReadOnly++;
            break;
          case Parameter::ACCEL_BUFFER_TYPE_WRITEONLY:
            accel_dmaList_numWriteOnly++;
            break;
          default:
            XLAT_ERROR_NOCOL("unknown accel param type", first_line_);
            break;
        }
      } else {
        accel_numScalars++;
        switch (curParam->param->getAccelBufferType()) {
          case Parameter::ACCEL_BUFFER_TYPE_READWRITE:
            accel_dmaList_scalarNeedsWrite++;
            break;
          case Parameter::ACCEL_BUFFER_TYPE_READONLY:
            break;
          case Parameter::ACCEL_BUFFER_TYPE_WRITEONLY:
            accel_dmaList_scalarNeedsWrite++;
            break;
          default:
            XLAT_ERROR_NOCOL("unknown accel param type", first_line_);
            break;
        }
      }
      curParam = curParam->next;
    }
    if (accel_numScalars > 0) {
      if (accel_dmaList_scalarNeedsWrite) {
        accel_dmaList_numReadWrite++;
      } else {
        accel_dmaList_numReadOnly++;
      }
    }
  }
}

int Entry::paramIsMarshalled(void) { return param->isMarshalled(); }

int Entry::getStackSize(void) { return (stacksize ? stacksize->getIntVal() : 0); }

void Entry::setAccelParam(ParamList* apl) { accelParam = apl; }
void Entry::setAccelCodeBody(XStr* acb) { accelCodeBody = acb; }
void Entry::setAccelCallbackName(XStr* acbn) { accelCallbackName = acbn; }

int Entry::isThreaded(void) { return (hasAttribute(STHREADED)); }
int Entry::isSync(void) { return (hasAttribute(SSYNC)); }
int Entry::isIget(void) { return (hasAttribute(SIGET)); }
int Entry::isConstructor(void) {
  return !strcmp(name, container->baseName(0).get_string());
}
bool Entry::isMigrationConstructor() { return isConstructor() && hasAttribute(SMIGRATE); }
int Entry::isExclusive(void) { return (hasAttribute(SLOCKED)); }
int Entry::isImmediate(void) { return (hasAttribute(SIMMEDIATE)); }
int Entry::isSkipscheduler(void) { return (hasAttribute(SSKIPSCHED)); }
int Entry::isInline(void) { return hasAttribute(SINLINE); }
int Entry::isLocal(void) { return hasAttribute(SLOCAL); }
int Entry::isCreate(void) { return (hasAttribute(SCREATEHERE)) || (hasAttribute(SCREATEHOME)); }
int Entry::isCreateHome(void) { return (hasAttribute(SCREATEHOME)); }
int Entry::isCreateHere(void) { return (hasAttribute(SCREATEHERE)); }
int Entry::isPython(void) { return (hasAttribute(SPYTHON)); }
int Entry::isNoTrace(void) { return (hasAttribute(SNOTRACE)); }
int Entry::isAppWork(void) { return (hasAttribute(SAPPWORK)); }
int Entry::isNoKeep(void) { return (hasAttribute(SNOKEEP)); }
int Entry::isSdag(void) { return (sdagCon != 0); }
bool Entry::isTramTarget(void) { return (hasAttribute(SAGGREGATE)) != 0; }
int Entry::isWhenIdle(void) { return hasAttribute(SWHENIDLE); }

// DMK - Accel support
int Entry::isAccel(void) { return (hasAttribute(SACCEL)); }

int Entry::isMemCritical(void) { return (hasAttribute(SMEM)); }
int Entry::isReductionTarget(void) { return (hasAttribute(SREDUCE)); }

char* Entry::getEntryName() { return name; }
int Entry::getLine() { return line; }
Chare* Entry::getContainer(void) const { return container; }

}  // namespace xi
