#include "xi-Chare.h"
#include "xi-Entry.h"
#include "xi-Member.h"
#include "xi-Parameter.h"
#include "xi-SdagCollection.h"

#include "sdag/constructs/When.h"

#include "CParsedFile.h"

using std::cout;
using std::endl;

namespace xi {

static void disambig_group(XStr& str, const XStr& super);
static void disambig_array(XStr& str, const XStr& super);
static void disambig_reduction_client(XStr& str, const XStr& super);

static XStr indexSuffix2object(const XStr& indexSuffix);

static const char* CIClassStart =  // prefix, name
    "{\n"
    "  public:\n";

static const char* CIClassEnd = "};\n";

extern int fortranMode;
extern int internalMode;

Chare::Chare(int ln, attrib_t Nattr, NamedType* t, TypeList* b, AstChildren<Member>* l)
    : attrib(Nattr), type(t), list(l), bases(b) {
  line = ln;
  entryCount = 1;
  hasElement = 0;
  forElement = forAll;
  hasSection = 0;
  bases_CBase = NULL;
  setTemplate(0);
  hasSdagEntry = 0;
  hasWhenIdleEntry = 0;
}

void Chare::check() {
  if (list) {
    list->check();
  }
}

void Chare::genRegisterMethodDef(XStr& str) {
  if (external || type->isTemplated()) return;
  templateGuardBegin(isTemplated(), str);
  str << tspec(false) << "void " << indexName()
      << "::__register(const char *s, size_t size) {\n"
         "  __idx = CkRegisterChare(s, size,";
  if (isMainChare())
    str << " TypeMainChare";
  else if (isGroup())
    str << " TypeGroup";
  else if (isNodeGroup())
    str << " TypeNodeGroup";
  else if (isArray())
    str << " TypeArray";
  else if (isChare())
    str << " TypeChare";
  else
    str << " TypeInvalid";
  str << ");\n";

  if (isArray()) {
    str << "  CkRegisterArrayDimensions(__idx, ";
    XStr dim = ((Array*)this)->dim();
    if (dim == (const char*)"1D")
      str << "1";
    else if (dim == (const char*)"2D")
      str << "2";
    else if (dim == (const char*)"3D")
      str << "3";
    else if (dim == (const char*)"4D")
      str << "4";
    else if (dim == (const char*)"5D")
      str << "5";
    else if (dim == (const char*)"6D")
      str << "6";
    else
      str << "-1";

    str << ");\n";
  }

  if (internalMode) str << "  CkRegisterChareInCharm(__idx);\n";
  // register all bases
  genIndexNames(str, "  CkRegisterBase(__idx, ", NULL, "::__idx);\n", "");
  genSubRegisterMethodDef(str);
  if (list) list->genReg(str);
  if (hasSdagEntry) {
    str << "  " << baseName() << "::__sdag_register(); // Potentially missing "
        << baseName() << "_SDAG_CODE in your class definition?\n";
  }
  str << "}\n";
  templateGuardEnd(str);
}

void Chare::outputClosuresDecl(XStr& str) { str << closuresDecl; }

void Chare::outputClosuresDef(XStr& str) { str << closuresDef; }

void Chare::genDecls(XStr& str) {
  if (type->isTemplated()) return;
  str << "/* DECLS: ";
  print(str);
  str << " */\n";

  // include python header and add two methods called execute and iterate.
  // these cannot be added to the .ci file of the PythonCCS interface since
  // different charm object require different definitions...
  if (isPython()) {
    str << "#include \"PythonCCS.h\"\n";
    if (list) {
      Entry* etemp =
          new Entry(0, 0, new BuiltinType("void"), "pyRequest",
                    new ParamList(new Parameter(
                        0, new PtrType(new NamedType("CkCcsRequestMsg", 0)), "msg")),
                    0, 0, 0);
      list->push_back(etemp);
      etemp->setChare(this);
      // etemp = new Entry(0,0,new BuiltinType("void"),"getPrint",new ParamList(new
      // Parameter(0,new PtrType(new NamedType("CkCcsRequestMsg",0)),"msg")),0,0,0,0);
      // list->appendMember(etemp);
      // etemp->setChare(this);
    }
  }

  genTramTypes();

  // Forward declaration of the user-defined implementation class*/
  str << tspec(false) << " class " << type << ";\n";
  str << tspec(false) << " class " << Prefix::Index << type << ";\n";
  str << tspec(false) << " class " << Prefix::Proxy << type << ";\n";
  if (hasElement)
    str << tspec(false) << " class " << Prefix::ProxyElement << type << ";\n";
  if (hasSection)
    str << tspec(false) << " class " << Prefix::ProxySection << type << ";\n";
  if (isPython()) str << tspec(false) << " class " << Prefix::Python << type << ";\n";

  // Generate index class
  str << "/* --------------- index object ------------------ */\n";
  str << tspec() << "class " << Prefix::Index << type;
  str << ":";
  genIndexNames(str, "public ", NULL, "", ", ");
  if (external ||
      type->isTemplated()) {  // Just a template instantiation/forward declaration
    str << ";";
  } else {  // Actual implementation
    str << CIClassStart;
    genTypedefs(str);
    str << "    static int __idx;\n";
    str << "    static void __register(const char *s, size_t size);\n";
    if (list) list->recurse<XStr&>(str, &Member::genIndexDecls);
    str << CIClassEnd;
  }
  str << "/* --------------- element proxy ------------------ */\n";
  generateTramInits = true;
  genSubDecls(str);
  generateTramInits = false;
  if (hasElement) {
    str << "/* ---------------- collective proxy -------------- */\n";
    forElement = forAll;
    genSubDecls(str);
    forElement = forIndividual;
  }
  if (hasSection) {
    str << "/* ---------------- section proxy -------------- */\n";
    forElement = forSection;
    genSubDecls(str);
    forElement = forIndividual;
  }
  if (isPython()) {
    str << "/* ---------------- python wrapper -------------- */\n";
    genPythonDecls(str);
  }

  closuresDecl << "/* ---------------- method closures -------------- */\n";
  closuresDef << closuresDecl;
  genClosureEntryDecls(closuresDecl);
  genClosureEntryDefs(closuresDef);

  if (list) {
    // handle the case that some of the entries may be sdag Entries
    int sdagPresent = 0;
    XStr sdagStr;
    CParsedFile myParsedFile(this);
    SdagCollection sc(&myParsedFile);
    list->recurse(&sc, &Member::collectSdagCode);
    if (sc.sdagPresent) {
      XStr classname;
      XStr sdagDecls;
      classname << baseName(0);
      resetNumbers();
      myParsedFile.doProcess(classname, sdagDecls, sdagDefs);
      str << sdagDecls;
    } else {
      str << "#define " << baseName(0) << "_SDAG_CODE \n";
    }
  }

  // Create CBase_Whatever convenience type so that chare implementations can
  // avoid inheriting from a complex CBaseT templated type.
  XStr CBaseName;
  CBaseName << "CBase_" << type;
  if (isTemplateDeclaration()) {
    templat->genSpec(str);
    str << "\nstruct " << CBaseName << ";\n";
  } else {
    str << "typedef " << cbaseTType() << CBaseName << ";\n";
  }
}

void Chare::preprocess() {
  if (list) {
    list->preprocess();
    list->recurse(this, &Member::setChare);
    list->recursev(&Member::preprocessSDAG);
    // Add migration constructor to MemberList
    if (isMigratable()) {
      Entry* e = new Entry(line, new Attribute(SMIGRATE), NULL,
                          (char *)type->getBaseName(),
                          new ParamList(new Parameter(line,
                              new PtrType(new NamedType("CkMigrateMessage")))), 0, 0, 0);
      e->setChare(this);
      list->push_back(e);
    }
  }

  if (bases == NULL)  // Always add Chare as a base class
    bases = new TypeList(new NamedType("Chare"), NULL);
}

/*This disambiguation code is needed to support
  multiple inheritance in Chares (Groups, Arrays).
  They resolve ambiguous accessor calls to the parent "super".
  Because mutator routines need to change *all* the base
  classes, mutators are generated in xi-symbol.C.
*/
static void disambig_proxy(XStr& str, const XStr& super) {
  str << "\n    int ckIsDelegated(void) const"
      << "\n    { return " << super << "::ckIsDelegated(); }"
      << "\n    inline CkDelegateMgr *ckDelegatedTo(void) const"
      << "\n    { return " << super << "::ckDelegatedTo(); }"
      << "\n    inline CkDelegateData *ckDelegatedPtr(void) const"
      << "\n    { return " << super << "::ckDelegatedPtr(); }"
      << "\n    CkGroupID ckDelegatedIdx(void) const"
      << "\n    { return " << super << "::ckDelegatedIdx(); }"
      << "\n";
}

// This is a separate function becase template chare classes have
// their definition of virtual_pup() generated inline in the
// CBase_foo<T> definition in the matching namespace, while
// non-templates are overridding the definition of CBaseTn<Proxy,
// etc>::virtual_pup() in the global namespace
XStr Chare::virtualPupDef(const XStr& name) {
  XStr str;
  str << "virtual_pup(PUP::er &p) {"
#if CMK_NO_RTTI
      << "\n    recursive_pup<" << name << ">((" << name << " *)this), p);"
#else
      << "\n    recursive_pup<" << name << ">(dynamic_cast<" << name << "*>(this), p);"
#endif
      << "\n}";
  return str;
}

void Chare::genRecursivePup(XStr& scopedName, XStr templateSpec, XStr& decls,
                            XStr& defs) {
  templateGuardBegin(false, defs);

  XStr rec_pup_impl_name, rec_pup_impl_sig, rec_pup_impl_body;
  rec_pup_impl_name << "recursive_pup_impl<" << scopedName << ", 1>";
  rec_pup_impl_sig << "operator()(" << scopedName << " *obj, PUP::er &p)";

  rec_pup_impl_body << rec_pup_impl_sig << " {"
                    << "\n    obj->parent_pup(p);";
  if (hasSdagEntry) rec_pup_impl_body << "\n    obj->_sdag_pup(p);";
  rec_pup_impl_body << "\n    obj->" << scopedName << "::pup(p);"
                    << "\n}\n";

  decls << "\ntemplate <>"
        << "\nvoid " << rec_pup_impl_name << "::" << rec_pup_impl_sig << ";\n";

  defs << templateSpec << "void " << rec_pup_impl_name << "::" << rec_pup_impl_body;

  templateGuardEnd(defs);
}

void Chare::genGlobalCode(XStr scope, XStr& decls, XStr& defs) {
  if (isTemplateInstantiation()) return;

  XStr templatedType;
  templatedType << type;
  if (templat) templat->genVars(templatedType);

  XStr scopedName;
  scopedName << scope << templatedType;

  if (list) list->genTramPups(scope, decls, defs);

  if (!isTemplateDeclaration()) {
    // Leave out ArrayElement because of its funny inheritance
    // structure. It doesn't inherit from CBase_ArrayElement anyway.
    if (0 != strcmp(type->getBaseName(), "ArrayElement")) {
      templateGuardBegin(false, defs);
      defs << "template <>\n"
           << "void " << scope << "CBase_" << baseName(true)
           << "::" << virtualPupDef(scopedName) << "\n";
      templateGuardEnd(defs);
    }
  }
}

void Chare::genClosureEntryDecls(XStr& str) {
  XStr ptype;
  ptype << "Closure_" << type;
  str << tspec() << "class " << ptype << " ";
  str << CIClassStart;
  if (list) list->genClosureEntryDecls(str);
  str << CIClassEnd;
}

void Chare::genClosureEntryDefs(XStr& str) {
  if (list) list->genClosureEntryDefs(str);
}

void Chare::genSubDecls(XStr& str) {
  XStr ptype;
  ptype << proxyPrefix() << type;

  // Class declaration
  str << tspec() << "class " << ptype;
  if (external || type->isTemplated()) {
    str << ";";
    return;
  }
  str << ":";
  genProxyNames(str, "public ", NULL, "", ", ");
  str << CIClassStart;

  genTypedefs(str);

  // Various constructors:
  str << "    " << ptype << "(void) {};\n";

  str << "    " << ptype << "(CkChareID __cid) : ";
  genProxyNames(str, "", NULL, "(__cid)", ", ");
  str << "{  }\n";

  str << "    " << ptype << "(const Chare *c) : ";
  genProxyNames(str, "", NULL, "(c)", ", ");
  str << "{  }\n";

  // Multiple inheritance-- resolve inheritance ambiguity
  XStr super;
  bases->getFirst()->genProxyName(super, forElement);
  disambig_proxy(str, super);
  str << "\n    inline void ckCheck(void) const"
      << "\n    { " << super << "::ckCheck(); }"
      << "\n    const CkChareID &ckGetChareID(void) const"
      << "\n    { return " << super << "::ckGetChareID(); }"
      << "\n    operator const CkChareID &(void) const"
      << "\n    { return ckGetChareID(); }"
      << "\n";

  sharedDisambiguation(str, super);
  str << "\n    void ckSetChareID(const CkChareID &c)"
      << "\n    {";
  genProxyNames(str, "      ", NULL, "::ckSetChareID(c); ", "");
  str << "}"
      << "\n    " << type << tvars() << " *ckLocal(void) const"
      << "\n    { return (" << type << tvars() << " *)CkLocalChare(&ckGetChareID()); }"
      << "\n";

  genMemberDecls(str);
  str << CIClassEnd;
}

void Chare::genMemberDecls(XStr& str) {
  if (list) list->genDecls(str);
}

void Chare::genPythonDecls(XStr& str) {
  XStr ptype;
  ptype << Prefix::Python << type;

  // Class declaration
  str << tspec() << "class " << ptype;
  if (external || type->isTemplated()) {
    str << ";";
    return;
  }
  str << ":";
  TypeList* b = bases_CBase;
  if (b == NULL) b = bases;  // Fall back to normal bases list if no CBase available
  b->genProxyNames(str, "public ", NULL, "", ", ", forPython);
  str << ", public PythonObject ";
  str << CIClassStart;

  // default constructor methods
  str << "    " << ptype << "(void) {}\n";
  str << "    " << ptype << "(CkMigrateMessage *msg): ";
  b->genProxyNames(str, "", NULL, "(msg)", ", ", forPython);
  str << " {}\n";

  // define pupper
  str << "    void pup(PUP::er &p) {\n";
  b->genProxyNames(str, "      ", NULL, "::pup(p);", "\n", forPython);
  str << "\n    }\n";

  // define the python custom methods and their documentation
  str << "    static PyMethodDef CkPy_MethodsCustom[];\n";
  str << "    PyMethodDef *getMethods(void) {return CkPy_MethodsCustom;}\n";
  str << "    static const char *CkPy_MethodsCustomDoc;\n";
  str << "    const char *getMethodsDoc(void) {return CkPy_MethodsCustomDoc;}\n";

  str << CIClassEnd;

  // declare all static python methods and CkPy_MethodsCustom
  if (list) list->recurse<XStr&>(str, &Member::genPythonDecls);
  str << "\n";
}

void Chare::genPythonDefs(XStr& str) {
  XStr ptype;
  ptype << Prefix::Python << type;

  // generate the python methods array
  str << "PyMethodDef " << ptype << "::CkPy_MethodsCustom[] = {\n";
  if (list) list->recurse<XStr&>(str, &Member::genPythonStaticDefs);
  str << "  {NULL, NULL}\n};\n\n";
  // generate documentaion for the methods
  str << "const char * " << ptype
      << "::CkPy_MethodsCustomDoc = \"charm.__doc__ = \\\"Available methods for object "
      << type << ":\\\\n\"";
  if (list) list->recurse<XStr&>(str, &Member::genPythonStaticDocs);
  str << "\n  \"\\\"\";\n\n";

  if (list) list->recurse<XStr&>(str, &Member::genPythonDefs);
}

void Chare::printChareNames() { cout << baseName(0) << endl; }

XStr Chare::proxyName(int withTemplates) {
  XStr str;
  str << proxyPrefix() << type;
  if (withTemplates) str << tvars();
  return str;
}

XStr Chare::indexName(int withTemplates) {
  XStr str;
  str << Prefix::Index << type;
  if (withTemplates) str << tvars();
  return str;
}

// only for chare arrays and groups
XStr Chare::sectionName(int withTemplates) {
  XStr str;
  str << Prefix::ProxySection << type;
  if (withTemplates) str << tvars();
  return str;
}

XStr Chare::indexList() {
  // generating "int *index1, int *index2, int *index3"
  XStr str;
  if (!isArray()) {  // Currently, only arrays are supported
    cerr << (char*)baseName() << ": only chare arrays are currently supported\n";
    exit(1);
  }
  XStr dim = ((Array*)this)->dim();
  if (dim == (const char*)"1D")
    str << "const int *index1";
  else if (dim == (const char*)"2D")
    str << "const int *index1, const int *index2";
  else if (dim == (const char*)"3D")
    str << "const int *index1, const int *index2, const int *index3";
  else {
    cerr << (char*)baseName()
         << ": only up to 3 dimension chare arrays are currently supported\n";
    exit(1);
  }
  return str;
}

void Chare::genProxyNamesExceptFirst(XStr& str, const char* prefix, const char* middle,
                                     const char* suffix, const char* sep) {
  TypeList* base2 = bases->next;
  if (base2 != NULL) base2->genProxyNames(str, prefix, middle, suffix, sep, forElement);
}

void Chare::genProxyNames(XStr& str, const char* prefix, const char* middle,
                          const char* suffix, const char* sep) {
  bases->genProxyNames(str, prefix, middle, suffix, sep, forElement);
}

void Chare::genIndexNames(XStr& str, const char* prefix, const char* middle,
                          const char* suffix, const char* sep) {
  bases->genProxyNames(str, prefix, middle, suffix, sep, forIndex);
}

char* Chare::proxyPrefix(void) { return (char*)forWhomStr(forElement); }

// Common multiple inheritance disambiguation code
void Chare::sharedDisambiguation(XStr& str, const XStr& super) {
  (void)super;
  str << "\n    void ckDelegate(CkDelegateMgr *dTo,CkDelegateData *dPtr=NULL)"
      << "\n    { ";
  genProxyNames(str, "      ", NULL, "::ckDelegate(dTo,dPtr); ", "");
  str << "}"
      << "\n    void ckUndelegate(void)"
      << "\n    { ";
  genProxyNames(str, "      ", NULL, "::ckUndelegate(); ", "");
  str << "}"
      << "\n    void pup(PUP::er &p)"
      << "\n    { ";
  genProxyNames(str, "      ", NULL, "::pup(p);\n", "");
  if (isForElement() && !tramInstances.empty()) {
    str << "      if (p.isUnpacking()) {\n";
    for (int i = 0; i < tramInstances.size(); i++) {
      str << "        " << tramInstances[i].name.c_str() << " = NULL;\n";
    }
    str << "      }\n";
  }
  str << "    }";
  if (isPython()) {
    str << "\n    void registerPython(const char *str)"
        << "\n    { CcsRegisterHandler(str, CkCallback(" << Prefix::Index << type
        << "::pyRequest(0), *this)); }";
  }
  str << "\n";
}

void Chare::genTypedefs(XStr& str) {
  str << "    typedef " << baseName(1) << " local_t;\n";
  str << "    typedef " << Prefix::Index << baseName(1) << " index_t;\n";
  str << "    typedef " << Prefix::Proxy << baseName(1) << " proxy_t;\n";

  if (hasElement)
    str << "    typedef " << Prefix::ProxyElement << baseName(1) << " element_t;\n";
  else /* !hasElement, so generic proxy is element type */
    str << "    typedef " << Prefix::Proxy << baseName(1) << " element_t;\n";

  if (hasSection)
    str << "    typedef " << Prefix::ProxySection << baseName(1) << " section_t;\n";
  str << "\n";
}

void Chare::genDefs(XStr& str) {
  str << "/* DEFS: ";
  print(str);
  str << " */\n";
  if (fortranMode) {   // For Fortran90
    if (!isArray()) {  // Currently, only arrays are supported
      cerr << (char*)baseName() << ": only chare arrays are currently supported\n";
      exit(1);
    }
    // We have to generate the chare array itself
    str << "/* FORTRAN */\n";
    str << "extern \"C\" void " << fortranify(baseName(), "_allocate")
        << "(char **, void *, " << indexList() << ");\n";
    str << "extern \"C\" void " << fortranify(baseName(), "_pup")
        << "(pup_er p, char **, void *);\n";
    str << "extern \"C\" void " << fortranify(baseName(), "_resumefromsync")
        << "(char **, void *, " << indexList() << ");\n";
    str << "\n";
    XStr dim = ((Array*)this)->dim();
    str << "class " << baseName() << " : public CBase_" << baseName() << "\n";
    str << "{\n";
    if (hasSdagEntry)
      str << "  " << baseName() << "_SDAG_CODE\n";
    str << "public:\n";
    str << "  char user_data[64];\n";
    str << "public:\n";
    str << "  " << baseName() << "()\n";
    str << "  {\n";
    //    str << "    CkPrintf(\"" << baseName() << " %d created\\n\",thisIndex);\n";
    str << "    CkArrayID *aid = &thisArrayID;\n";
    if (dim == (const char*)"1D")
      str << "    " << fortranify(baseName(), "_allocate")
          << "((char **)&user_data, &aid, &thisIndex);\n";
    else if (dim == (const char*)"2D")
      str << "    " << fortranify(baseName(), "_allocate")
          << "((char **)&user_data, &aid, &thisIndex.x, &thisIndex.y);\n";
    else if (dim == (const char*)"3D")
      str << "    " << fortranify(baseName(), "_allocate")
          << "((char **)&user_data, &aid, &thisIndex.x, &thisIndex.y, &thisIndex.z);\n";
    str << "      usesAtSync = true;\n";
    str << "  }\n";
    str << "\n";
    str << "  " << baseName() << "(CkMigrateMessage *m)\n";
    str << "  {\n";
    str << "    /* CkPrintf(\"" << baseName() << " %d migrating\\n\",thisIndex);*/ \n";
    str << "  }\n";
    str << "\n";

    str << "  virtual void pup(PUP::er &p)\n";
    str << "  {\n";
    str << "    p(user_data, 64);\n";
    str << "    CkArrayID *aid = &thisArrayID;\n";
    str << "    ::" << fortranify(baseName(), "_pup")
        << "(&p, (char **)&user_data, &aid); \n";
    str << "  }\n";
    str << "\n";

    // Define the Fortran interface function for ResumeFromSync
    str << "  void ResumeFromSync()\n";
    str << "  {\n";
    str << "    CkArrayID *aid = &thisArrayID;\n";
    str << "    ::" << fortranify(baseName(), "_resumefromSync");
    if (dim == (const char*)"1D") {
      str << "((char **)&user_data, &aid, &thisIndex);\n";
    } else if (dim == (const char*)"2D") {
      str << "((char **)&user_data, &aid, &thisIndex.x, &thisIndex.y);\n";
    } else if (dim == (const char*)"3D") {
      str << "((char **)&user_data, &aid, &thisIndex.x, &thisIndex.y, &thisIndex.z);\n";
    }
    str << "  }\n";

    str << "};\n";
    str << "\n";
    if (dim == (const char*)"1D") {
      str << "extern \"C\" void " << fortranify(baseName(), "_cknew")
          << "(int *numElem, void **aindex)\n";
      str << "{\n";
      str << "    CkArrayID *aid = new CkArrayID;\n";
      str << "    *aid = CProxy_" << baseName() << "::ckNew(*numElem); \n";
    } else if (dim == (const char*)"2D") {
      str << "extern \"C\" void " << fortranify(baseName(), "_cknew")
          << "(int *numx, int *numy, void **aindex)\n";
      str << "{\n";
      str << "    CkArrayID *aid = new CkArrayID;\n";
      str << "    *aid = CProxy_" << baseName() << "::ckNew(); \n";
      str << "    CProxy_" << baseName() << " p(*aid);\n";
      str << "    for (int i=0; i<*numx; i++) \n";
      str << "      for (int j=0; j<*numy; j++) \n";
      str << "        p[CkArrayIndex2D(i, j)].insert(); \n";
      str << "    p.doneInserting(); \n";
    } else if (dim == (const char*)"3D") {
      str << "extern \"C\" void " << fortranify(baseName(), "_cknew")
          << "(int *numx, int *numy, int *numz, void **aindex)\n";
      str << "{\n";
      str << "    CkArrayID *aid = new CkArrayID;\n";
      str << "    *aid = CProxy_" << baseName() << "::ckNew(); \n";
      str << "    CProxy_" << baseName() << " p(*aid);\n";
      str << "    for (int i=0; i<*numx; i++) \n";
      str << "      for (int j=0; j<*numy; j++) \n";
      str << "        for (int k=0; k<*numz; k++) \n";
      str << "          p[CkArrayIndex3D(i, j, k)].insert(); \n";
      str << "    p.doneInserting(); \n";
    }
    str << "    *aindex = (void*)aid;\n";
    str << "}\n";

    // Define the Fortran interface function for AtSync
    if (dim == (const char*)"1D") {
      str << "extern \"C\" void " << fortranify(baseName(), "_atsync")
          << "(void** aindex, int *index1)\n";
      str << "{\n";
      str << "  CkArrayID *aid = (CkArrayID *)*aindex;\n";
      str << "\n";
      str << "  CProxy_" << baseName() << " h(*aid);\n";
      str << "  h[*index1].ckLocal()->AtSync();\n";
    } else if (dim == (const char*)"2D") {
      str << "extern \"C\" void " << fortranify(baseName(), "_atsync")
          << "(void** aindex, int *index1, int *index2)\n";
      str << "{\n";
      str << "  CkArrayID *aid = (CkArrayID *)*aindex;\n";
      str << "\n";
      str << "  CProxy_" << baseName() << " h(*aid);\n";
      str << "  h[CkArrayIndex2D(*index1, *index2)].ckLocal()->AtSync();\n";
    } else if (dim == (const char*)"3D") {
      str << "extern \"C\" void " << fortranify(baseName(), "_atsync")
          << "(void** aindex, int *index1, int *index2, int *index3)\n";
      str << "{\n";
      str << "  CkArrayID *aid = (CkArrayID *)*aindex;\n";
      str << "\n";
      str << "  CProxy_" << baseName() << " h(*aid);\n";
      str << "  h[CkArrayIndex3D(*index1, *index2, *index3)].ckLocal()->AtSync();\n";
    }
    str << "}\n";

    // define reduction interface function
    if (dim == (const char*)"1D") {
      str << "extern \"C\" void "
          << fortranify(baseName(), "_contribute")
          << "(void** aindex, int *index1, int *size, void *data, int *op, int (*target)())\n";
      str << "{\n";
      str << "  CkArrayID *aid = (CkArrayID *)*aindex;\n";
      str << "  CProxy_" << baseName() << " h(*aid);\n";
      str << "  h[*index1].ckLocal()->contribute(*size, data, CkReduction::reducerType(*op), CkCallback(target(), h[0]));\n";
    }
    else if (dim == (const char*)"2D") {
      str << "extern \"C\" void "
          << fortranify(baseName(), "_contribute")
          << "(void** aindex, int *index1, int *index2, int *size, void *data, int *op, int (*target)())\n";
      str << "{\n";
      str << "  CkArrayID *aid = (CkArrayID *)*aindex;\n";
      str << "  CProxy_" << baseName() << " h(*aid);\n";
      str << "  h(*index1, *index2).ckLocal()->contribute(*size, data, CkReduction::reducerType(*op), CkCallback(target(), h(0, 0)));\n";
    }
    else if (dim == (const char*)"3D") {
      str << "extern \"C\" void "
          << fortranify(baseName(), "_contribute")
          << "(void** aindex, int *index1, int *index2, int *index3, int *size, void *data, int *op, int (*target)())\n";
      str << "{\n";
      str << "  CkArrayID *aid = (CkArrayID *)*aindex;\n";
      str << "  CProxy_" << baseName() << " h(*aid);\n";
      str << "  h(*index1, *index2, *index3).ckLocal()->contribute(*size, data, CkReduction::reducerType(*op), CkCallback(target(), h(0, 0, 0)));\n";
    }
    str << "}\n";

    str << "/* FORTRAN END */\n\n";
  } // fortranMode

  templateGuardBegin(isTemplated(), str);
  if (!type->isTemplated()) {
    if (external) str << "extern ";
    str << tspec(false) << " int " << indexName() << "::__idx";
    if (!external) str << "=0";
    str << ";\n";
  }
  templateGuardEnd(str);

  templateGuardBegin(isTemplated(), str);
  if (isArray() && hasSection && !isTemplateInstantiation()) {
    if (isTemplated()) str << tspec(false) << "\n";
    str << "void " << sectionName()
        << "::contribute(CkSectionInfo &sid, int userData, int fragSize)\n";
    str << "{\n";
    str << "   CkArray *ckarr = CProxy_CkArray(sid.get_aid()).ckLocalBranch();\n";
    str << "   CkMulticastMgr *mCastGrp = "
           "CProxy_CkMulticastMgr(ckarr->getmCastMgr()).ckLocalBranch();\n";
    str << "   mCastGrp->contribute(sid, userData, fragSize);\n";
    str << "}\n\n";

    if (isTemplated()) str << tspec(false) << "\n";
    str << "void " << sectionName()
        << "::contribute(int dataSize,void *data,CkReduction::reducerType type, "
           "CkSectionInfo &sid, int userData, int fragSize)\n";
    str << "{\n";
    str << "   CkArray *ckarr = CProxy_CkArray(sid.get_aid()).ckLocalBranch();\n";
    str << "   CkMulticastMgr *mCastGrp = "
           "CProxy_CkMulticastMgr(ckarr->getmCastMgr()).ckLocalBranch();\n";
    str << "   mCastGrp->contribute(dataSize, data, type, sid, userData, fragSize);\n";
    str << "}\n\n";

    if (isTemplated()) str << tspec(false) << "\n";
    str << "template <typename T>\n";
    str << "void " << sectionName()
        << "::contribute(std::vector<T> &data, CkReduction::reducerType type, "
           "CkSectionInfo &sid, int userData, int fragSize)\n";
    str << "{\n";
    str << "   CkArray *ckarr = CProxy_CkArray(sid.get_aid()).ckLocalBranch();\n";
    str << "   CkMulticastMgr *mCastGrp = "
           "CProxy_CkMulticastMgr(ckarr->getmCastMgr()).ckLocalBranch();\n";
    str << "   mCastGrp->contribute(data, type, sid, userData, fragSize);\n";
    str << "}\n\n";

    if (isTemplated()) str << tspec(false) << "\n";
    str << "void " << sectionName()
        << "::contribute(CkSectionInfo &sid, const CkCallback &cb, int userData, int fragSize)\n";
    str << "{\n";
    str << "   CkArray *ckarr = CProxy_CkArray(sid.get_aid()).ckLocalBranch();\n";
    str << "   CkMulticastMgr *mCastGrp = "
           "CProxy_CkMulticastMgr(ckarr->getmCastMgr()).ckLocalBranch();\n";
    str << "   mCastGrp->contribute(sid, cb, userData, fragSize);\n";
    str << "}\n\n";

    if (isTemplated()) str << tspec(false) << "\n";
    str << "void " << sectionName()
        << "::contribute(int dataSize,void *data,CkReduction::reducerType type, "
           "CkSectionInfo &sid, const CkCallback &cb, int userData, int fragSize)\n";
    str << "{\n";
    str << "   CkArray *ckarr = CProxy_CkArray(sid.get_aid()).ckLocalBranch();\n";
    str << "   CkMulticastMgr *mCastGrp = "
           "CProxy_CkMulticastMgr(ckarr->getmCastMgr()).ckLocalBranch();\n";
    str << "   mCastGrp->contribute(dataSize, data, type, sid, cb, userData, "
           "fragSize);\n";
    str << "}\n\n";

    if (isTemplated()) str << tspec(false) << "\n";
    str << "template <typename T>\n";
    str << "void " << sectionName()
        << "::contribute(std::vector<T> &data, CkReduction::reducerType type, "
           "CkSectionInfo &sid, const CkCallback &cb, int userData, int fragSize)\n";
    str << "{\n";
    str << "   CkArray *ckarr = CProxy_CkArray(sid.get_aid()).ckLocalBranch();\n";
    str << "   CkMulticastMgr *mCastGrp = "
           "CProxy_CkMulticastMgr(ckarr->getmCastMgr()).ckLocalBranch();\n";
    str << "   mCastGrp->contribute(data, type, sid, cb, userData, fragSize);\n";
    str << "}\n\n";
  }
  templateGuardEnd(str);

  if (list) {  // Add definitions for all entry points
    list->genDefs(str);
    if (hasElement) {  // Define the entry points for the element
      forElement = forAll;
      list->genDefs(str);
      if (hasSection) {  // for Section
        forElement = forSection;
        list->genDefs(str);
      }
      forElement = forIndividual;
    }
  }

  templateGuardBegin(isTemplated(), str);
  // define the python routines
  if (isPython()) {
    str << "/* ---------------- python wrapper -------------- */\n";

    // write CkPy_MethodsCustom
    genPythonDefs(str);
  }
  templateGuardEnd(str);

  if (!external && !type->isTemplated()) genRegisterMethodDef(str);
  if (hasSdagEntry) {
    str << "\n";
    str << sdagDefs;
  }

  XStr templateSpec;
  if (templat)
    templat->genSpec(templateSpec, false);
  else
    templateSpec << "template <>";
  templateSpec << "\n";

  if (isTemplateDeclaration()) {
    templateGuardBegin(true, str);

    TypeList* b = bases_CBase;
    if (b == NULL) b = bases;  // Fall back to normal bases list if no CBase available

    XStr baseClass;
    if (isPython()) {
      baseClass << Prefix::Python << type;
    } else {
      baseClass << b;
    }

    XStr CBaseName;
    CBaseName << "CBase_" << type;

    str << templateSpec << "struct " << CBaseName << " : public " << baseClass
        << ", virtual CBase";
    str << "\n";
    str << " {"
        << "\n  CProxy_" << type << tvars() << " thisProxy;";

    /***** Constructors *****/
    // Default constructor
    str << "\n  " << CBaseName << "() : thisProxy(this)";
    for (TypeList* t = b; t; t = t->next) {
      str << "\n    , " << t->type << "()";
    }
    str << "\n  { }";

    // Migration constructor
    str << "\n  " << CBaseName << "(CkMigrateMessage* m) : thisProxy(this)";
    for (TypeList* t = b; t; t = t->next) {
      str << "\n    , " << t->type << "(m)";
    }
    str << "  { }";

    // Constructor(s) with user-defined arguments to pass to parent class
    if (b->length() == 1) {
      str << "\n  template <typename... Args>"
          << "\n  " << CBaseName << "(Args... args) : thisProxy(this)";
      for (TypeList* t = b; t; t = t->next) {
        str << "\n    , " << t->type << "(args...)";
      }
      str << "  { }";
    }

    // PUP-related methods
    str << "\n  void pup(PUP::er &p) { }"
        << "\n  void _sdag_pup(PUP::er &p) { }"
        << "\n  void " << virtualPupDef(baseName(true))
        << "\n  void parent_pup(PUP::er &p) {";
    for (TypeList* t = b; t; t = t->next) {
      str << "\n    recursive_pup<" << t->type << ">(this, p);";
    }
    str << "\n    p|thisProxy;"
        << "\n  }"
        << "\n};\n";

    templateGuardEnd(str);
  }
}

XStr Chare::cbaseTType() {
  TypeList* b = bases_CBase;
  if (b == NULL) b = bases;  // Fall back to normal bases list if no CBase available

  XStr baseClass;
  if (isPython()) {
    baseClass << Prefix::Python << type;
  } else {
    baseClass << b;
  }

  XStr templatedType;
  templatedType << type;
  if (isTemplateDeclaration()) templat->genVars(templatedType);

  XStr CBaseT_type;
  CBaseT_type << "CBaseT" << b->length() << "<" << baseClass << ", CProxy_"
              << templatedType << ">";
  return CBaseT_type;
}

void Chare::genReg(XStr& str) {
  str << "/* REG: ";
  print(str);
  str << "*/\n";
  if (external || templat) return;
  str << "  " << indexName() << "::__register(\"" << type << "\", sizeof(" << type
      << "));\n";

  if (list) {
    list->genTramRegs(str);
  }
}

void Chare::print(XStr& str) {
  if (external) str << "extern ";
  if (templat) templat->genSpec(str);

  str << chareTypeName() << " " << type;
  if (bases) {
    str << ": ";
    bases->print(str);
  }
  if (list) {
    str << "{\n";
    list->print(str);
    str << "};\n";
  } else {
    str << ";\n";
  }
}

void Chare::lookforCEntry(CEntry* centry) {
  if (list) list->recurse(centry, &Member::lookforCEntry);
}

bool Chare::isTramTarget() {
  if (list)
    return list->isTramTarget();
  else
    return false;
}

void Chare::genTramTypes() {
  if (list) {
    list->genTramTypes();
  }
}

void Chare::genTramDecls(XStr& str) {
  if (isForElement()) {
    str << "\n    /* TRAM aggregators */\n";
    for (int i = 0; i < tramInstances.size(); i++) {
      str << "    " << tramInstances[i].type.c_str() << "* "
          << tramInstances[i].name.c_str() << ";\n";
    }
    str << "\n";
  }
}

void Chare::genTramInits(XStr& str) {
  if (generateTramInits) {
    for (int i = 0; i < tramInstances.size(); i++) {
      str << "      " << tramInstances[i].name.c_str() << " = NULL;\n";
    }
  }
}

Group::Group(int ln, attrib_t Nattr, NamedType* t, TypeList* b, AstChildren<Member>* l)
    : Chare(ln, Nattr | CGROUP, t, b, l) {
  hasElement = 1;
  forElement = forIndividual;
  hasSection = 1;
  bases_CBase = NULL;
  if (b == NULL) {
    // Add Group as a base class
    delete bases;
    if (isNodeGroup()) {
      bases = new TypeList(new NamedType("NodeGroup"), NULL);
    } else {
      bases = new TypeList(new NamedType("IrrGroup"), NULL);
      bases_CBase = new TypeList(new NamedType("Group"), NULL);
    }
  }
}

void Group::genSubRegisterMethodDef(XStr& str) {
  if (!isTemplated()) {
    str << "   CkRegisterGroupIrr(__idx," << type << "::isIrreducible());\n";
  } else {
    str << "   CkRegisterGroupIrr(__idx," << type << tvars() << "::isIrreducible());\n";
  }
}

void Group::genSubDecls(XStr& str) {
  XStr ptype;
  ptype << proxyPrefix() << type;
  XStr ttype;
  ttype << type << tvars();
  XStr super;
  bases->getFirst()->genProxyName(super, forElement);

  // Class declaration:
  str << tspec() << "class " << ptype;
  if (external || type->isTemplated()) {
    str << ";";
    return;
  }
  str << ": ";

  genProxyNames(str, "public ", NULL, "", ", ");
  str << CIClassStart;

  genTypedefs(str);

  genTramDecls(str);

  XStr constructorBody;
  constructorBody << "{\n";
  genTramInits(constructorBody);
  constructorBody << "    }\n";

  // Basic constructors:
  str << "    " << ptype << "(void) " << constructorBody;
  str << "    " << ptype << "(const IrrGroup *g) : ";
  genProxyNames(str, "", NULL, "(g)", ", ");
  str << constructorBody;

  if (forElement == forIndividual) {  // For a single element
    str << "    " << ptype << "(CkGroupID _gid,int _onPE,CK_DELCTOR_PARAM) : ";
    genProxyNames(str, "", NULL, "(_gid,_onPE,CK_DELCTOR_ARGS)", ", ");
    str << constructorBody;
    str << "    " << ptype << "(CkGroupID _gid,int _onPE) : ";
    genProxyNames(str, "", NULL, "(_gid,_onPE)", ", ");
    str << constructorBody;

    disambig_group(str, super);
    str << "int ckGetGroupPe(void) const\n"
        << "{return " << super << "::ckGetGroupPe();}\n";

  } else if (forElement == forSection) {  // For a section of the group
    str << "    " << ptype
        << "(const CkGroupID &_gid,const int *_pelist,int _npes, CK_DELCTOR_PARAM) : ";
    genProxyNames(str, "", NULL, "(_gid,_pelist,_npes,CK_DELCTOR_ARGS)", ", ");
    str << "{  }\n";
    str << "    " << ptype
        << "(const CkGroupID &_gid,const int *_pelist,int _npes, int factor = "
           "USE_DEFAULT_BRANCH_FACTOR) : ";
    genProxyNames(str, "", NULL, "(_gid,_pelist,_npes,factor)", ", ");
    str << "{  }\n";
    str << "    " << ptype
        << "(int n,const CkGroupID *_gid, int const * const *_pelist,const int *_npes, "
           "int factor = USE_DEFAULT_BRANCH_FACTOR) : ";
    genProxyNames(str, "", NULL, "(n,_gid,_pelist,_npes,factor)", ", ");
    str << "{  }\n";
    str << "    " << ptype
        << "(int n,const CkGroupID *_gid, int const * const *_pelist,const int *_npes, "
           "CK_DELCTOR_PARAM) : ";
    genProxyNames(str, "", NULL, "(n,_gid,_pelist,_npes,CK_DELCTOR_ARGS)", ", ");
    str << "{  }\n";

    disambig_group(str, super);
    str << "inline int ckGetNumSections() const\n"
        << "{ return " << super << "::ckGetNumSections(); }\n"
        << "inline CkSectionInfo &ckGetSectionInfo()\n"
        << "{ return " << super << "::ckGetSectionInfo(); }\n"
        << "inline CkSectionID *ckGetSectionIDs()\n"
        << "{ return " << super << "::ckGetSectionIDs(); }\n"
        << "inline CkSectionID &ckGetSectionID()\n"
        << "{ return " << super << "::ckGetSectionID(); }\n"
        << "inline CkSectionID &ckGetSectionID(int i)\n"
        << "{ return " << super << "::ckGetSectionID(i); }\n"
        << "inline CkGroupID ckGetGroupIDn(int i) const\n"
        << "{ return " << super << "::ckGetGroupIDn(i); }\n"
        << "inline const int *ckGetElements() const\n"
        << "{ return " << super << "::ckGetElements(); }\n"
        << "inline const int *ckGetElements(int i) const\n"
        << "{ return " << super << "::ckGetElements(i); }\n"
        << "inline int ckGetNumElements() const\n"
        << "{ return " << super << "::ckGetNumElements(); } \n"
        << "inline int ckGetNumElements(int i) const\n"
        << "{ return " << super << "::ckGetNumElements(i); }\n";
  } else if (forElement == forAll) {  // For whole group
    str << "    " << ptype << "(CkGroupID _gid,CK_DELCTOR_PARAM) : ";
    genProxyNames(str, "", NULL, "(_gid,CK_DELCTOR_ARGS)", ", ");
    str << "{  }\n";
    str << "    " << ptype << "(CkGroupID _gid) : ";
    genProxyNames(str, "", NULL, "(_gid)", ", ");
    str << "{  }\n";

    // Group proxy can be indexed into an element proxy:
    forElement = forIndividual;  //<- for the proxyName below
    str << "    " << proxyName(1) << " operator[](int onPE) const\n";
    str << "      {return " << proxyName(1)
        << "(ckGetGroupID(),onPE,CK_DELCTOR_CALL);}\n";
    forElement = forAll;

    disambig_group(str, super);
  }

  // Multiple inheritance-- resolve inheritance ambiguity
  sharedDisambiguation(str, super);
  str << "    void ckSetGroupID(CkGroupID g) {\n";
  genProxyNames(str, "      ", NULL, "::ckSetGroupID(g);\n", "");
  str << "    }\n";

  str << "    " << ttype << "* ckLocalBranch(void) const {\n";
  str << "      return ckLocalBranch(ckGetGroupID());\n";
  str << "    }\n";
  str << "    static " << ttype << "* ckLocalBranch(CkGroupID gID) {\n";
  str << "      return (" << ttype << "*)";
  if (isNodeGroup())
    str << "CkLocalNodeBranch(gID);\n";
  else
    str << "CkLocalBranch(gID);\n";
  str << "    }\n";
  genMemberDecls(str);
  str << CIClassEnd;
}
//
// Array Constructor
Array::Array(int ln, attrib_t Nattr, NamedType* index, NamedType* t, TypeList* b,
             AstChildren<Member>* l)
    : Chare(ln, Nattr | CARRAY | CMIGRATABLE, t, b, l), hasVoidConstructor(false) {
  hasElement = 1;
  forElement = forIndividual;
  hasSection = 1;
  index->print(indexSuffix);
  // printf("indexSuffix = %s\n", indexSuffix.charstar());
  if (indexSuffix != (const char*)"none")
    indexType << "CkArrayIndex" << indexSuffix;
  else
    indexType << "CkArrayIndex";

  if (b == 0) {  // No other base class:
    delete bases;
    if (0 == strcmp(type->getBaseName(), "ArrayElement"))
      // ArrayElement has special "ArrayBase" superclass
      bases = new TypeList(new NamedType("ArrayBase"), NULL);
    else {  // Everybody else inherits from ArrayElementT<indexType>
      bases = new TypeList(new NamedType("ArrayElement"), NULL);
      XStr indexObject(indexSuffix2object(indexSuffix));
      XStr parentClass;
      parentClass << "ArrayElementT<" << indexObject << ">";
      char* parentClassName = strdup(parentClass);
      bases_CBase = new TypeList(new NamedType(parentClassName), NULL);
    }
  }
  // cppcheck-suppress memleak
}

void Array::genSubDecls(XStr& str) {
  XStr ptype;
  ptype << proxyPrefix() << type;

  // Class declaration:
  str << tspec() << " class " << ptype;
  if (external || type->isTemplated()) {
    str << ";";
    return;
  }
  str << " : ";
  genProxyNames(str, "public ", NULL, "", ", ");
  str << CIClassStart;

  genTypedefs(str);
  str << "    using array_index_t = " << indexType << ";\n";

  genTramDecls(str);

  XStr constructorBody;
  constructorBody << "{\n";
  genTramInits(constructorBody);
  constructorBody << "    }\n";

  str << "    " << ptype << "(void) " << constructorBody;  // An empty constructor
  if (forElement != forSection) {  // Generate constructor based on array element
    str << "    " << ptype << "(const ArrayElement *e) : ";
    genProxyNames(str, "", NULL, "(e)", ", ");
    str << constructorBody;
  }

  // Resolve multiple inheritance ambiguity
  XStr super;
  bases->getFirst()->genProxyName(super, forElement);
  sharedDisambiguation(str, super);

  if (forElement == forIndividual) { /*For an individual element (no indexing)*/
    disambig_array(str, super);
    str << "\n    inline void ckInsert(CkArrayMessage *m,int ctor,int onPe)"
        << "\n    { " << super << "::ckInsert(m,ctor,onPe); }"
        << "\n    inline void ckSend(CkArrayMessage *m, int ep, int opts = 0) const"
        << "\n    { " << super << "::ckSend(m,ep,opts); }"
        << "\n    inline void *ckSendSync(CkArrayMessage *m, int ep) const"
        << "\n    { return " << super << "::ckSendSync(m,ep); }"
        << "\n    inline const CkArrayIndex &ckGetIndex() const"
        << "\n    { return " << super << "::ckGetIndex(); }"
        << "\n"
        << "\n    " << type << tvars() << " *ckLocal(void) const"
        << "\n    { return (" << type << tvars() << " *)" << super << "::ckLocal(); }"
        << "\n";

    // This constructor is used for array indexing
    str << "\n    " << ptype << "(const CkArrayID &aid,const " << indexType
        << " &idx,CK_DELCTOR_PARAM)"
        << "\n        :";
    genProxyNames(str, "", NULL, "(aid,idx,CK_DELCTOR_ARGS)", ", ");
    str << "\n    {\n";
    genTramInits(str);
    str << "}"
        << "\n    " << ptype << "(const CkArrayID &aid,const " << indexType << " &idx)"
        << "\n        :";
    genProxyNames(str, "", NULL, "(aid,idx)", ", ");
    str << "\n    {\n";
    genTramInits(str);
    str << "}"
        << "\n";

    if ((indexType != (const char*)"CkArrayIndex") &&
        (indexType != (const char*)"CkArrayIndexMax")) {
      // Emit constructors that take the base class array index too.  This proves
      // useful for runtime code that needs to access an element via a CkArrayIndex and
      // an array proxy. This might compromise type safety a wee bit and is hence not
      // propagated throughout.  For eg, CProxy_Foo::operator[] still accepts only the
      // appropriate CkArrayIndexND.
      str << "\n    " << ptype
          << "(const CkArrayID &aid,const CkArrayIndex &idx,CK_DELCTOR_PARAM)"
          << "\n        :";
      genProxyNames(str, "", NULL, "(aid,idx,CK_DELCTOR_ARGS)", ", ");
      str << "\n    {\n";
      genTramInits(str);
      str << "}"
          << "\n    " << ptype << "(const CkArrayID &aid,const CkArrayIndex &idx)"
          << "\n        :";
      genProxyNames(str, "", NULL, "(aid,idx)", ", ");
      str << "\n    {\n";
      genTramInits(str);
      str << "}"
          << "\n";
    }
  } else if (forElement == forAll) { /*Collective, indexible version*/
    disambig_array(str, super);

    // If there is a void constructor, the code that it generates covers empty
    // array construction, and would produce a conflicting overload if we still
    // emitted this.
    if (!hasVoidConstructor) {
      str << "\n    // Empty array construction";
      str << "\n    static CkArrayID ckNew(CkArrayOptions opts = CkArrayOptions()) { "
             "return ckCreateEmptyArray(opts); }";
      str << "\n    static void      ckNew(CkCallback cb, CkArrayOptions opts = "
             "CkArrayOptions()) { ckCreateEmptyArrayAsync(cb, opts); }\n";
    }

    XStr etype;
    etype << Prefix::ProxyElement << type << tvars();
    if (indexSuffix != (const char*)"none") {
      str << "\n    // Generalized array indexing:"
          << "\n    " << etype << " operator [] (const " << indexType << " &idx) const"
          << "\n    { return " << etype << "(ckGetArrayID(), idx, CK_DELCTOR_CALL); }"
          << "\n    " << etype << " operator() (const " << indexType << " &idx) const"
          << "\n    { return " << etype << "(ckGetArrayID(), idx, CK_DELCTOR_CALL); }"
          << "\n";
    }

    // Add specialized indexing for these common types
    if (indexSuffix == (const char*)"1D") {
      str << "    " << etype << " operator [] (int idx) const \n"
          << "        {return " << etype
          << "(ckGetArrayID(), CkArrayIndex1D(idx), CK_DELCTOR_CALL);}\n"
          << "    " << etype << " operator () (int idx) const \n"
          << "        {return " << etype
          << "(ckGetArrayID(), CkArrayIndex1D(idx), CK_DELCTOR_CALL);}\n";
    } else if (indexSuffix == (const char*)"2D") {
      str << "    " << etype
          << " operator () (int i0,int i1) const \n"
             "        {return "
          << etype
          << "(ckGetArrayID(), CkArrayIndex2D(i0,i1), CK_DELCTOR_CALL);}\n"
             "    "
          << etype
          << " operator () (CkIndex2D idx) const \n"
             "        {return "
          << etype << "(ckGetArrayID(), CkArrayIndex2D(idx), CK_DELCTOR_CALL);}\n";
    } else if (indexSuffix == (const char*)"3D") {
      str << "    " << etype
          << " operator () (int i0,int i1,int i2) const \n"
             "        {return "
          << etype
          << "(ckGetArrayID(), CkArrayIndex3D(i0,i1,i2), CK_DELCTOR_CALL);}\n"
             "    "
          << etype
          << " operator () (CkIndex3D idx) const \n"
             "        {return "
          << etype << "(ckGetArrayID(), CkArrayIndex3D(idx), CK_DELCTOR_CALL);}\n";
    } else if (indexSuffix == (const char*)"4D") {
      str << "    " << etype
          << " operator () (short int i0,short int i1,short int i2,short int i3) const \n"
             "        {return "
          << etype
          << "(ckGetArrayID(), CkArrayIndex4D(i0,i1,i2,i3), CK_DELCTOR_CALL);}\n"
             "    "
          << etype
          << " operator () (CkIndex4D idx) const \n"
             "        {return "
          << etype << "(ckGetArrayID(), CkArrayIndex4D(idx), CK_DELCTOR_CALL);}\n";
    } else if (indexSuffix == (const char*)"5D") {
      str << "    " << etype
          << " operator () (short int i0,short int i1,short int i2,short int i3,short "
             "int i4) const \n"
             "        {return "
          << etype
          << "(ckGetArrayID(), CkArrayIndex5D(i0,i1,i2,i3,i4), CK_DELCTOR_CALL);}\n"
             "    "
          << etype
          << " operator () (CkIndex5D idx) const \n"
             "        {return "
          << etype << "(ckGetArrayID(), CkArrayIndex5D(idx), CK_DELCTOR_CALL);}\n";
    } else if (indexSuffix == (const char*)"6D") {
      str << "    " << etype
          << " operator () (short int i0,short int i1,short int i2,short int i3,short "
             "int i4,short int i5) const \n"
             "        {return "
          << etype
          << "(ckGetArrayID(), CkArrayIndex6D(i0,i1,i2,i3,i4,i5), CK_DELCTOR_CALL);}\n"
             "    "
          << etype
          << " operator () (CkIndex6D idx) const \n"
             "        {return "
          << etype << "(ckGetArrayID(), CkArrayIndex6D(idx), CK_DELCTOR_CALL);}\n";
    }
    str << "    " << ptype
        << "(const CkArrayID &aid,CK_DELCTOR_PARAM) \n"
           "        :";
    genProxyNames(str, "", NULL, "(aid,CK_DELCTOR_ARGS)", ", ");
    str << " {}\n";
    str << "    " << ptype
        << "(const CkArrayID &aid) \n"
           "        :";
    genProxyNames(str, "", NULL, "(aid)", ", ");
    str << " {}\n";
  } else if (forElement == forSection) { /* for Section, indexible version*/
    disambig_array(str, super);
    str << "\n    inline void ckSend(CkArrayMessage *m, int ep, int opts = 0)"
        << "\n    { " << super << "::ckSend(m,ep,opts); }"
        << "\n    inline CkSectionInfo &ckGetSectionInfo()"
        << "\n    { return " << super << "::ckGetSectionInfo(); }"
        << "\n    inline CkSectionID *ckGetSectionIDs()"
        << "\n    { return " << super << "::ckGetSectionIDs(); }"
        << "\n    inline CkSectionID &ckGetSectionID()"
        << "\n    { return " << super << "::ckGetSectionID(); }"
        << "\n    inline CkSectionID &ckGetSectionID(int i)"
        << "\n    { return " << super << "::ckGetSectionID(i); }"
        << "\n    inline CkArrayID ckGetArrayIDn(int i) const"
        << "\n    { return " << super << "::ckGetArrayIDn(i); } "
        << "\n    inline CkArrayIndex *ckGetArrayElements() const"
        << "\n    { return " << super << "::ckGetArrayElements(); }"
        << "\n    inline CkArrayIndex *ckGetArrayElements(int i) const"
        << "\n    { return " << super << "::ckGetArrayElements(i); }"
        << "\n    inline int ckGetNumElements() const"
        << "\n    { return " << super << "::ckGetNumElements(); } "
        << "\n    inline int ckGetNumElements(int i) const"
        << "\n    { return " << super << "::ckGetNumElements(i); }";

    XStr etype;
    etype << Prefix::ProxyElement << type << tvars();
    if (indexSuffix != (const char*)"none") {
      str << "    // Generalized array indexing:\n"
             "    "
          << etype << " operator [] (const " << indexType
          << " &idx) const\n"
             "        {return "
          << etype
          << "(ckGetArrayID(), idx, CK_DELCTOR_CALL);}\n"
             "    "
          << etype << " operator() (const " << indexType
          << " &idx) const\n"
             "        {return "
          << etype << "(ckGetArrayID(), idx, CK_DELCTOR_CALL);}\n";
    }

    // Add specialized indexing for these common types
    if (indexSuffix == (const char*)"1D") {
      str << "    " << etype
          << " operator [] (int idx) const \n"
             "        {return "
          << etype
          << "(ckGetArrayID(), *(CkArrayIndex1D*)&ckGetArrayElements()[idx], "
             "CK_DELCTOR_CALL);}\n"
             "    "
          << etype
          << " operator () (int idx) const \n"
             "        {return "
          << etype
          << "(ckGetArrayID(), *(CkArrayIndex1D*)&ckGetArrayElements()[idx], "
             "CK_DELCTOR_CALL);}\n"
             "    static CkSectionID ckNew(const CkArrayID &aid, CkArrayIndex1D *elems, "
             "int nElems, int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      return CkSectionID(aid, elems, nElems, factor);\n"
             "    } \n"
             "    static CkSectionID ckNew(const CkArrayID &aid, const std::vector<CkArrayIndex1D> &elems, "
             "int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      return CkSectionID(aid, elems, factor);\n"
             "    } \n"
             "    static CkSectionID ckNew(const CkArrayID &aid, int l, int u, int s, "
             "int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      std::vector<CkArrayIndex1D> al;\n"
             "      for (int i=l; i<=u; i+=s) al.emplace_back(i);\n"
             "      return CkSectionID(aid, al, factor);\n"
             "    } \n";
    } else if (indexSuffix == (const char*)"2D") {
      str << "    " << etype
          << " operator () (int idx) const \n"
             "        {return "
          << etype
          << "(ckGetArrayID(), *(CkArrayIndex2D*)&ckGetArrayElements()[idx], "
             "CK_DELCTOR_CALL);}\n"
             "    static CkSectionID ckNew(const CkArrayID &aid, CkArrayIndex2D *elems, "
             "int nElems, int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      return CkSectionID(aid, elems, nElems, factor);\n"
             "    } \n"
             "    static CkSectionID ckNew(const CkArrayID &aid, const std::vector<CkArrayIndex2D> &elems, "
             "int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      return CkSectionID(aid, elems, factor);\n"
             "    } \n"
             "    static CkSectionID ckNew(const CkArrayID &aid, int l1, int u1, int s1, "
             "int l2, int u2, int s2, int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      std::vector<CkArrayIndex2D> al;\n"
             "      for (int i=l1; i<=u1; i+=s1) \n"
             "        for (int j=l2; j<=u2; j+=s2) \n"
             "          al.emplace_back(i, j);\n"
             "      return CkSectionID(aid, al, factor);\n"
             "    } \n";
    } else if (indexSuffix == (const char*)"3D") {
      str << "    " << etype
          << " operator () (int idx) const \n"
             "        {return "
          << etype
          << "(ckGetArrayID(), *(CkArrayIndex3D*)&ckGetArrayElements()[idx], "
             "CK_DELCTOR_CALL);}\n"
             "    static CkSectionID ckNew(const CkArrayID &aid, CkArrayIndex3D *elems, "
             "int nElems, int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      return CkSectionID(aid, elems, nElems, factor);\n"
             "    } \n"
             "    static CkSectionID ckNew(const CkArrayID &aid, const std::vector<CkArrayIndex3D> &elems, "
             "int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      return CkSectionID(aid, elems, factor);\n"
             "    } \n"
             "    static CkSectionID ckNew(const CkArrayID &aid, int l1, int u1, int s1, "
             "int l2, int u2, int s2, int l3, int u3, int s3, int "
             "factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      std::vector<CkArrayIndex3D> al;\n"
             "      for (int i=l1; i<=u1; i+=s1) \n"
             "        for (int j=l2; j<=u2; j+=s2) \n"
             "          for (int k=l3; k<=u3; k+=s3) \n"
             "          al.emplace_back(i, j, k);\n"
             "      return CkSectionID(aid, al, factor);\n"
             "    } \n";
    } else if (indexSuffix == (const char*)"4D") {
      str << "    " << etype
          << " operator () (int idx) const \n"
             "        {return "
          << etype
          << "(ckGetArrayID(), *(CkArrayIndex4D*)&ckGetArrayElements()[idx], "
             "CK_DELCTOR_CALL);}\n"
             "    static CkSectionID ckNew(const CkArrayID &aid, CkArrayIndex4D *elems, "
             "int nElems, int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      return CkSectionID(aid, elems, nElems, factor);\n"
             "    } \n"
             "    static CkSectionID ckNew(const CkArrayID &aid, const std::vector<CkArrayIndex4D> &elems, "
             "int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      return CkSectionID(aid, elems, factor);\n"
             "    } \n"
             "    static CkSectionID ckNew(const CkArrayID &aid, int l1, int u1, int s1, "
             "int l2, int u2, int s2, int l3, int u3, int s3, int l4, int u4, int s4, "
             "int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      std::vector<CkArrayIndex4D> al;\n"
             "      for (int i=l1; i<=u1; i+=s1) \n"
             "        for (int j=l2; j<=u2; j+=s2) \n"
             "          for (int k=l3; k<=u3; k+=s3) \n"
             "            for (int l=l4; l<=u4; l+=s4) \n"
             "              al.emplace_back(i, j, k, l);\n"
             "      return CkSectionID(aid, al, factor);\n"
             "    } \n";
    } else if (indexSuffix == (const char*)"5D") {
      str << "    " << etype
          << " operator () (int idx) const \n"
             "        {return "
          << etype
          << "(ckGetArrayID(), *(CkArrayIndex5D*)&ckGetArrayElements()[idx], "
             "CK_DELCTOR_CALL);}\n"
             "    static CkSectionID ckNew(const CkArrayID &aid, CkArrayIndex5D *elems, "
             "int nElems, int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      return CkSectionID(aid, elems, nElems, factor);\n"
             "    } \n"
             "    static CkSectionID ckNew(const CkArrayID &aid, const std::vector<CkArrayIndex5D> &elems, "
             "int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      return CkSectionID(aid, elems, factor);\n"
             "    } \n"
             "    static CkSectionID ckNew(const CkArrayID &aid, int l1, int u1, int s1, "
             "int l2, int u2, int s2, int l3, int u3, int s3, int l4, int u4, int s4, "
             "int l5, int u5, int s5, int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      std::vector<CkArrayIndex5D> al;\n"
             "      for (int i=l1; i<=u1; i+=s1) \n"
             "        for (int j=l2; j<=u2; j+=s2) \n"
             "          for (int k=l3; k<=u3; k+=s3) \n"
             "            for (int l=l4; l<=u4; l+=s4) \n"
             "              for (int m=l5; m<=u5; m+=s5) \n"
             "                al.emplace_back(i, j, k, l, m);\n"
             "      return CkSectionID(aid, al, factor);\n"
             "    } \n";
    } else if (indexSuffix == (const char*)"6D") {
      str << "    " << etype
          << " operator () (int idx) const \n"
             "        {return "
          << etype
          << "(ckGetArrayID(), *(CkArrayIndex6D*)&ckGetArrayElements()[idx], "
             "CK_DELCTOR_CALL);}\n"
             "    static CkSectionID ckNew(const CkArrayID &aid, CkArrayIndex6D *elems, "
             "int nElems, int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      return CkSectionID(aid, elems, nElems, factor);\n"
             "    } \n"
             "    static CkSectionID ckNew(const CkArrayID &aid, const std::vector<CkArrayIndex6D> &elems, "
             "int factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      return CkSectionID(aid, elems, factor);\n"
             "    } \n"
             "    static CkSectionID ckNew(const CkArrayID &aid, int l1, int u1, int s1, "
             "int l2, int u2, int s2, int l3, int u3, int s3, int l4, int u4, int s4, "
             "int l5, int u5, int s5, int l6, int u6, int s6, int "
             "factor=USE_DEFAULT_BRANCH_FACTOR) {\n"
             "      std::vector<CkArrayIndex6D> al;\n"
             "      for (int i=l1; i<=u1; i+=s1) \n"
             "        for (int j=l2; j<=u2; j+=s2) \n"
             "          for (int k=l3; k<=u3; k+=s3) \n"
             "            for (int l=l4; l<=u4; l+=s4) \n"
             "              for (int m=l5; m<=u5; m+=s5) \n"
             "                for (int n=l6; n<=u6; n+=s6) \n"
             "                  al.emplace_back(i, j, k, l, m, n);\n"
             "      return CkSectionID(aid, al, factor);\n"
             "    } \n";
    }

    str << "    " << ptype
        << "(const CkArrayID &aid, CkArrayIndex *elems, int nElems, CK_DELCTOR_PARAM) \n"
           "        :";
    genProxyNames(str, "", NULL, "(aid,elems,nElems,CK_DELCTOR_ARGS)", ", ");
    str << " {}\n";
    str << "    " << ptype
        << "(const CkArrayID &aid, const std::vector<CkArrayIndex> &elems, CK_DELCTOR_PARAM) \n"
           "        :";
    genProxyNames(str, "", NULL, "(aid,elems,CK_DELCTOR_ARGS)", ", ");
    str << " {}\n";
    str << "    " << ptype
        << "(const CkArrayID &aid, CkArrayIndex *elems, int nElems, int "
           "factor=USE_DEFAULT_BRANCH_FACTOR) \n"
           "        :";
    genProxyNames(str, "", NULL, "(aid,elems,nElems, factor)", ", ");
    str << " {}\n";
    str << "    " << ptype
        << "(const CkArrayID &aid, const std::vector<CkArrayIndex> &elems, int "
           "factor=USE_DEFAULT_BRANCH_FACTOR) \n"
           "        :";
    genProxyNames(str, "", NULL, "(aid,elems, factor)", ", ");
    str << " { ckAutoDelegate(); }\n";
    str << "    " << ptype
        << "(const CkSectionID &sid)  \n"
           "        :";
    genProxyNames(str, "", NULL, "(sid)", ", ");
    str << " { ckAutoDelegate(); }\n";
    str << "    " << ptype
        << "(int n, const CkArrayID *aid, CkArrayIndex const * const *elems, const int "
           "*nElems, CK_DELCTOR_PARAM) \n"
           "        :";
    genProxyNames(str, "", NULL, "(n,aid,elems,nElems,CK_DELCTOR_ARGS)", ", ");
    str << " {}\n";
    str << "    " << ptype
        << "(const std::vector<CkArrayID> &aid, const std::vector<std::vector<CkArrayIndex> > &elems, "
           "CK_DELCTOR_PARAM) \n"
           "        :";
    genProxyNames(str, "", NULL, "(aid,elems,CK_DELCTOR_ARGS)", ", ");
    str << " {}\n";
    str << "    " << ptype
        << "(int n, const CkArrayID *aid, CkArrayIndex const * const *elems, const int "
           "*nElems) \n"
           "        :";
    genProxyNames(str, "", NULL, "(n,aid,elems,nElems)", ", ");
    str << " { ckAutoDelegate(); }\n";
    str << "    " << ptype
        << "(const std::vector<CkArrayID> &aid, const std::vector<std::vector<CkArrayIndex> > &elems) \n"
           "        :";
    genProxyNames(str, "", NULL, "(aid,elems)", ", ");
    str << " { ckAutoDelegate(); }\n";
    str << "    " << ptype
        << "(int n, const CkArrayID *aid, CkArrayIndex const * const *elems, const int "
           "*nElems, int factor) \n"
           "        :";
    genProxyNames(str, "", NULL, "(n,aid,elems,nElems, factor)", ", ");
    str << " { ckAutoDelegate(); }\n";
    str << "    " << ptype
        << "(const std::vector<CkArrayID> &aid, const std::vector<std::vector<CkArrayIndex> > &elems, "
           "int factor) \n"
           "        :";
    genProxyNames(str, "", NULL, "(aid,elems, factor)", ", ");
    str << " { ckAutoDelegate(); }\n";
    str << "    static CkSectionID ckNew(const CkArrayID &aid, CkArrayIndex *elems, int "
           "nElems) {\n"
           "      return CkSectionID(aid, elems, nElems);\n"
           "    } \n";
    str << "    static CkSectionID ckNew(const CkArrayID &aid, const std::vector<CkArrayIndex> &elems) {\n "
           "      return CkSectionID(aid, elems);\n"
           "    } \n";

    str << "    static CkSectionID ckNew(const CkArrayID &aid, CkArrayIndex *elems, int "
           "nElems, int factor) {\n"
           "      return CkSectionID(aid, elems, nElems, factor);\n"
           "    } \n";
    str << "    static CkSectionID ckNew(const CkArrayID &aid, const std::vector<CkArrayIndex> &elems, int "
           "factor) {\n"
           "      return CkSectionID(aid, elems, factor);\n"
           "    } \n";

    str << "    void ckAutoDelegate(int opts=1) {\n"
        << "      if(ckIsDelegated()) return;\n"
        << "      " << super << "::ckAutoDelegate(opts);";
    genProxyNamesExceptFirst(str, "", NULL, "::ckAutoDelegate(0);", "; ");
    str << "\n";
    str << "    } \n";

    str << "    void setReductionClient(CkCallback *cb) {\n"
           "      "
        << super
        << "::setReductionClient(cb);\n"
           "    } \n";
    str << "    void resetSection() {\n"
           "      "
        << super
        << "::resetSection();\n"
           "    } \n";

    str << "    static void contribute(CkSectionInfo &sid, int userData=-1, int fragSize=-1);\n";
    str << "    static void contribute(int dataSize,void *data,CkReduction::reducerType "
           "type, CkSectionInfo &sid, int userData=-1, int fragSize=-1);\n";
    str << "    template <typename T>\n"
           "    static void contribute(std::vector<T> &data, CkReduction::reducerType "
           "type, CkSectionInfo &sid, int userData=-1, int fragSize=-1);\n";
    str << "    static void contribute(CkSectionInfo &sid, const CkCallback &cb, int userData=-1, int fragSize=-1);\n";
    str << "    static void contribute(int dataSize,void *data,CkReduction::reducerType "
           "type, CkSectionInfo &sid, const CkCallback &cb, int userData=-1, int "
           "fragSize=-1);\n";
    str << "    template <typename T>\n"
           "    static void contribute(std::vector<T> &data, CkReduction::reducerType "
           "type, CkSectionInfo &sid, const CkCallback &cb, int userData=-1, int "
           "fragSize=-1);\n";
  }

  genMemberDecls(str);
  str << CIClassEnd;
}

static void disambig_array(XStr& str, const XStr& super) {
  disambig_proxy(str, super);
  str << "\n    inline void ckCheck(void) const"
      << "\n    { " << super << "::ckCheck(); }"
      << "\n    inline operator CkArrayID () const"
      << "\n    { return ckGetArrayID(); }"
      << "\n    inline CkArrayID ckGetArrayID(void) const"
      << "\n    { return " << super << "::ckGetArrayID(); }"
      << "\n    inline CkArray *ckLocalBranch(void) const"
      << "\n    { return " << super << "::ckLocalBranch(); }"
      << "\n    inline CkLocMgr *ckLocMgr(void) const"
      << "\n    { return " << super << "::ckLocMgr(); }"
      << "\n"
      << "\n    inline static CkArrayID ckCreateEmptyArray(CkArrayOptions opts = "
         "CkArrayOptions())"
      << "\n    { return " << super << "::ckCreateEmptyArray(opts); }"
      << "\n    inline static void ckCreateEmptyArrayAsync(CkCallback cb, CkArrayOptions "
         "opts = CkArrayOptions())"
      << "\n    { " << super << "::ckCreateEmptyArrayAsync(cb, opts); }"
      << "\n    inline static CkArrayID ckCreateArray(CkArrayMessage *m,int ctor,const "
         "CkArrayOptions &opts)"
      << "\n    { return " << super << "::ckCreateArray(m,ctor,opts); }"
      << "\n    inline void ckInsertIdx(CkArrayMessage *m,int ctor,int onPe,const "
         "CkArrayIndex &idx)"
      << "\n    { " << super << "::ckInsertIdx(m,ctor,onPe,idx); }"
      << "\n    inline void doneInserting(void)"
      << "\n    { " << super << "::doneInserting(); }"
      << "\n"
      << "\n    inline void ckBroadcast(CkArrayMessage *m, int ep, int opts=0) const"
      << "\n    { " << super << "::ckBroadcast(m,ep,opts); }";
  disambig_reduction_client(str, super);
}

static void disambig_reduction_client(XStr& str, const XStr& super) {
  str << "\n    inline void setReductionClient(CkReductionClientFn fn,void *param=NULL) "
         "const"
      << "\n    { " << super << "::setReductionClient(fn,param); }"
      << "\n    inline void ckSetReductionClient(CkReductionClientFn fn,void "
         "*param=NULL) const"
      << "\n    { " << super << "::ckSetReductionClient(fn,param); }"
      << "\n    inline void ckSetReductionClient(CkCallback *cb) const"
      << "\n    { " << super << "::ckSetReductionClient(cb); }"
      << "\n";
}

static void disambig_group(XStr& str, const XStr& super) {
  disambig_proxy(str, super);
  str << "inline void ckCheck(void) const {" << super << "::ckCheck();}\n"
      << "CkChareID ckGetChareID(void) const\n"
      << "   {return " << super << "::ckGetChareID();}\n"
      << "CkGroupID ckGetGroupID(void) const\n"
      << "   {return " << super << "::ckGetGroupID();}\n"
      << "operator CkGroupID () const { return ckGetGroupID(); }\n";
  disambig_reduction_client(str, super);
}

static XStr indexSuffix2object(const XStr& indexSuffix) {
  if (indexSuffix == (const char*)"1D") return "CkIndex1D";
  if (indexSuffix == (const char*)"2D") return "CkIndex2D";
  if (indexSuffix == (const char*)"3D") return "CkIndex3D";
  if (indexSuffix == (const char*)"4D") return "CkIndex4D";
  if (indexSuffix == (const char*)"5D") return "CkIndex5D";
  if (indexSuffix == (const char*)"6D") return "CkIndex6D";
  if (indexSuffix == (const char*)"Max")
    return "CkIndexMax";
  else
    return indexSuffix;
}

}  // namespace xi
