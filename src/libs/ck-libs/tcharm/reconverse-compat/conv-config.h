/*
  conv-config.h for reconverse builds.

  The classic conv-config.h (src/conv-core/conv-config.h) assembles the
  configuration from machine-layer headers (conv-common.h, conv-mach.h,
  lrts-common.h) that reconverse does not have. On reconverse the
  configuration is three headers, the same ones charm.h includes:
    charm-config.h     installed from reconverse (CMK_RECONVERSE, ...)
    conv-mach-opt.h    build-time options
    conv-autoconfig.h  detected compiler and platform features
  This header includes those three, so code that includes conv-config.h
  (memory-isomalloc.h, cmitls.h, ampi.h) sees the same configuration
  macros as Charm++ itself. It defines no types and includes no Converse API;
  include converse.h for that.
*/
#ifndef __UIUC_CHARM_CONV_CONFIG_H
#define __UIUC_CHARM_CONV_CONFIG_H

#include "charm-config.h"
#include "conv-mach-opt.h"
#include "conv-autoconfig.h"

/* conv-mach.h, which defines CMK_64BIT for the classic layers, is not on
   this include chain, so the files that size address-space heuristics by it
   (isomalloc.C) saw the 32-bit values on every reconverse build. */
#include <stdint.h>
#if !defined(CMK_64BIT) && UINTPTR_MAX > 0xffffffffu
#define CMK_64BIT 1
#endif

#endif
