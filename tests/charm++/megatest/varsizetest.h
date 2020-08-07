#ifndef _VARSIZETEST_H
#define _VARSIZETEST_H

#include "megatest.h"
#include "varsizetest.decl.h"


class varsizetest_Msg : public CMessage_varsizetest_Msg {
 public:
  static void *alloc(int mnum, size_t size, size_t *sizes, int priobits, GroupDepNum groupDepNum);
  static void *pack(varsizetest_Msg *msg);
  static varsizetest_Msg *unpack(void *buf);
  varsizetest_Msg(int s);
  int check(void);
  int isize;
  int fsize;
  int *iarray;
  float *farray;
  int seqnum;
  CkChareID myMain;
}; 

class varsizetest_main : public CBase_varsizetest_main {
 public:
  varsizetest_main(void);
  varsizetest_main(CkMigrateMessage *m) {}
  void exit(varsizetest_Msg *m);
};

class varsizetest_test : public CBase_varsizetest_test {
 public:
  varsizetest_test(varsizetest_Msg *m);
  varsizetest_test(CkMigrateMessage *m) {}
};

#endif
