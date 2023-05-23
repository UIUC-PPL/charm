
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#ifndef __USE_GNU
# define __USE_GNU
#endif

#include "ampi_funcptr_loader.h"
#undef atexit

#include <string>
#include <atomic>
#include <vector>
#include <map>
#include <functional>

#if CMK_HAS_DL_ITERATE_PHDR
#include <link.h>

#if CMK_HAS_ELF_H
#include <elf.h>
#endif

#if CMK_HAS_MMAP
#include <sys/mman.h>
#endif

#ifndef PT_GNU_PROPERTY
#define PT_GNU_PROPERTY 0x6474e553
#endif
#endif


// debug level 1: process steps, object and segment layouts
// debug level 2: (extremely verbose) relocation patching
#define PIEGLOBALS_DEBUG 0


struct itemstruct
{
  size_t offset;
  size_t size;
  int flags;
};

struct objectstruct
{
  std::string name{};
  char * source = nullptr;
  size_t size = 0;
  size_t align = 0;
  std::vector<itemstruct> items{};
};

struct pieglobalsstruct
{
  ampi_mainstruct mainstruct;
  SharedObject myexe;
  ptrdiff_t main_c_diff, main_f_diff;

  size_t numobjects = 0;
  std::vector<objectstruct> objects{};

  std::vector<std::tuple<ptrdiff_t, uintptr_t, size_t>> segment_allocation_offsets{};

  CmiIsomallocContext ctx;
  std::vector<std::tuple<uintptr_t, size_t, size_t>> global_constructor_heap{};

  int nodeleaderrank;

  void dealloc()
  {
    global_constructor_heap.clear();
  }
};


#if PIEGLOBALS_DEBUG >= 1
static const char * phdr_get_dynamic_type_name(uintptr_t type)
{
  switch (type)
  {
    case DT_NULL: return "DT_NULL";
    case DT_NEEDED: return "DT_NEEDED";
    case DT_PLTRELSZ: return "DT_PLTRELSZ";
    case DT_PLTGOT: return "DT_PLTGOT";
    case DT_HASH: return "DT_HASH";
    case DT_STRTAB: return "DT_STRTAB";
    case DT_SYMTAB: return "DT_SYMTAB";
    case DT_RELA: return "DT_RELA";
    case DT_RELASZ: return "DT_RELASZ";
    case DT_RELAENT: return "DT_RELAENT";
    case DT_STRSZ: return "DT_STRSZ";
    case DT_SYMENT: return "DT_SYMENT";
    case DT_INIT: return "DT_INIT";
    case DT_FINI: return "DT_FINI";
    case DT_SONAME: return "DT_SONAME";
    case DT_RPATH: return "DT_RPATH";
    case DT_SYMBOLIC: return "DT_SYMBOLIC";
    case DT_REL: return "DT_REL";
    case DT_RELSZ: return "DT_RELSZ";
    case DT_RELENT: return "DT_RELENT";
    case DT_PLTREL: return "DT_PLTREL";
    case DT_DEBUG: return "DT_DEBUG";
    case DT_TEXTREL: return "DT_TEXTREL";
    case DT_JMPREL: return "DT_JMPREL";
    case DT_BIND_NOW: return "DT_BIND_NOW";
    case DT_INIT_ARRAY: return "DT_INIT_ARRAY";
    case DT_FINI_ARRAY: return "DT_FINI_ARRAY";
    case DT_INIT_ARRAYSZ: return "DT_INIT_ARRAYSZ";
    case DT_FINI_ARRAYSZ: return "DT_FINI_ARRAYSZ";
    case DT_RUNPATH: return "DT_RUNPATH";
    case DT_FLAGS: return "DT_FLAGS";
    case DT_PREINIT_ARRAY: return "DT_PREINIT_ARRAY";
    case DT_PREINIT_ARRAYSZ: return "DT_PREINIT_ARRAYSZ";
    case DT_SYMTAB_SHNDX: return "DT_SYMTAB_SHNDX";
#if 0
    case DT_RELRSZ: return "DT_RELRSZ";
    case DT_RELR: return "DT_RELR";
    case DT_RELRENT: return "DT_RELRENT";
    case DT_ENCODING: return "DT_ENCODING";
#endif
    case DT_LOOS: return "DT_LOOS";
    case DT_HIOS: return "DT_HIOS";
    case DT_LOPROC: return "DT_LOPROC";
    case DT_HIPROC: return "DT_HIPROC";
    case DT_VALRNGLO: return "DT_VALRNGLO";
    // case DT_GNU_FLAGS_1: return "DT_GNU_FLAGS_1";
    case DT_GNU_PRELINKED: return "DT_GNU_PRELINKED";
    case DT_GNU_CONFLICTSZ: return "DT_GNU_CONFLICTSZ";
    case DT_GNU_LIBLISTSZ: return "DT_GNU_LIBLISTSZ";
    case DT_CHECKSUM: return "DT_CHECKSUM";
    case DT_PLTPADSZ: return "DT_PLTPADSZ";
    case DT_MOVEENT: return "DT_MOVEENT";
    case DT_MOVESZ: return "DT_MOVESZ";
    // case DT_FEATURE: return "DT_FEATURE";
    case DT_POSFLAG_1: return "DT_POSFLAG_1";
    case DT_SYMINSZ: return "DT_SYMINSZ";
    case DT_SYMINENT: return "DT_SYMINENT";
    // case DT_VALRNGHI: return "DT_VALRNGHI";
    case DT_ADDRRNGLO: return "DT_ADDRRNGLO";
    case DT_GNU_HASH: return "DT_GNU_HASH";
    case DT_TLSDESC_PLT: return "DT_TLSDESC_PLT";
    case DT_TLSDESC_GOT: return "DT_TLSDESC_GOT";
    case DT_GNU_CONFLICT: return "DT_GNU_CONFLICT";
    case DT_GNU_LIBLIST: return "DT_GNU_LIBLIST";
    case DT_CONFIG: return "DT_CONFIG";
    case DT_DEPAUDIT: return "DT_DEPAUDIT";
    case DT_AUDIT: return "DT_AUDIT";
    case DT_PLTPAD: return "DT_PLTPAD";
    case DT_MOVETAB: return "DT_MOVETAB";
    case DT_SYMINFO: return "DT_SYMINFO";
    // case DT_ADDRRNGHI: return "DT_ADDRRNGHI";
    case DT_RELACOUNT: return "DT_RELACOUNT";
    case DT_RELCOUNT: return "DT_RELCOUNT";
    case DT_FLAGS_1: return "DT_FLAGS_1";
    case DT_VERDEF: return "DT_VERDEF";
    case DT_VERDEFNUM: return "DT_VERDEFNUM";
    case DT_VERNEED: return "DT_VERNEED";
    case DT_VERNEEDNUM: return "DT_VERNEEDNUM";
    case DT_VERSYM: return "DT_VERSYM";
#if 0
    case DT_AUXILIARY: return "DT_AUXILIARY";
    case DT_USED: return "DT_USED";
    case DT_FILTER: return "DT_FILTER";
#endif
    default: return "";
  }
}
#endif

static int phdr_print(struct dl_phdr_info * info, size_t size, void * data)
{
  auto & numobjects = *(size_t *)data;
  if (numobjects > 0)
  {
    --numobjects;
    return 0;
  }

#if PIEGLOBALS_DEBUG >= 1
  CmiPrintf("pieglobals> Object - \"%s\" - %d segments\n", info->dlpi_name, info->dlpi_phnum);
#endif

  for (int j = 0; j < info->dlpi_phnum; j++)
  {
    const auto & phdr = info->dlpi_phdr[j];
    const int p_type = phdr.p_type;

#if PIEGLOBALS_DEBUG >= 1
    const char * type;
    switch (p_type)
    {
      case PT_LOAD: type = "PT_LOAD"; break;
      case PT_DYNAMIC: type = "PT_DYNAMIC"; break;
      case PT_INTERP: type = "PT_INTERP"; break;
      case PT_NOTE: type = "PT_NOTE"; break;
      case PT_SHLIB: type = "PT_SHLIB"; break;
      case PT_PHDR: type = "PT_PHDR"; break;
      case PT_TLS: type = "PT_TLS"; break;
      case PT_GNU_EH_FRAME: type = "PT_GNU_EH_FRAME"; break;
      case PT_GNU_STACK: type = "PT_GNU_STACK"; break;
      case PT_GNU_RELRO: type = "PT_GNU_RELRO"; break;
      case PT_GNU_PROPERTY: type = "PT_GNU_PROPERTY"; break;
      default: type = nullptr; break;
    }

    const uintptr_t start = (uintptr_t)info->dlpi_addr + phdr.p_vaddr;
    const uintptr_t end = start + phdr.p_memsz;
    CmiPrintf("    %2d: [0x%012" PRIxPTR ", 0x%012" PRIxPTR "); memsz: 0x%07lx; align: 0x%04lx; flags: %c%c%c + 0x%x; ", j,
           start,
           end,
           phdr.p_memsz,
           phdr.p_align,
           (phdr.p_flags & PF_R) ? 'R' : '-',
           (phdr.p_flags & PF_W) ? 'W' : '-',
           (phdr.p_flags & PF_X) ? 'X' : '-',
           phdr.p_flags & ~(PF_R|PF_W|PF_X));
    if (type != nullptr)
      CmiPrintf("%s\n", type);
    else
      CmiPrintf("[other (0x%x)]\n", p_type);

    if (p_type == PT_DYNAMIC)
    {
      const auto item_start = (const uintptr_t *)start, item_end = (const uintptr_t *)end;
      for (auto item = item_start; item + 1 < item_end && item[0] != DT_NULL; item += 2)
      {
        const char * const item_type_name = phdr_get_dynamic_type_name(item[0]);
        CmiPrintf("     - %2zu: 0x%08" PRIxPTR " = 0x%012" PRIxPTR "; %s\n",
          (item - item_start) / 2, item[0], item[1], item_type_name);
      }
    }
#endif
  }

  return 0;
}

static int phdr_probe(struct dl_phdr_info * info, size_t size, void * data)
{
  auto & pieglobalsdata = *(pieglobalsstruct *)data;
  size_t & numobjects = pieglobalsdata.numobjects;
  if (numobjects > 0)
  {
    --numobjects;
    return 0;
  }

  pieglobalsdata.objects.emplace_back();
  objectstruct & obj = pieglobalsdata.objects.back();
  obj.name = info->dlpi_name;

  {
    void * objstart = (void *)(intptr_t)-1, * objend = nullptr;

    for (int j = 0; j < info->dlpi_phnum; j++)
    {
      const auto & phdr = info->dlpi_phdr[j];
      if (phdr.p_type != PT_LOAD)
        continue;

      void * start = (void *)(info->dlpi_addr + phdr.p_vaddr);
      void * end = (char *)start + phdr.p_memsz;
      if (objstart > start)
        objstart = start;
      if (objend < end)
        objend = end;
    }

    obj.source = (char *)objstart;
    obj.size = (char *)objend - (char *)objstart;
  }

  for (int j = 0; j < info->dlpi_phnum; j++)
  {
    const auto & phdr = info->dlpi_phdr[j];
    if (phdr.p_type != PT_LOAD)
      continue;

    obj.items.emplace_back();
    itemstruct & item = obj.items.back();

    auto start = (char *)(info->dlpi_addr + phdr.p_vaddr);
    if (start == obj.source)
      obj.align = phdr.p_align;

    item.offset = (char *)start - obj.source;
    item.size = phdr.p_memsz;
    item.flags = phdr.p_flags;
  }

  return 0;
}

static int phdr_count(struct dl_phdr_info * info, size_t size, void * data)
{
  auto & numobjects = *(size_t *)data;
  ++numobjects;
  return 0;
}


static pieglobalsstruct pieglobalsdata{};

static void pieglobalscleanup()
{
  if (pieglobalsdata.ctx.opaque == nullptr)
    return;

  const CthThread th = TCharm::get()->getThread();

  CthInterceptionsDeactivatePush(th);

  CmiMemoryIsomallocContextActivate(pieglobalsdata.ctx);
  dlclose(pieglobalsdata.myexe);

  CthInterceptionsDeactivatePop(th);

  CmiIsomallocContextDelete(pieglobalsdata.ctx);

  pieglobalsdata.ctx.opaque = nullptr;
}

static void pieglobalscleanupatexit()
{
  if (pieglobalsdata.ctx.opaque == nullptr)
    return;

  CmiMemoryIsomallocContextActivate(pieglobalsdata.ctx);
  dlclose(pieglobalsdata.myexe);
  CmiMemoryIsomallocContextActivate(CmiIsomallocContext{});

  CmiIsomallocContextDelete(pieglobalsdata.ctx);

  pieglobalsdata.ctx.opaque = nullptr;
}


void AMPI_Node_Setup(int numranks)
{
  ampiUsingPieglobals = true;

  if (CmiMyNode() == 0 && !quietModeRequested)
    CmiPrintf("AMPI> Using pieglobals privatization method.\n");

#if PIEGLOBALS_DEBUG >= 1
  if (!quietModeRequested)
    CmiPrintf("pieglobals> [%d][%d] AMPI_Node_Setup(%d)\n",
              CmiMyNode(), CmiMyPe(), numranks);
#endif


  // set up fake isomalloc context using slot n+1

  constexpr int subdivisions = 2;
  int finalslotstart = numranks * subdivisions;
  int numsubdividedslots = (numranks+1) * subdivisions;
  CmiIsomallocContext ctx = CmiIsomallocContextCreate(finalslotstart, numsubdividedslots);

  if (ctx.opaque == nullptr)
    CkAbort("AMPI> pieglobals requires Isomalloc!");


  // prepare common globals method user object data

  AMPI_FuncPtr_Transport funcptrs{};
  if (AMPI_FuncPtr_Pack(&funcptrs, sizeof(funcptrs)))
    CkAbort("Globals runtime linking pack failed due to mismatch!");

  static const char exe_suffix[] = STRINGIFY(CMK_POST_EXE);
  static const char suffix[] = STRINGIFY(CMK_USER_SUFFIX) "." STRINGIFY(CMK_SHARED_SUF);
  static constexpr size_t exe_suffix_len = sizeof(exe_suffix)-1;

  std::string binary_path{ampi_binary_path};
  if (exe_suffix_len > 0)
  {
    size_t pos = binary_path.length() - exe_suffix_len;
    if (!binary_path.compare(pos, exe_suffix_len, exe_suffix))
      binary_path.resize(pos);
  }
  binary_path += suffix;
  const char * const binary_path_str = binary_path.c_str();


  // pre-dlopen setup

  size_t numobjects = 0;
  dl_iterate_phdr(phdr_count, &numobjects);

  atexit(pieglobalscleanupatexit);

  CmiIsomallocContextEnableRandomAccess(ctx);
  CmiIsomallocContextEnableRecording(ctx, 1);
  CmiMemoryIsomallocContextActivate(ctx);


  // load user object

  int flags = RTLD_NOW|RTLD_LOCAL;
#if CMK_HAS_RTLD_DEEPBIND
  if (AMPI_FuncPtr_Active())
    flags |= RTLD_DEEPBIND;
#endif
  SharedObject myexe = dlopen(binary_path_str, flags);

  if (myexe == nullptr)
  {
    CkError("dlopen error: %s\n", dlerror());
    CkAbort("Could not open pieglobals user program!");
  }

  auto unpack = AMPI_FuncPtr_Unpack_Locate(myexe);
  if (unpack != nullptr)
  {
    if (unpack(&funcptrs, sizeof(funcptrs)))
      CkAbort("Globals runtime linking unpack failed due to mismatch!");
  }

  pieglobalsdata.mainstruct = AMPI_Main_Get(myexe);

  if (pieglobalsdata.mainstruct.c == nullptr && pieglobalsdata.mainstruct.f == nullptr)
    CkAbort("Could not find any AMPI entry points!");


  // post-dlopen handling

  CmiMemoryIsomallocContextActivate(CmiIsomallocContext{});
  CmiIsomallocContextEnableRecording(ctx, 0);

  CmiIsomallocGetRecordedHeap(ctx, pieglobalsdata.global_constructor_heap);
  pieglobalsdata.ctx = ctx;
  pieglobalsdata.myexe = myexe;

  if (CmiMyNode() == 0 && !quietModeRequested)
  {
    size_t mynumobjects = numobjects;
    dl_iterate_phdr(phdr_print, &mynumobjects);
  }

  pieglobalsdata.numobjects = numobjects;
  dl_iterate_phdr(phdr_probe, &pieglobalsdata);
  CmiAssert(!pieglobalsdata.objects.empty());

  atexit(ampiMarkAtexit);

#if CMK_HAS_TLS_VARIABLES
  CmiTLSStatsInit();
#endif
}


struct local_heap_entry
{
  void * addr;
  size_t size;
};

using heap_map_t = std::map<uintptr_t, local_heap_entry, std::greater<uintptr_t>>;

static void replace_in_range(
  size_t myrank,
  void * range_start, void * range_end,
  const heap_map_t & my_heap_map,
  char * dstheapstart, CmiIsomallocRegion srcheap,
  const std::vector<char *> & segment_allocations,
  const char * range_name)
{
  for (void ** scan = (void **)range_start, ** end = (void **)range_end; scan < end; ++scan)
  {
    void * data = *scan;

    for (const auto & obj2 : pieglobalsdata.objects)
    {
      if (obj2.source <= data && data <= obj2.source + obj2.size)
      {
        char * allocation2 = segment_allocations[&obj2 - pieglobalsdata.objects.data()];
        void * out = allocation2 + ((char *)data - obj2.source);
#if PIEGLOBALS_DEBUG >= 2
        if (!quietModeRequested)
          CmiPrintf("pieglobals> [%d][%d][%zu] "
                    "0x%012" PRIxPTR ": found 0x%012" PRIxPTR ", writing 0x%012" PRIxPTR
                    " (in %s, to %s)\n",
                    CmiMyNode(), CmiMyPe(), myrank,
                    (uintptr_t)scan, (uintptr_t)data, (uintptr_t)out,
                    range_name, obj2.name.c_str());
#endif
        *scan = out;
      }
    }

#if 0
    // fast O(n) method, needs more work elsewhere to ensure deterministic heap reproduction
    if (srcheap.start <= data && data <= srcheap.end)
    {
      void * out = dstheapstart + ((char *)data - (char *)srcheap.start);
#if PIEGLOBALS_DEBUG >= 2
      if (!quietModeRequested)
        CmiPrintf("pieglobals> [%d][%d][%zu] "
                  "0x%012" PRIxPTR ": found 0x%012" PRIxPTR ", writing 0x%012" PRIxPTR
                  " (in %s, to heap)\n",
                  CmiMyNode(), CmiMyPe(), myrank,
                  (uintptr_t)scan, (uintptr_t)data, (uintptr_t)out,
                  from);
#endif
      *scan = out;
    }
#else
    // O(log n) method
    auto iter = my_heap_map.lower_bound((uintptr_t)data);
    if (iter != my_heap_map.end() &&
        /* (char *)iter->first <= data && */
        data <= (char *)iter->first + iter->second.size)
    {
      void * out = (char *)iter->second.addr + ((char *)data - (char *)iter->first);
#if PIEGLOBALS_DEBUG >= 2
      if (!quietModeRequested)
        CmiPrintf("pieglobals> [%d][%d][%zu] "
                  "0x%012" PRIxPTR ": found 0x%012" PRIxPTR ", writing 0x%012" PRIxPTR
                  " (in %s, to heap)\n",
                  CmiMyNode(), CmiMyPe(), myrank,
                  (uintptr_t)scan, (uintptr_t)data, (uintptr_t)out,
                  from);
#endif
      *scan = out;
    }
#endif
  }
}

static constexpr ptrdiff_t null_funcptr_diff = -1;

template <typename T>
static void calc_funcptr_isomorphic_offset(
  ptrdiff_t & diff, T funcptr,
  const void * src, const char * srcend,
  const void * isomorphic, const char * dst)
{
  auto ptr = (const char *)funcptr;
  if (ptr == nullptr)
  {
    diff = null_funcptr_diff;
    return;
  }

  if (ptr < src || srcend <= ptr)
    return;

  diff = (ptr - (const char *)src) + (dst - (const char *)isomorphic);
}

void AMPI_Rank_Setup(int myrank, int numranks, CmiIsomallocContext ctx)
{
  static std::atomic<size_t> rank_count{};
  const size_t localrank = rank_count++;

#if PIEGLOBALS_DEBUG >= 1
  if (!quietModeRequested)
    CmiPrintf("pieglobals> [%d][%d][%zu] AMPI_Rank_Setup(%d, %d, %p)\n",
              CmiMyNode(), CmiMyPe(), localrank, myrank, numranks, ctx.opaque);
#endif

  if (ctx.opaque == nullptr)
    CkAbort("pieglobals requires Isomalloc!");

  if (localrank == 0)
    pieglobalsdata.nodeleaderrank = myrank;

  // allocate privatized segments for this rank

  std::vector<char *> segment_allocations;
  for (const auto & obj : pieglobalsdata.objects)
  {
#if PIEGLOBALS_DEBUG >= 1
    if (!quietModeRequested)
      CmiPrintf("pieglobals> [%d][%d][%d] Processing %s\n",
                CmiMyNode(), CmiMyPe(), myrank, obj.name.c_str());
#endif

    auto allocation = (char *)CmiIsomallocContextPermanentAllocAlign(ctx, obj.align, obj.size);
    segment_allocations.emplace_back(allocation);

    for (const auto & item : obj.items)
    {
      char * dst = allocation + item.offset;
      char * src = obj.source + item.offset;
      size_t len = item.size;

#if PIEGLOBALS_DEBUG >= 1
      if (!quietModeRequested)
        CmiPrintf("pieglobals> [%d][%d][%d] [0x%012" PRIxPTR ", 0x%012" PRIxPTR ") --> [0x%012" PRIxPTR ", 0x%012" PRIxPTR ")\n",
                  CmiMyNode(), CmiMyPe(), myrank, (uintptr_t)src, (uintptr_t)src + len, (uintptr_t)dst, (uintptr_t)dst + len);
#endif

      memcpy(dst, src, len);
    }
  }

  const CmiIsomallocRegion memregion = CmiIsomallocContextGetUsedExtent(ctx);
  const auto memregionstart = (char *)memregion.start;
  const auto mallocstart = (char *)memregion.end;
  CmiIsomallocContextEnableRandomAccess(ctx);


  // determine entry point location

  if (localrank == 0)
  {
    for (const auto & obj : pieglobalsdata.objects)
    {
      char * allocation = segment_allocations[&obj - pieglobalsdata.objects.data()];

      pieglobalsdata.segment_allocation_offsets.emplace_back(allocation - memregionstart, (uintptr_t)obj.source, obj.size);

      for (const auto & item : obj.items)
      {
        char * dst = allocation + item.offset;
        char * src = obj.source + item.offset;
        size_t len = item.size;

        // calculate the offset of the function pointer into the isomalloc region
        // will be isomorphic for all ranks
        // avoids needing to store per-rank segment information between AMPI_Rank_Setup and main
        char * srcend = src + len;
        calc_funcptr_isomorphic_offset(pieglobalsdata.main_c_diff, pieglobalsdata.mainstruct.c, src, srcend, memregionstart, dst);
        calc_funcptr_isomorphic_offset(pieglobalsdata.main_f_diff, pieglobalsdata.mainstruct.f, src, srcend, memregionstart, dst);
      }
    }
  }


  // replay heap allocations from global constructors

  heap_map_t my_heap_map;
  for (const auto & entry : pieglobalsdata.global_constructor_heap)
  {
    const auto src = (void *)std::get<0>(entry);
    const size_t size = std::get<1>(entry);
    const size_t align = std::get<2>(entry);

    void * dst = align == 0
                 ? CmiIsomallocContextMalloc(ctx, size)
                 : CmiIsomallocContextMallocAlign(ctx, align, size);
    CmiEnforce(dst != nullptr);

    memcpy(dst, src, size);

    my_heap_map.emplace((uintptr_t)src, local_heap_entry{dst, size});
  }


  // apply fixups for pointers to locations in now-privatized segments

  const CmiIsomallocRegion srcheap = CmiIsomallocContextGetUsedExtent(pieglobalsdata.ctx);

  // scan the globals segment for pointers to code, globals, or the global constructor heap
  for (const auto & obj : pieglobalsdata.objects)
  {
    char * allocation = segment_allocations[&obj - pieglobalsdata.objects.data()];

    for (const auto & item : obj.items)
    {
      char * dst = allocation + item.offset;
      void * src = obj.source + item.offset;
      size_t len = item.size;

      if (!(item.flags & PF_X))
      {
        replace_in_range(myrank, dst, dst + len, my_heap_map, mallocstart, srcheap, segment_allocations, obj.name.c_str());
      }
    }
  }

  // scan the global constructor heap
  for (const auto & entry : my_heap_map)
  {
    const local_heap_entry & dst = entry.second;
    replace_in_range(myrank, dst.addr, (char *)dst.addr + dst.size, my_heap_map, mallocstart, srcheap, segment_allocations, "heap");
  }


  // apply mprotect permissions

  for (const auto & obj : pieglobalsdata.objects)
  {
    char * allocation = segment_allocations[&obj - pieglobalsdata.objects.data()];
    for (const auto & item : obj.items)
    {
      char * dst = allocation + item.offset;
      size_t len = item.size;

      int prot = 0;
      if (item.flags & PF_R)
        prot |= PROT_READ;
      if (item.flags & PF_W)
        prot |= PROT_WRITE;
      if (item.flags & PF_X)
        prot |= PROT_EXEC;

      CmiIsomallocContextProtect(ctx, dst, CMIALIGN(len, obj.align), prot);
    }
  }
}


// separate function so that setting a breakpoint is straightforward
static int ampi_pieglobals(int argc, char ** argv)
{
  const CthThread th = TCharm::get()->getThread();
  auto ctx = CmiIsomallocGetThreadContext(th);
  const CmiIsomallocRegion memregion = CmiIsomallocContextGetUsedExtent(ctx);
  const auto memregionstart = (char *)memregion.start;

  if (pieglobalsdata.main_c_diff != null_funcptr_diff)
  {
    auto newptr = (ampi_maintype)(memregionstart + pieglobalsdata.main_c_diff);
    return newptr(argc, argv);
  }
  else if (pieglobalsdata.main_f_diff != null_funcptr_diff)
  {
    auto newptr = (ampi_fmaintype)(memregionstart + pieglobalsdata.main_f_diff);
    newptr();
    return 0;
  }

  return 1;
}

int main(int argc, char ** argv)
{
  const int myrank = TCHARM_Element();

  TCHARM_Barrier();

  if (myrank == pieglobalsdata.nodeleaderrank)
  {
    pieglobalsdata.dealloc();

#if 0
    // clean up system-provided segments and global constructor heap BEFORE running
    // benefit: saves memory
    // benefit: replaces silent lapses in privatization with segfaults
    // benefit: avoids crashes during global destructors
    // detriment: no longer have original code in memory to cross-reference in debugger
    // showstopper: causes crashes inside libdl during program execution
    pieglobalscleanup();
#endif
  }

  TCHARM_Barrier();

  return ampi_pieglobals(argc, argv);
}


// for debugging purposes
// GDB: `call pieglobalsfind($rip)` or `call pieglobalsfind((void *)0x...)`
void * pieglobalsfind(void * ptr)
{
  const CthThread th = TCharm::get()->getThread();
  auto ctx = CmiIsomallocGetThreadContext(th);
  const CmiIsomallocRegion memregion = CmiIsomallocContextGetUsedExtent(ctx);

  if (ptr < memregion.start || memregion.end <= ptr)
  {
    fprintf(stderr, "pointer not in range of rank's data\n");
    return nullptr;
  }

  ptrdiff_t offset = (char *)ptr - (char *)memregion.start;

  for (const auto & seg : pieglobalsdata.segment_allocation_offsets)
  {
    ptrdiff_t allocation_offset = std::get<0>(seg);
    uintptr_t src = std::get<1>(seg);
    size_t size = std::get<2>(seg);

    if (allocation_offset <= offset && offset < allocation_offset + size)
    {
      uintptr_t out = src + (offset - allocation_offset);
      return (void *)out;
    }
  }

  fprintf(stderr, "other rank data (call stack or heap)\n");
  return nullptr;
}
