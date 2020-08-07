#include "xi-Message.h"

#include <vector>

namespace xi {

static const char* CIMsgClassAnsi =
    "{\n"
    "  public:\n"
    "    struct size_t_wrap {\n"
    "      size_t size;\n"
    "      template <typename T, typename = typename std::enable_if<std::is_integral<T>::value>::type>\n"
    "      size_t_wrap(T s) : size{(size_t)s} { };\n"
    "      operator size_t() const { return size; }\n"
    "    };\n"
    "    static int __idx;\n"
    "    void* operator new(size_t, void*p) { return p; }\n"
    "    void* operator new(size_t);\n"
    "    void* operator new(size_t, size_t*, const int);\n"
    "    void* operator new(size_t, size_t*, const int, const GroupDepNum);\n"
    "    void* operator new(size_t, size_t*);\n"
    "#if CMK_MULTIPLE_DELETE\n"
    "    void operator delete(void*p, void*){dealloc(p);}\n"
    "    void operator delete(void*p){dealloc(p);}\n"
    "    void operator delete(void*p, int*, const int){dealloc(p);}\n"
    "    void operator delete(void*p, int*, const int, const GroupDepNum){dealloc(p);}\n"
    "    void operator delete(void*p, int*){dealloc(p);}\n"
    "#endif\n"
    "    void operator delete(void*p, size_t){dealloc(p);}\n"
    "    static void* alloc(int,size_t, size_t*, int, GroupDepNum);\n"
    "    static void dealloc(void *p);\n";

Message::Message(int l, NamedType* t, MsgVarList* mv) : type(t), mvlist(mv) {
  line = l;
  setTemplate(0);
}

const char* Message::proxyPrefix(void) { return Prefix::Message; }

void Message::genAllocDecl(XStr& str) {
  int i, num;
  XStr mtype;
  mtype << type;
  if (templat) templat->genVars(mtype);
  str << CIMsgClassAnsi;
  str << "    CMessage_" << mtype << "();\n";
  str << "    static void *pack(" << mtype << " *p);\n";
  str << "    static " << mtype << "* unpack(void* p);\n";
  num = numArrays();
  if (num > 0) {
    str << "    void *operator new(size_t";
    for (i = 0; i < num; i++) str << ", size_t_wrap";
    str << ");\n";
  }
  str << "    void *operator new(size_t, ";
  for (i = 0; i < num; i++) str << "size_t_wrap, ";
  str << "const int);\n";
  str << "    void *operator new(size_t, ";
  for(i=0;i<num;i++)
    str << "size_t_wrap, ";
  str << "const int, const GroupDepNum);\n";
  str << "#if CMK_MULTIPLE_DELETE\n";
  if (num > 0) {
    str << "    void operator delete(void *p";
    for (i = 0; i < num; i++) str << ", int";
    str << "){dealloc(p);}\n";
  }
  str << "    void operator delete(void *p, ";
  for (i = 0; i < num; i++) str << "int, ";
  str << "const int){dealloc(p);}\n";
  str << "    void operator delete(void *p, ";
  for (i = 0; i < num; i++) str << "int, ";
  str << "const int, const GroupDepNum){dealloc(p);}\n";
  str << "#endif\n";
}

const char GroupDepNumStruct[] =
"#ifndef GROUPDEPNUM_DECLARED\n"
"# define GROUPDEPNUM_DECLARED\n"
"struct GroupDepNum\n"
"{\n"
"  int groupDepNum;\n"
"  explicit GroupDepNum(int g = 0) : groupDepNum{g} { }\n"
"  operator int() const { return groupDepNum; }\n"
"};\n"
"#endif\n";

void Message::genDecls(XStr& str) {
  XStr ptype;
  ptype << proxyPrefix() << type;
  if (type->isTemplated()) return;
  str << "#include <type_traits>\n";
  str << GroupDepNumStruct;
  str << "/* DECLS: ";
  print(str);
  str << " */\n";
  if (templat) templat->genSpec(str);
  str << "class ";
  type->print(str);
  str << ";\n";
  if (templat) templat->genSpec(str);
  str << "class " << ptype;
  if (external || type->isTemplated()) {
    str << ";\n";
    return;
  }
  str << ":public CkMessage";

  genAllocDecl(str);

  if (!(external || type->isTemplated())) {
    // generate register function
    str << "    static void __register(const char *s, size_t size, CkPackFnPtr pack, "
           "CkUnpackFnPtr unpack) {\n";
    str << "      __idx = CkRegisterMsg(s, pack, unpack, dealloc, size);\n";
    str << "    }\n";
  }
  str << "};\n";

  if (strncmp(type->getBaseName(), "MarshallMsg_", 12) == 0) {
    MsgVarList* ml;
    MsgVar* mv;
    int i;
    str << "class " << type << " : public " << ptype << " {\n";
    str << "  public:\n";
    int num = numVars();
    for (i = 0, ml = mvlist; i < num; i++, ml = ml->next) {
      mv = ml->msg_var;
      if (mv->isConditional() || mv->isArray()) {
        str << "    /* ";
        mv->print(str);
        str << " */\n";
        str << "    " << mv->type << " *" << mv->name << ";\n";
      }
    }
    str << "};\n";
  }
}

void Message::genDefs(XStr& str) {
  int i, count, num = numVars();
  int numArray = numArrays();
  MsgVarList* ml;
  MsgVar* mv;
  XStr ptype, mtype, tspec;
  ptype << proxyPrefix() << type;
  if (templat) templat->genVars(ptype);
  mtype << type;
  if (templat) templat->genVars(mtype);
  if (templat) {
    templat->genSpec(tspec);
    tspec << " ";
  }

  str << "/* DEFS: ";
  print(str);
  str << " */\n";

  templateGuardBegin(templat, str);
  if (!(external || type->isTemplated())) {
    // new (size_t)
    str << tspec << "void *" << ptype << "::operator new(size_t s){\n";
    str << "  return " << mtype << "::alloc(__idx, s, 0, 0, GroupDepNum{});\n}\n";
    // new (size_t, size_t*)
    str << tspec << "void *" << ptype << "::operator new(size_t s, size_t* sz){\n";
    str << "  return " << mtype << "::alloc(__idx, s, sz, 0, GroupDepNum{});\n}\n";
    // new (size_t, size_t*, priobits)
    str << tspec << "void *" << ptype << "::operator new(size_t s, size_t* sz,";
    str << "const int pb){\n";
    str << "  return " << mtype << "::alloc(__idx, s, sz, pb, GroupDepNum{});\n}\n";
    // new (size_t, size_t*, priobits, groupDepNum)
    str << tspec << "void *" << ptype << "::operator new(size_t s, size_t* sz,";
    str << "const int pb, const GroupDepNum groupDepNum){\n";
    str << "  return " << mtype << "::alloc(__idx, s, sz, pb, groupDepNum);\n}\n";
    // new (size_t, size_t, size_t, ..., int)
    if (numArray > 0) {
      str << tspec << "void *" << ptype << "::operator new(size_t s";
      for (i = 0; i < numArray; i++) str << ", size_t_wrap sz" << i;
      str << ") {\n";
      str << "  size_t sizes[" << numArray << "];\n";
      for (i = 0; i < numArray; i++) str << "  sizes[" << i << "] = sz" << i << ";\n";
      str << "  return " << mtype << "::alloc(__idx, s, sizes, 0, GroupDepNum{});\n";
      str << "}\n";
    }
    // new (size_t, int, int, ..., int, priobits)
    // degenerates to  new(size_t, priobits)  if no varsize
    std::vector<MsgVar*> arrayVars;
    str << tspec << "void *" << ptype << "::operator new(size_t s, ";
    for (i = 0; i < numArray; i++) str << "size_t_wrap sz" << i << ", ";
    str << "const int p) {\n";
    if (numArray > 0) {
      str << "  size_t sizes[" << numArray << "];\n";
      for (i = 0, count = 0, ml = mvlist; i < num; i++, ml = ml->next) {
        mv = ml->msg_var;
        if (mv->isArray()) {
          str << "  sizes[" << count << "] = sz" << count << ";\n";
          count++;
          arrayVars.push_back(mv);
        }
      }
    }
    str << "  return " << mtype << "::alloc(__idx, s, " << (numArray > 0 ? "sizes" : "0")
        << ", p, GroupDepNum{});\n";
    str << "}\n";
    // new (size_t, int, int, ..., int, priobits, groupDepNum)
    // degenerates to  new(size_t, priobits, groupDepNum)  if no varsize
    arrayVars.clear();
    str << tspec << "void *"<< ptype << "::operator new(size_t s, ";
    for(i=0;i<numArray;i++)
      str << "size_t_wrap sz" << i << ", ";
    str << "const int p, const GroupDepNum groupDepNum) {\n";
    if (numArray>0) {
      str << "  size_t sizes[" << numArray << "];\n";
      for(i=0, count=0, ml=mvlist ;i<num; i++, ml=ml->next) {
        mv = ml->msg_var;
        if (mv->isArray()) {
          str << "  sizes[" << count << "] = sz" << count << ";\n";
          count ++;
          arrayVars.push_back(mv);
        }
      }
    }
    str << "  return " << mtype << "::alloc(__idx, s, " << (numArray>0?"sizes":"0") << ", p, groupDepNum);\n";
    str << "}\n";

    // alloc(int, size_t, size_t*, priobits, groupDepNum)
    str << tspec << "void* " << ptype;
    str << "::alloc(int msgnum, size_t sz, size_t *sizes, int pb, GroupDepNum groupDepNum) {\n";
    str << "  CkpvAccess(_offsets)[0] = ALIGN_DEFAULT(sz);\n";
    for (count = 0; count < numArray; count++) {
      mv = arrayVars[count];
      str << "  if(sizes==0)\n";
      str << "    CkpvAccess(_offsets)[" << count + 1 << "] = CkpvAccess(_offsets)[0];\n";
      str << "  else\n";
      str << "    CkpvAccess(_offsets)[" << count + 1 << "] = CkpvAccess(_offsets)["
          << count << "] + ";
      str << "ALIGN_DEFAULT(sizeof(" << mv->type << ")*sizes[" << count << "]);\n";
    }
    str << "  return CkAllocMsg(msgnum, CkpvAccess(_offsets)[" << numArray << "], pb, groupDepNum);\n";
    str << "}\n";

    str << tspec << ptype << "::" << proxyPrefix() << type << "() {\n";
    str << mtype << " *newmsg = (" << mtype << " *)this;\n";
    for (i = 0, count = 0, ml = mvlist; i < num; i++, ml = ml->next) {
      mv = ml->msg_var;
      if (mv->isArray()) {
        str << "  newmsg->" << mv->name << " = (" << mv->type << " *) ";
        str << "((char *)newmsg + CkpvAccess(_offsets)[" << count << "]);\n";
        count++;
      }
    }
    str << "}\n";

    int numCond = numConditional();
    str << tspec << "void " << ptype << "::dealloc(void *p) {\n";
    if (numCond > 0) {
      str << "  " << mtype << " *msg = (" << mtype << "*) p;\n";
      for (i = 0, count = 0, ml = mvlist; i < num; i++, ml = ml->next) {
        mv = ml->msg_var;
        if (mv->isConditional()) {
          if (mv->type->isPointer())
            XLAT_ERROR_NOCOL("conditional variable cannot be a pointer", line);
          str << "  CkConditional *cond_" << mv->name
              << " = static_cast<CkConditional*>(msg->" << mv->name << ");\n";
          str << "  if (cond_" << mv->name << "!=NULL) cond_" << mv->name
              << "->deallocate();\n";
        }
      }
    }
    str << "  CkFreeMsg(p);\n";
    str << "}\n";
    // pack
    str << tspec << "void* " << ptype << "::pack(" << mtype << " *msg) {\n";
    for (i = 0, ml = mvlist; i < num; i++, ml = ml->next) {
      mv = ml->msg_var;
      if (mv->isArray()) {
        str << "  msg->" << mv->name << " = (" << mv->type << " *) ";
        str << "((char *)msg->" << mv->name << " - (char *)msg);\n";
      }
    }
    if (numCond > 0) {
      str << "  size_t impl_off[" << numCond + 1 << "];\n";
      str << "  impl_off[0] = UsrToEnv(msg)->getUsersize();\n";
      for (i = 0, count = 0, ml = mvlist; i < num; i++, ml = ml->next) {
        mv = ml->msg_var;
        if (mv->isConditional()) {
          str << "  if (msg->" << mv->name << "!=NULL) { /* conditional packing of ";
          mv->type->print(str);
          str << " " << mv->name << " */\n";
          str << "    PUP::sizer implP;\n";
          str << "    implP|*msg->" << mv->name << ";\n";
          str << "    impl_off[" << count + 1 << "] = impl_off[" << count
              << "] + implP.size();\n";
          str << "  } else {\n";
          str << "    impl_off[" << count + 1 << "] = impl_off[" << count << "];\n";
          str << "  }\n";
          count++;
        }
      }
      str << "  " << mtype << " *newmsg = (" << mtype << "*) CkAllocMsg(__idx, impl_off["
          << numCond << "], UsrToEnv(msg)->getPriobits());\n";
      str << "  envelope *newenv = UsrToEnv(newmsg);\n";
      str << "  UInt newSize = newenv->getTotalsize();\n";
      str << "  CmiMemcpy(newenv, UsrToEnv(msg), impl_off[0]+sizeof(envelope));\n";
      str << "  newenv->setTotalsize(newSize);\n";
      str << "  if (UsrToEnv(msg)->getPriobits() > 0) CmiMemcpy(newenv->getPrioPtr(), "
             "UsrToEnv(msg)->getPrioPtr(), newenv->getPrioBytes());\n";
      for (i = 0, count = 0, ml = mvlist; i < num; i++, ml = ml->next) {
        mv = ml->msg_var;
        if (mv->isConditional()) {
          str << "  if (msg->" << mv->name << "!=NULL) { /* conditional packing of ";
          mv->type->print(str);
          str << " " << mv->name << " */\n";
          str << "    newmsg->" << mv->name << " = (";
          mv->type->print(str);
          str << "*)(((char*)newmsg)+impl_off[" << count << "]);\n";
          str << "    PUP::toMem implP((void *)newmsg->" << mv->name << ");\n";
          str << "    implP|*msg->" << mv->name << ";\n";
          str << "    newmsg->" << mv->name << " = (" << mv->type
              << "*) ((char *)newmsg->" << mv->name << " - (char *)newmsg);\n";
          str << "  }\n";
          count++;
        }
      }
      str << "  CkFreeMsg(msg);\n";
      str << "  msg = newmsg;\n";
    }
    str << "  return (void *) msg;\n}\n";
    // unpack
    str << tspec << mtype << "* " << ptype << "::unpack(void* buf) {\n";
    str << "  " << mtype << " *msg = (" << mtype << " *) buf;\n";
    for (i = 0, ml = mvlist; i < num; i++, ml = ml->next) {
      mv = ml->msg_var;
      if (mv->isArray()) {
        str << "  msg->" << mv->name << " = (" << mv->type << " *) ";
        str << "((size_t)msg->" << mv->name << " + (char *)msg);\n";
      }
    }
    if (numCond > 0) {
      for (i = 0, count = 0, ml = mvlist; i < num; i++, ml = ml->next) {
        mv = ml->msg_var;
        if (mv->isConditional()) {
          str << "  if (msg->" << mv->name << "!=NULL) { /* conditional packing of ";
          mv->type->print(str);
          str << " " << mv->name << " */\n";
          str << "    PUP::fromMem implP((char*)msg + (size_t)msg->" << mv->name
              << ");\n";
          str << "    msg->" << mv->name << " = new " << mv->type << ";\n";
          str << "    implP|*msg->" << mv->name << ";\n";
          str << "  }\n";
          count++;
        }
      }
    }
    str << "  return msg;\n}\n";
  }
  if (!templat) {
    if (!external && !type->isTemplated()) {
      str << "int " << ptype << "::__idx=0;\n";
    }
  } else {
    str << tspec << "int " << ptype << "::__idx=0;\n";
  }
  templateGuardEnd(str);
}

void Message::genReg(XStr& str) {
  str << "/* REG: ";
  print(str);
  str << "*/\n";
  if (!templat && !external) {
    XStr ptype, mtype, tspec;
    ptype << proxyPrefix() << type;
    str << ptype << "::__register(\"" << type << "\", sizeof(" << type << "),";
    str << "(CkPackFnPtr) " << type << "::pack,";
    str << "(CkUnpackFnPtr) " << type << "::unpack);\n";
  }
}

void Message::print(XStr& str) {
  if (external) str << "extern ";
  if (templat) templat->genSpec(str);
  str << "message ";
  type->print(str);
  printVars(str);
  str << ";\n";
}

void Message::printVars(XStr& str) {
  if (mvlist != 0) {
    str << "{\n";
    mvlist->print(str);
    str << "}\n";
  }
}

int Message::numArrays(void) {
  if (mvlist == 0) return 0;
  int count = 0;
  MsgVarList* mv = mvlist;
  for (int i = 0; i < mvlist->len(); ++i, mv = mv->next)
    if (mv->msg_var->isArray()) count++;
  return count;
}

int Message::numConditional(void) {
  if (mvlist == 0) return 0;
  int count = 0;
  MsgVarList* mv = mvlist;
  for (int i = 0; i < mvlist->len(); ++i, mv = mv->next)
    if (mv->msg_var->isConditional()) count++;
  return count;
}

int Message::numVars(void) { return ((mvlist == 0) ? 0 : mvlist->len()); }

MsgVar::MsgVar(Type* t, const char* n, int c, int a)
    : type(t), name(n), cond(c), array(a) {}

Type* MsgVar::getType() { return type; }

const char* MsgVar::getName() { return name; }

int MsgVar::isConditional() { return cond; }

int MsgVar::isArray() { return array; }

void MsgVar::print(XStr& str) {
  str << (isConditional() ? "conditional " : "");
  type->print(str);
  str << " " << name << (isArray() ? "[]" : "") << ";";
}

MsgVarList::MsgVarList(MsgVar* mv, MsgVarList* n) : msg_var(mv), next(n) {}

void MsgVarList::print(XStr& str) {
  msg_var->print(str);
  str << "\n";
  if (next) next->print(str);
}

int MsgVarList::len(void) { return (next == 0) ? 1 : (next->len() + 1); }

}  // namespace xi
