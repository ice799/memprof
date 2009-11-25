#define _GNU_SOURCE
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sysexits.h>
#include <sys/mman.h>
#include <err.h>

#include <ruby.h>
#include <intern.h>

#if defined(HAVE_ELF)
#include <gelf.h>
#include <link.h>
#elif defined(HAVE_MACH)
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach-o/ldsyms.h>
#include <dlfcn.h>
#endif

struct tramp_tbl_entry {
  unsigned char rbx_save[1];
  unsigned char mov[2];
  void *addr;
  unsigned char callq[2];
  unsigned char rbx_restore[1];
  unsigned char ret[1];
} __attribute__((__packed__));

static void *text_segment = NULL;
static unsigned long text_segment_len = 0;

/*
  trampoline specific stuff
*/
static struct tramp_tbl_entry *tramp_table = NULL;
static size_t tramp_size = 0;

/*
  ELF specific stuff
*/
#if defined(HAVE_ELF)
static ElfW(Shdr) symtab_shdr;
static Elf *elf = NULL;
static Elf_Data *symtab_data = NULL;
#endif

static void
error_tramp() {
  printf("WARNING: NO TRAMPOLINE SET.\n");
  return;
}

static VALUE
newobj_tramp() {
  printf("source = %s, line = %d\n", ruby_sourcefile, ruby_sourceline);
  return rb_newobj();
}

static void
create_tramp_table() {
  int i, j = 0;

  struct tramp_tbl_entry ent = {
    .rbx_save      = {'\x53'},                // push rbx
    .mov           = {'\x48', '\xbb'},        // mov addr into rbx
    .addr          = error_tramp,             // ^^^
    .callq         = {'\xff', '\xd3'},        // callq rbx
    .rbx_restore   = {'\x5b'},                // pop rbx
    .ret           = {'\xc3'},                // ret
  };

  int pagesize = 4096;

  for (; i < INT_MAX - pagesize; i += pagesize) {
    tramp_table = mmap((void*)(NULL + i), pagesize, PROT_WRITE|PROT_READ|PROT_EXEC, MAP_ANON|MAP_PRIVATE, -1, 0);
    if (tramp_table != MAP_FAILED) {
      for (; j < pagesize/sizeof(struct tramp_tbl_entry); j ++ ) {
        memcpy(tramp_table + j, &ent, sizeof(struct tramp_tbl_entry));
      }
      return;
    }
  }
  errx(EX_UNAVAILABLE, "Failed to allocate a page for stage 1 trampoline table");
}

static void
update_callqs(int entry, void *trampee_addr) {
  char *byte = text_segment;
  size_t count = 0;
  int fn_addr = 0;
  void *aligned_addr = NULL;

  for(; count < text_segment_len; count++) {
    if (*byte == '\xe8') {
      fn_addr = *(int *)(byte+1);
      if (((void *)trampee_addr - (void *)(byte+5)) == fn_addr) {
        aligned_addr = (void*)(((long)byte+1)&~(0xffff));
        mprotect(aligned_addr, (((void *)byte+1) - aligned_addr) + 10, PROT_READ|PROT_WRITE|PROT_EXEC);
        *(int  *)(byte+1) = (uint32_t)((void *)(tramp_table + entry) - (void *)(byte + 5));
        mprotect(aligned_addr, (((void *)byte+1) - aligned_addr) + 10, PROT_READ|PROT_EXEC);
      }
    }
    byte++;
  }
}

#ifdef HAVE_MACH
static void
update_dyld_stubs(int entry, void *trampee_addr) {
  char *byte = text_segment;
  size_t count = 0;

  for(; count < text_segment_len; count++) {
    if (*byte == '\xff') {
      int off = *(int *)(byte+2);
      if (trampee_addr == (void*)(*(long long*)(byte + 6 + off))) {
        *(long long*)(byte + 6 + off) = tramp_table[entry].addr;
      }
    }
    byte++;
  }
}
#endif

#ifdef HAVE_MACH
static void
set_text_segment(struct mach_header *header, const char *sectname) {
#if defined(__x86_64__) && defined(MH_MAGIC_64)
  text_segment = getsectdatafromheader_64((struct mach_header_64*)header, "__TEXT", sectname, (uint64_t*)&text_segment_len);
#else
  text_segment = getsectdatafromheader(header, "__TEXT", sectname, (uint32_t*)&text_segment_len);
#endif
  if (!text_segment)
    errx(EX_UNAVAILABLE, "Failed to locate the %s section", sectname);
}
#endif

static void
update_image(int entry, void *trampee_addr) {
#if defined(HAVE_ELF)
  update_callqs(entry, trampee_addr);
#elif defined(HAVE_MACH)
  Dl_info info;

  if (!dladdr(trampee_addr, &info))
    errx(EX_UNAVAILABLE, "Failed to locate the mach header that contains function at %p", trampee_addr);

  struct mach_header *header = (struct mach_header*) info.dli_fbase;

  if (header == (struct mach_header*)&_mh_execute_header) {
    set_text_segment(header, "__text");
    update_callqs(entry, trampee_addr);
  }
  else {
    set_text_segment(header, "__symbol_stub1");
    unsigned long cur_idx, real_idx = 0;
    unsigned long count = _dyld_image_count();
    for (cur_idx = 0; cur_idx < count; cur_idx++) {
      if (_dyld_get_image_header(cur_idx) == header) {
        real_idx = cur_idx;
        break;
      }
    }
    text_segment = text_segment + _dyld_get_image_vmaddr_slide(real_idx);
    update_dyld_stubs(entry, trampee_addr);
  }
#endif
}

static void *
find_symbol(char *sym) {
#if defined(HAVE_ELF)
  char *name = NULL;

  /*now print the symbols*/
  ElfW(Sym) *esym = (ElfW(Sym)*) symtab_data->d_buf;
  ElfW(Sym) *lastsym = (ElfW(Sym)*) ((char*) symtab_data->d_buf + symtab_data->d_size);
  /* now loop through the symbol table and print it*/
  for (; esym < lastsym; esym++){
    if ((esym->st_value == 0) ||
        (ELF32_ST_BIND(esym->st_info)== STB_WEAK) ||
        (ELF32_ST_BIND(esym->st_info)== STB_NUM) ||
        (ELF32_ST_TYPE(esym->st_info)!= STT_FUNC))
      continue;
    name = elf_strptr(elf, symtab_shdr.sh_link, (size_t)esym->st_name);
    if (strcmp(name, sym) == 0) {
       return (void *)esym->st_value;
    }
  }
  return NULL;
#elif defined(HAVE_MACH)
  void *ptr = NULL;
  _dyld_lookup_and_bind((const char*)sym, &ptr, NULL);
  return ptr;
#endif
}

static void
insert_tramp(char *trampee, void *tramp) {
  void *trampee_addr = find_symbol(trampee);
  int entry = tramp_size;
  tramp_table[tramp_size].addr = tramp;
  tramp_size++;
  update_image(entry, trampee_addr);
}

void Init_memprof()
{
#if defined(HAVE_ELF)
  int fd;
  ElfW(Shdr) shdr;
  size_t shstrndx;
  char *filename; 
  Elf_Scn *scn;

  if (elf_version(EV_CURRENT) == EV_NONE)
    errx(EX_SOFTWARE, "ELF library initialization failed: %s",
        elf_errmsg(-1));

  asprintf(&filename, "/proc/%ld/exe", (long)getpid());

  if ((fd = open(filename, O_RDONLY, 0)) < 0)
    err(EX_NOINPUT, "open \%s\" failed", filename);

  if ((elf = elf_begin(fd, ELF_C_READ, NULL)) == NULL)
    errx(EX_SOFTWARE, "elf_begin() failed: %s.",
        elf_errmsg(-1));

  if (elf_kind(elf) != ELF_K_ELF)
    errx(EX_DATAERR, "%s is not an ELF object.", filename);

  if (elf_getshstrndx(elf, &shstrndx) == 0)
    errx(EX_SOFTWARE, "getshstrndx() failed: %s.",
        elf_errmsg(-1));

  scn = NULL;

  while ((scn = elf_nextscn(elf, scn)) != NULL) {
    if (gelf_getshdr(scn, &shdr) != &shdr)
      errx(EX_SOFTWARE, "getshdr() failed: %s.",
          elf_errmsg(-1));

    if (shdr.sh_type == SHT_PROGBITS &&
        (shdr.sh_flags == (SHF_ALLOC | SHF_EXECINSTR)) &&
        strcmp(elf_strptr(elf, shstrndx, shdr.sh_name), ".text") == 0) {
      
        text_segment = (void *)shdr.sh_addr;
        text_segment_len = shdr.sh_size;
    } else if (shdr.sh_type == SHT_SYMTAB) {
        symtab_shdr = shdr;
        if ((symtab_data = elf_getdata(scn,symtab_data)) == 0 || symtab_data->d_size == 0) {
          return;
        }
    }
  }

#endif
  create_tramp_table();
#if defined(HAVE_MACH)
  insert_tramp("_rb_newobj", newobj_tramp);
#elif defined(HAVE_ELF)
  insert_tramp("rb_newobj", newobj_tramp);
#if 0
  (void) elf_end(e);
  (void) close(fd);
#endif
  return;
#endif
}
