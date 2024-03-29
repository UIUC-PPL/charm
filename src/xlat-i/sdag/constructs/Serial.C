#include "Serial.h"

namespace xi {

extern void RemoveSdagComments(char* str);

SerialConstruct::SerialConstruct(const char* code, const char* trace_name, int line_no)
    : BlockConstruct(SSERIAL, NULL, 0, 0, 0, 0, 0, 0), line_no_(line_no) {
  char* tmp = strdup(code);
  text = new XStr(tmp);
  free(tmp);

  if (trace_name) {
    tmp = strdup(trace_name);
    tmp[strlen(tmp) - 1] = 0;
    traceName = new XStr(tmp + 1);
    free(tmp);
  }

  label_str = "serial";
}

void SerialConstruct::propagateStateToChildren(std::list<EncapState*> encap,
                                               std::list<CStateVar*>& stateVarsChildren,
                                               std::list<CStateVar*>& wlist,
                                               int uniqueVarNum) {}

void SerialConstruct::generateCode(XStr& decls, XStr& defs, Entry* entry) {
  generateClosureSignature(decls, defs, entry, false, "void", label, false, encapState);


  generateTraceBeginCall(defs, 1);

  char* str = text->get_string();
  bool hasCode = false;

  while (*str != '\0') {
    if (*str != '\n' && *str != ' ' && *str != '\t') {
      hasCode = true;
      break;
    }
    str++;
  }

  if (hasCode) {
    int indent = unravelClosuresBegin(defs);

    // We would like to properly annotate the generated C++ code for serial blocks
    // blocks so that the C++ compiler can print the right line in the corresponding
    // .ci file in case of syntax errors. To achieve this, we surround the generated
    // code with '#line' directives; the first identifies the .ci line, while the
    // second resets the line pointer to the .def.h header to identify other kinds
    // of errors. However, since at this point we do not have the second line number,
    // we defer that insertion by adding a single '#' that will be eventually
    // replaced with the correct number in Module::generate and addLineNumbers.
    indentBy(defs, indent);
    defs << "{ // begin serial block\n";
    defs << "#line " << line_no_ << " \"" << cur_file << "\"\n";
    defs << text << "\n";
    defs << "#"
         << "\n";
    indentBy(defs, indent);
    defs << "} // end serial block\n";

    unravelClosuresEnd(defs);
  }

  generateTraceEndCall(defs, 1);


  indentBy(defs, 1);
  generateCall(defs, encapState, encapState, next->label, nextBeginOrEnd ? 0 : "_end");
  endMethod(defs);
}

void SerialConstruct::generateTrace() {
  char traceText[1024];
  if (traceName) {
    snprintf(traceText, sizeof(traceText), "%s_%s", CParsedFile::className->charstar(),
             traceName->charstar());
    // remove blanks
    for (unsigned int i = 0; i < strlen(traceText); i++)
      if (traceText[i] == ' ' || traceText[i] == '\t') traceText[i] = '_';
  } else {
    snprintf(traceText, sizeof(traceText), "%s%s", CParsedFile::className->charstar(), label->charstar());
  }
  traceName = new XStr(traceText);

  if (con1) con1->generateTrace();
}

void SerialConstruct::numberNodes(void) {
  nodeNum = numSerials++;
  SdagConstruct::numberNodes();
}

}  // namespace xi
