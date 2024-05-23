
#include <stdio.h>

#include "converse.h"
#include "cmitls.h"

#include <string.h>
#include <stdlib.h>
#if CMK_HAS_MALLOC_H
#include <malloc.h>
#endif

#if CMK_HAS_TLS_VARIABLES

extern int quietModeRequested;

/* These macros are needed for:
 * dlfcn.h: RTLD_DEFAULT
 * link.h: dl_iterate_phdr
 */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#ifndef __USE_GNU
# define __USE_GNU
#endif


// ----- TLS segment pointer access -----

/*
 * For a description of the system TLS implementations this file works with, see:
 * "ELF Handling For Thread-Local Storage"
 * https://www.akkadia.org/drepper/tls.pdf
 * Of note are sections 3.4.2 (IA-32, a.k.a. x86) and 3.4.6 (x86-64).
 * For ppc64: https://uclibc.org/docs/tls-ppc64.txt
 */

#if defined __x86_64__ || defined _M_X64
# define CMK_TLS_VARIANT2
# define CMK_TLS_X86
# define CMK_TLS_X86_MOV "movq"
# ifdef __APPLE__
#  define CMK_TLS_X86_REG "gs"
# else
#  define CMK_TLS_X86_REG "fs"
# endif
# define CMK_TLS_X86_WIDTH "8"
#elif defined __i386 || defined __i386__ || defined _M_IX86
# define CMK_TLS_VARIANT2
# define CMK_TLS_X86
# define CMK_TLS_X86_MOV "movl"
# define CMK_TLS_X86_REG "gs"
# define CMK_TLS_X86_WIDTH "4"
#elif defined __aarch64__
# define CMK_TLS_ARM64
# ifdef __APPLE__
#  define CMK_TLS_ARM64_REG "tpidrro_el0"
# else
#  define CMK_TLS_ARM64_REG "tpidr_el0"
# endif
# define CMK_TLS_DISPLACEMENT (0)
#elif defined __arm__
# define CMK_TLS_ARM32
# define CMK_TLS_DISPLACEMENT (0)
#elif defined __powerpc64__
# define CMK_TLS_PPC64
# define CMK_TLS_DISPLACEMENT (0x7000)
#else
# define CMK_TLS_SWITCHING_UNAVAILABLE
#endif

#ifdef __APPLE__

extern "C" {
void* getTLSForKey(size_t);
void setTLSForKey(size_t, void*);
}

void* getTLSForKey(size_t key)
{
  void * ptr = nullptr;
#if defined CMK_TLS_X86
  asm volatile (CMK_TLS_X86_MOV " %%" CMK_TLS_X86_REG ":0x0(,%1," CMK_TLS_X86_WIDTH "), %0\n"
                : "=&r"(ptr)
                : "r"(key));
#elif defined CMK_TLS_ARM64
  void ** block;
  asm volatile ("mrs %0, " CMK_TLS_ARM64_REG "\n"
                "and %0, %0, #-8\n"
                : "=&r"(block));
  ptr = block[key];
#endif
  return ptr;
}

void setTLSForKey(size_t key, void* newptr)
{
#if defined CMK_TLS_X86
  asm volatile (CMK_TLS_X86_MOV " %0, %%" CMK_TLS_X86_REG ":0x0(,%1," CMK_TLS_X86_WIDTH ")\n"
                :
                : "r"(newptr), "r"(key));
#elif defined CMK_TLS_ARM64
  void ** block;
  asm volatile ("mrs %0, " CMK_TLS_ARM64_REG "\n"
                "and %0, %0, #-8\n"
                : "=&r"(block));
  block[key] = newptr;
#endif
}

#else

extern "C" {
void* getTLS();
void setTLS(void*);
}

void* getTLS()
{
  void * ptr = nullptr;
#if defined CMK_TLS_X86
  asm volatile (CMK_TLS_X86_MOV " %%" CMK_TLS_X86_REG ":0x0, %0\n"
                : "=&r"(ptr));
#elif defined CMK_TLS_PPC64
  asm volatile ("addis %0, 13, 0\n"
                : "=&r"(ptr));
#elif defined CMK_TLS_ARM64
  asm volatile ("mrs %0, " CMK_TLS_ARM64_REG "\n"
                : "=&r"(ptr));
#elif defined CMK_TLS_ARM32
  asm volatile ("mrc p15, 0, %0, c13, c0, 3\n"
                "bic %0, %0, #3\n"
                : "=&r"(ptr));
#endif
  return ptr;
}

void setTLS(void* newptr)
{
#if defined CMK_TLS_X86
  asm volatile (CMK_TLS_X86_MOV " %0, %%" CMK_TLS_X86_REG ":0x0\n"
                :
                : "r"(newptr));
#elif defined CMK_TLS_PPC64
  asm volatile ("addis 13, %0, 0\n"
                :
                : "r"(newptr));
#elif defined CMK_TLS_ARM64
  asm volatile ("msr " CMK_TLS_ARM64_REG ", %0\n"
                :
                : "r"(newptr));
#elif defined CMK_TLS_ARM32
  asm volatile ("mcr p15, 0, %0, c13, c0, 3\n" // doesn't work, register is read-only in user mode
                :
                : "r"(newptr));
#endif
}

#endif


// ----- TLS segment size determination -----

static tlsdesc_t CmiTLSDescription;

#if CMK_HAS_DL_ITERATE_PHDR

# include <link.h>

static int count_tls_sizes(struct dl_phdr_info* info, size_t size, void* data)
{
  size_t i;
  auto t = (tlsdesc_t *)data;

  for (i = 0; i < info->dlpi_phnum; i++)
  {
    const ElfW(Phdr) * hdr = &info->dlpi_phdr[i];
    if (hdr->p_type == PT_TLS)
    {
      const size_t align = hdr->p_align;
      t->size += CMIALIGN(hdr->p_memsz, align);
      if (t->align < align)
        t->align = align;
    }
  }

  return 0;
}

static inline void CmiTLSStatsInit()
{
  CmiTLSDescription.size = 0;
  CmiTLSDescription.align = 0;
  dl_iterate_phdr(count_tls_sizes, &CmiTLSDescription); /* count all PT_TLS sections */

#ifdef CMK_TLS_VARIANT2
  CmiTLSDescription.offset = CMIALIGN(sizeof(void *), CmiTLSDescription.align);
#else
  CmiTLSDescription.offset = CMIALIGN(sizeof(void *) * 2, CmiTLSDescription.align);
#endif
  CmiTLSDescription.size += CmiTLSDescription.offset;
}

#elif defined __APPLE__

static constexpr size_t global_align = 16; // Apple uses alignment by 16

# include <map>

struct CmiTLSSegment
{
  const void * ptr;
  size_t size;
};
static std::map<unsigned long, CmiTLSSegment> CmiTLSSegments;


// things needed for GNU emutls

# include <vector>
# include <pthread.h>

struct CmiTLSEmuTLSObject
{
  size_t size;
  size_t align;
  uintptr_t offset;
  const void * initial;
};

static size_t CmiTLSSizeWithoutEmuTLS;
static size_t CmiTLSEmuTLSNumObjects;
static constexpr size_t CmiTLSEmuTLSArbitraryExtra = 32;
static unsigned long CmiTLSEmuTLSKey;
static std::vector<CmiTLSEmuTLSObject *> CmiTLSEmuTLSObjects;

static inline size_t CmiTLSEmuTLSGetAlignedSize(size_t numobjects)
{
  const size_t size = sizeof(void *) * (2 /* emutls_array */ + numobjects + CmiTLSEmuTLSArbitraryExtra);
  return CMIALIGN(size, global_align);
}


// things needed for TLV sections

# include <mach-o/dyld.h>
# include <mach-o/nlist.h>

#if __LP64__
  typedef struct mach_header_64 macho_header;
# define MACHO_HEADER_MAGIC MH_MAGIC_64
# define LC_SEGMENT_COMMAND LC_SEGMENT_64
  typedef struct segment_command_64 macho_segment_command;
  typedef struct section_64 macho_section;
  typedef struct nlist_64 macho_nlist;
#else
  typedef struct mach_header macho_header;
# define MACHO_HEADER_MAGIC MH_MAGIC
# define LC_SEGMENT_COMMAND LC_SEGMENT
  typedef struct segment_command macho_segment_command;
  typedef struct section macho_section;
  typedef struct nlist macho_nlist;
#endif

struct TLVDescriptor
{
  void * (*thunk)(struct TLVDescriptor *);
  unsigned long key;
  unsigned long offset;
};


static inline void CmiTLSStatsInit()
{
  size_t totalsize = 0;
  size_t total_emutls_num = 0;

  // Parse all Mach-O headers to get TLS/TLV information.
  // Adapted from threadLocalVariables.c in dyld.

  for (uint32_t c = 0; c < _dyld_image_count(); ++c)
  {
    const struct mach_header * const mh_orig = _dyld_get_image_header(c);
    if (mh_orig == nullptr)
      continue;

    CmiEnforce(mh_orig->magic == MACHO_HEADER_MAGIC);
    const auto mh = (const macho_header *)mh_orig;
    const auto mh_addr = (const char *)mh;
    // const char * const name = _dyld_get_image_name(c);

    uint64_t text_vmaddr = 0, linkedit_vmaddr = 0, linkedit_fileoff = 0;
    intptr_t slide = 0;
    bool slideComputed = false;

    const uint8_t * start = nullptr;
    unsigned long size = 0;
    unsigned long key = 0;
    bool haveKey = false;

    const uint32_t cmd_count = mh->ncmds;
    const auto cmds = (const struct load_command *)(mh_addr + sizeof(macho_header));
    const struct load_command * cmd = cmds;
    for (uint32_t i = 0; i < cmd_count; ++i)
    {
      const auto lc_type = cmd->cmd & ~LC_REQ_DYLD;
      if (lc_type == LC_SEGMENT_COMMAND)
      {
        const auto seg = (const macho_segment_command *)cmd;

        if (!slideComputed && seg->filesize != 0)
        {
          slide = (uintptr_t)mh - seg->vmaddr;
          slideComputed = true;
        }

        if (strcmp(seg->segname, SEG_TEXT) == 0)
          text_vmaddr = seg->vmaddr;
        else if (strcmp(seg->segname, SEG_LINKEDIT) == 0)
        {
          linkedit_vmaddr = seg->vmaddr;
          linkedit_fileoff = seg->fileoff;
        }

        // look for TLV sections, used by Apple's clang

        const auto sectionsStart = (const macho_section *)((const char *)seg + sizeof(macho_segment_command));
        const auto sectionsEnd = sectionsStart + seg->nsects;
        for (auto sect = sectionsStart; sect < sectionsEnd; ++sect)
        {
          const auto section_type = sect->flags & SECTION_TYPE;
          if (section_type == S_THREAD_LOCAL_ZEROFILL || section_type == S_THREAD_LOCAL_REGULAR)
          {
            if (start == nullptr)
            {
              // first of N contiguous TLV template sections: record as if this were the only section
              start = (const uint8_t *)(sect->addr + slide);
              size = sect->size;
            }
            else
            {
              // non-first of N contiguous TLV template sections: accumulate values
              const auto newEnd = (const uint8_t *)(sect->addr + slide + sect->size);
              size = newEnd - start;
            }
          }
          else if (section_type == S_THREAD_LOCAL_VARIABLES)
          {
            const auto tlvstart = (const TLVDescriptor *)(sect->addr + slide);
            const auto tlvend = (const TLVDescriptor *)(sect->addr + slide + sect->size);
            for (const TLVDescriptor * d = tlvstart; d < tlvend; ++d)
            {
              if (haveKey)
              {
                CmiEnforce(d->key == key);
              }
              else
              {
                key = d->key;
                haveKey = true;
              }
            }
          }
        }
      }
      else if (lc_type == LC_SYMTAB && text_vmaddr)
      {
        // look through all symbols for GNU emutls, used by GCC

        const auto symcmd = (const struct symtab_command *)cmd;
        auto strtab = (const char *)(linkedit_vmaddr + (symcmd->stroff - linkedit_fileoff) + slide);
        auto symtab = (const macho_nlist *)(linkedit_vmaddr + (symcmd->symoff - linkedit_fileoff) + slide);

        for (const macho_nlist * nl = symtab, * const nl_end = symtab + symcmd->nsyms; nl < nl_end; ++nl)
        {
          if ((nl->n_type & (N_TYPE & N_SECT)) != N_SECT)
            continue;

          static const char emutls_prefix[] = "___emutls_v.";
          static constexpr size_t emutls_prefix_len = sizeof(emutls_prefix)-1;
          const char * const symname = strtab + nl->n_un.n_strx;
          if (strncmp(symname, emutls_prefix, emutls_prefix_len) == 0)
          {
            const auto obj = (CmiTLSEmuTLSObject *)(nl->n_value + slide);

            // Save this object pointer for later. We need all fields in the struct.
            // Don't consider the placeholders for migration, since we need emutls to allocate them itself.
            static const char CmiTLSPlaceholder_prefix[] = "CmiTLSPlaceholder";
            if (strncmp(symname + emutls_prefix_len, CmiTLSPlaceholder_prefix, sizeof(CmiTLSPlaceholder_prefix)-1) != 0)
              CmiTLSEmuTLSObjects.emplace_back(obj);

            ++total_emutls_num;
          }
        }
      }

      cmd = (const struct load_command *)(((const char *)cmd) + cmd->cmdsize);
    }

    // Add any TLV section found in this image.
    CmiEnforce(haveKey == (size > 0));
    if (size > 0)
    {
      const size_t alignedsize = CMIALIGN(size, global_align);

      const bool didInsert = CmiTLSSegments.emplace(key, CmiTLSSegment{start, alignedsize}).second;
      CmiEnforce(didInsert);

      totalsize += alignedsize;
    }
  }

  // Tabulate the results.

  CmiTLSSizeWithoutEmuTLS = totalsize;

  if (total_emutls_num > 0)
  {
    // Predict the value that emutls_key will be after emutls_init is called.
    pthread_key_t fakekey;
    pthread_key_create(&fakekey, nullptr);
    CmiTLSEmuTLSKey = fakekey + 1;
    CmiTLSEmuTLSNumObjects = total_emutls_num;

    totalsize += CmiTLSEmuTLSGetAlignedSize(total_emutls_num);

    // Assign emutls offsets.
    // Leave room for the placeholders to take the first indexes so that the first one calls emutls_init.
    size_t migratable_emutls_idx = total_emutls_num - CmiTLSEmuTLSObjects.size();
    for (CmiTLSEmuTLSObject * obj : CmiTLSEmuTLSObjects)
    {
      obj->offset = ++migratable_emutls_idx;
      size_t size = CMIALIGN(obj->size, obj->align);
      size = CMIALIGN(size, global_align);
      totalsize += size;
    }
  }

  CmiTLSDescription.size = totalsize;
  CmiTLSDescription.align = global_align;
}

#elif CMK_HAS_ELF_H && CMK_DLL_USE_DLOPEN && CMK_HAS_RTLD_DEFAULT

# include <dlfcn.h>
# define CMK_TLS_NO_SHARED

static void* CmiTLSExecutableStart;

static inline Addr getCodeSegAddr()
{
  return (Addr) CmiTLSExecutableStart;
}

static inline Ehdr* getELFHeader()
{
  return (Ehdr*) getCodeSegAddr();
}

static inline Phdr* getProgramHeader(Ehdr* ehdr)
{
  return (Phdr*)((char *)ehdr + ehdr->e_phoff);
}

Phdr* getTLSPhdrEntry()
{
  int phnum, i;
  Ehdr* elfHeader;
  Phdr* progHeader;

  elfHeader = getELFHeader();
  if (elfHeader == NULL)
    return NULL;

  phnum = elfHeader->e_phnum;
  progHeader = getProgramHeader(elfHeader);
  for (i = 0; i < phnum; i++)
  {
    if (progHeader[i].p_type == PT_TLS)
    {
#if CMK_ERROR_CHECKING
      /* sanity check */
      /* align is power of 2 */
      int align = progHeader[i].p_align;
      CmiAssert(align > 0 && (align & (align-1)) == 0);
      /* total size is not less than the size of .tdata (initializer data) */
      CmiAssert(progHeader[i].p_memsz >= progHeader[i].p_filesz);
#endif
      return &progHeader[i];
    }
  }
  return NULL;
}

static void CmiTLSStatsInit()
{
  /* Use dynamic linking in case Charm++ shared objects are used by a binary lacking
   * conv-static.o, such as in the case of Charm4py. */
  void** pCmiExecutableStart = (void**)dlsym(RTLD_DEFAULT, "CmiExecutableStart");
  if (pCmiExecutableStart != NULL)
    CmiTLSExecutableStart = *pCmiExecutableStart;
  else
    CmiPrintf("Charm++> Error: \"CmiExecutableStart\" symbol not found. -tlsglobals disabled.\n");

  Phdr* phdr = getTLSPhdrEntry();
  if (phdr != NULL)
  {
    const size_t align = CmiTLSDescription.align = phdr->p_align;
    CmiTLSDescription.size = CMIALIGN(phdr->p_memsz, align);

#ifdef CMK_TLS_VARIANT2
    CmiTLSDescription.offset = CMIALIGN(sizeof(void *), align);
#else
    CmiTLSDescription.offset = CMIALIGN(sizeof(void *) * 2, align);
#endif
    CmiTLSDescription.size += CmiTLSDescription.offset;
  }
}

#else

static inline void CmiTLSStatsInit()
{
}

#endif


// ----- CmiTLS implementation -----

extern thread_local int CmiTLSPlaceholderInt;
thread_local int CmiTLSPlaceholderInt = -1;

void CmiTLSInit(tlsseg_t * newThreadParent)
{
#ifdef CMK_TLS_SWITCHING_UNAVAILABLE
  CmiAbort("TLS globals are not supported.");
#else
  if (CmiMyRank() == 0)
  {
    if (!quietModeRequested && CmiMyPe() == 0)
    {
      CmiPrintf("Charm++> -tlsglobals enabled for privatization of thread-local variables.\n");
#ifdef CMK_TLS_NO_SHARED
      CmiPrintf("Charm++> Warning: Unable to examine TLS segments of shared objects.\n");
#endif
    }

    CmiTLSStatsInit();
  }

#ifdef __APPLE__
  // Allocate our own thread-local storage for each PE's parent so that CmiTLSSegmentSet
  // is simple and tlsseg_t does not need to contain a std::map of keys to pointers.

  CmiNodeAllBarrier();

  void * memseg = CmiAlignedAlloc(CmiTLSDescription.align, CmiTLSDescription.size);
  memset(memseg, 0, CmiTLSDescription.size);

  auto data = (char *)memseg;
  for (const auto & tlv : CmiTLSSegments)
  {
    const void * const ptr = tlv.second.ptr;
    const size_t size = tlv.second.size;

    memcpy(data, ptr, size);

    data += size;
  }

  const size_t total_emutls_num = CmiTLSEmuTLSNumObjects;
  if (total_emutls_num > 0)
  {
    *(size_t *)data = total_emutls_num + CmiTLSEmuTLSArbitraryExtra;
    auto ptrs = (void **)data;
    size_t my_emutls_size = CmiTLSEmuTLSGetAlignedSize(total_emutls_num);
    data += my_emutls_size;

    // Fill in emutls pointers and copy initial values.
    for (CmiTLSEmuTLSObject * obj : CmiTLSEmuTLSObjects)
    {
      size_t size = CMIALIGN(obj->size, obj->align);
      size = CMIALIGN(size, global_align);

      ptrs[obj->offset] = data;

      if (obj->initial != nullptr)
        memcpy(data, obj->initial, obj->size);

      data += size;
      my_emutls_size += size;
    }

    CmiNodeAllBarrier();

    if (CmiMyRank() == 0)
    {
      // Add this entry for CmiTLSSegmentSet.
      const bool didInsert = CmiTLSSegments.emplace(CmiTLSEmuTLSKey, CmiTLSSegment{nullptr, my_emutls_size}).second;
      CmiEnforce(didInsert);
    }

    CmiNodeAllBarrier();
  }

  newThreadParent->memseg = memseg;

  // Replace the default key values with our custom ones packed into a single buffer.
  CmiTLSSegmentSet(newThreadParent);
#else
  newThreadParent->memseg = (Addr)getTLS();
#endif

  // If emutls is active, setting these will eventually call emutls_init, which we need.
  CmiTLSPlaceholderInt = CmiMyPe();
#endif
}

tlsdesc_t CmiTLSGetDescription()
{
  return CmiTLSDescription;
}

void CmiTLSCreateSegUsingPtr(const tlsseg_t * threadParent, tlsseg_t * t, void * ptr)
{
  auto memseg = (char *)ptr;

#ifdef __APPLE__
  memcpy(memseg, threadParent->memseg, CmiTLSDescription.size);

  // Fill in emutls pointers.
  auto ptrs = (void **)((char *)memseg + CmiTLSSizeWithoutEmuTLS);
  auto data = (char *)ptrs + CmiTLSEmuTLSGetAlignedSize(CmiTLSEmuTLSNumObjects);
  for (CmiTLSEmuTLSObject * obj : CmiTLSEmuTLSObjects)
  {
    size_t size = CMIALIGN(obj->size, obj->align);
    size = CMIALIGN(size, global_align);

    ptrs[obj->offset] = data;

    data += size;
  }

  t->memseg = memseg;
#else
#ifdef CMK_TLS_VARIANT2
  auto src = (const char *)threadParent->memseg - (CmiTLSDescription.size - CmiTLSDescription.offset);
  auto threadpointer = (Addr)(memseg + CmiTLSDescription.size - CmiTLSDescription.offset);
  auto selfreference = (void **)threadpointer;
#else
  auto src = (const char *)threadParent->memseg - CMK_TLS_DISPLACEMENT;
  auto selfreference = (void **)memseg;
  auto threadpointer = (Addr)(memseg + CMK_TLS_DISPLACEMENT);
#endif

  memcpy(memseg, src, CmiTLSDescription.size);
  t->memseg = threadpointer;
  *selfreference = selfreference;
#endif
}

void * CmiTLSGetBuffer(tlsseg_t * t)
{
#if defined __APPLE__
  return t->memseg;
#elif defined CMK_TLS_VARIANT2
  return (char *)t->memseg - (CmiTLSDescription.size - CmiTLSDescription.offset);
#else
  return (char *)t->memseg - CMK_TLS_DISPLACEMENT;
#endif
}

void CmiTLSSegmentSet(tlsseg_t * next)
{
#ifdef __APPLE__
  auto data = (char *)next->memseg;
  for (const auto & tlv : CmiTLSSegments)
  {
    const unsigned long key = tlv.first;
    const size_t size = tlv.second.size;

    setTLSForKey(key, data);
    data += size;
  }
#else
  setTLS((void*)next->memseg);
#endif
}

#endif
