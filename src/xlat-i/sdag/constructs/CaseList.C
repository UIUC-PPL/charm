#include "CaseList.h"
#include "When.h"
#include <list>

namespace xi {

CaseListConstruct::CaseListConstruct(WhenConstruct* single_construct)
    : SdagConstruct(SCASELIST, single_construct) {
  label_str = "caselist";
}

CaseListConstruct::CaseListConstruct(WhenConstruct* single_construct,
                                     CaseListConstruct* tail)
    : SdagConstruct(SCASELIST, single_construct, tail) {
  label_str = "caselist";
}

void CaseListConstruct::numberNodes() {
  nodeNum = numCaseLists++;
  SdagConstruct::numberNodes();
}

void CaseListConstruct::propagateState(std::list<EncapState*> encap,
                                       std::list<CStateVar*>& plist,
                                       std::list<CStateVar*>& wlist, int uniqueVarNum) {
  CStateVar* sv;
  std::list<CStateVar*>* whensEntryMethodStateVars = NULL;

  encapState = encap;

  stateVars = new std::list<CStateVar*>();

  stateVarsChildren = new std::list<CStateVar*>(plist);
  stateVars->insert(stateVars->end(), plist.begin(), plist.end());
  {
    char txt[128];
    snprintf(txt, sizeof(txt), "_cs%d", nodeNum);
    counter = new XStr(txt);
    sv = new CStateVar(0, "SDAG::CSpeculator *", 0, txt, 0, NULL, 1);
    sv->isSpeculator = true;
    stateVarsChildren->push_back(sv);

    for (std::list<SdagConstruct*>::iterator iter = constructs->begin();
         iter != constructs->end(); ++iter) {
#if CMK_NO_RTTI
      ((WhenConstruct *)*iter)->speculativeState = sv;
#else
      dynamic_cast<WhenConstruct*>(*iter)->speculativeState = sv;
#endif
    }
    std::list<CStateVar*> lst;
    lst.push_back(sv);
    EncapState* state = new EncapState(NULL, lst);
    state->name = new XStr(txt);
    state->type = new XStr("SDAG::CSpeculator");
    encap.push_back(state);
  }

  encapStateChild = encap;

  propagateStateToChildren(encap, *stateVarsChildren, wlist, uniqueVarNum);
}

void CaseListConstruct::generateCode(XStr& decls, XStr& defs, Entry* entry) {
  generateClosureSignature(decls, defs, entry, false, "void", label, false, encapState);
  defs << "  SDAG::CSpeculator* " << counter
       << " = new SDAG::CSpeculator(__dep->getAndIncrementSpeculationIndex());\n";

  defs << "  SDAG::Continuation* c = 0;\n";
  for (std::list<SdagConstruct*>::iterator it = constructs->begin();
       it != constructs->end(); ++it) {
    defs << "  c = ";
    generateCall(defs, encapStateChild, encapStateChild, (*it)->label);
    defs << "  if (!c) return;\n";
    defs << "  else c->speculationIndex = " << counter << "->speculationIndex;\n";
  }
  endMethod(defs);

  snprintf(nameStr, sizeof(nameStr), "%s%s", CParsedFile::className->charstar(), label->charstar());
  strcat(nameStr, "_end");

  generateClosureSignature(decls, defs, entry, false, "void", label, true,
                           encapStateChild);

  defs << "  " << counter << "->deref();\n";
  defs << "  ";
  generateCall(defs, encapState, encapState, next->label, nextBeginOrEnd ? 0 : "_end");
  endMethod(defs);

  generateChildrenCode(decls, defs, entry);
}

}  // namespace xi
