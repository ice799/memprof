#define _GNU_SOURCE
#include <err.h>
#include <fcntl.h>
#include <stddef.h>
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
#endif

struct tramp_inline {
  unsigned char jmp[1];
  uint32_t displacement;
  unsigned char pad[2];
} __attribute__((__packed__));

struct tramp_tbl_entry {
  unsigned char rbx_save[1];
  unsigned char mov[2];
  void *addr;
  unsigned char callq[2];
  unsigned char rbx_restore[1];
  unsigned char ret[1];
} __attribute__((__packed__));

struct inline_tramp_tbl_entry {
  unsigned char rex[1];
  unsigned char mov[1];
  unsigned char src_reg[1];
  uint32_t mov_displacement;

  struct {
    unsigned char push_rbx[1];
    unsigned char push_rbp[1];
    unsigned char save_rsp[3];
    unsigned char align_rsp[4];
    unsigned char mov[2];
    void *addr;
    unsigned char callq[2];
    unsigned char leave[1];
    unsigned char rbx_restore[1];
  } __attribute__((__packed__)) frame;

  unsigned char jmp[1];
  uint32_t jmp_displacement;
} __attribute__((__packed__));

static void *text_segment = NULL;
static unsigned long text_segment_len = 0;

/*
   trampoline specific stuff
 */
static struct tramp_tbl_entry *tramp_table = NULL;
static size_t tramp_size = 0;

/*
   inline trampoline specific stuff
 */

/* overwriting the mov instructions for inline tramps */
static size_t inline_tramp_size = 0;
static struct inline_tramp_tbl_entry *inline_tramp_table = NULL;

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
  VALUE ret = rb_newobj();
  printf("source = %s, line = %d, ret = %ld\n", ruby_sourcefile, ruby_sourceline, ret);
  return ret;
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

  struct inline_tramp_tbl_entry inline_ent = {
    .rex     = {'\x48'},
    .mov     = {'\x89'},
    .src_reg = {'\x05'},
    .mov_displacement = 0,

    .frame = {
      .push_rbx = {'\x53'},
      .push_rbp = {'\x55'},
      .save_rsp = {'\x48', '\x89', '\xe5'},
      .align_rsp = {'\x48', '\x83', '\xe4', '\xf0'},
      .mov = {'\x48', '\xbb'},
      .addr = error_tramp,
      .callq = {'\xff', '\xd3'},
      .leave = {'\xc9'},
      .rbx_restore = {'\x5b'},
    },

    .jmp  = {'\xe9'},
    .jmp_displacement = 0,
  };

  int pagesize = 4096;

  for (i = pagesize; i < INT_MAX - pagesize; i += pagesize) {
    tramp_table = mmap((void*)(NULL + i), 2*pagesize, PROT_WRITE|PROT_READ|PROT_EXEC, MAP_ANON|MAP_PRIVATE|MAP_FIXED, -1, 0);
    if (tramp_table != MAP_FAILED) {
      inline_tramp_table = (void *)tramp_table + pagesize;
      for (; j < pagesize/sizeof(struct tramp_tbl_entry); j ++ ) {
        memcpy(tramp_table + j, &ent, sizeof(struct tramp_tbl_entry));
      }
      for (j = 0; j < pagesize/sizeof(struct inline_tramp_tbl_entry); j++) {
        memcpy(inline_tramp_table + j, &inline_ent, sizeof(struct inline_tramp_tbl_entry));
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
  text_segment = getsectdatafromheader_64((struct mach_header_64*)header, "__TEXT", sectname, (uint64_t*)&text_segment_len);
  if (!text_segment)
    errx(EX_UNAVAILABLE, "Failed to locate the %s section", sectname);
}
#endif

static void
update_image(int entry, void *trampee_addr) {
#if defined(HAVE_ELF)
  update_callqs(entry, trampee_addr);
#elif defined(HAVE_MACH)
  // Modify any callsites residing inside the text segment of the executable itself
  set_text_segment((struct mach_header*)&_mh_execute_header, "__text");
  update_callqs(entry, trampee_addr);

  // Modify all dyld stubs in shared libraries that have been loaded
  int i, j, k;
  int header_count = _dyld_image_count();

  // Go through all the mach objects that are loaded into this process
  for (i=0; i < header_count; i++) {
    const struct mach_header *current_hdr = _dyld_get_image_header(i);
    int lc_count = current_hdr->ncmds;

    // this as a char* because we need to step it forward by an arbitrary number of bytes
    const char *lc = ((const char*) current_hdr) + sizeof(struct mach_header_64);

    // Check all the load commands in the object to see if they are segment commands
    for (j = 0; j < lc_count; j++) {
      if (((struct load_command*)lc)->cmd == LC_SEGMENT_64) {
        const struct segment_command_64 *seg = (const struct segment_command_64 *) lc;
        const struct section_64 * sect = (const struct section_64*)(lc + sizeof(struct segment_command_64));
        int section_count = (seg->cmdsize - sizeof(struct segment_command_64)) / sizeof(struct section_64);

        // Search the segment for a section containing dyld stub functions
        for (k=0; k < section_count; k++) {
          if (strncmp(sect->sectname, "__symbol_stub", 13) == 0) {
            set_text_segment((struct mach_header*)current_hdr, sect->sectname);
            text_segment += _dyld_get_image_vmaddr_slide(i);
            update_dyld_stubs(entry, trampee_addr);
          }
          sect++;
        }
      }
      lc += ((struct load_command*)lc)->cmdsize;
    }
  }
#endif
}

static void *
find_symbol(char *sym, long *size) {
#if defined(HAVE_ELF)
  char *name = NULL;

  /*now print the symbols*/
  ElfW(Sym) *esym = (ElfW(Sym)*) symtab_data->d_buf;
  ElfW(Sym) *lastsym = (ElfW(Sym)*) ((char*) symtab_data->d_buf + symtab_data->d_size);
  /* now loop through the symbol table and print it*/
  for (; esym < lastsym; esym++){
    if ((esym->st_value == 0) ||
        (ELF32_ST_BIND(esym->st_info)== STB_WEAK) ||
        (ELF32_ST_BIND(esym->st_info)== STB_NUM)) // ||
        //(ELF32_ST_TYPE(esym->st_info)!= STT_FUNC))
      continue;
    name = elf_strptr(elf, symtab_shdr.sh_link, (size_t)esym->st_name);
    if (strcmp(name, sym) == 0) {
        if (size) {
          *size = esym->st_size;
          printf("sym: %s, size: %lx\n", sym, *size);
        }
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
freelist_tramp(unsigned long rval) {
  printf("free: %ld\n", rval);
}

static void
hook_freelist(int entry) {
  long sizes[] = { 0, 0, 0 };
  void *sym1 = find_symbol("gc_sweep", &sizes[0]);
  void *sym2 = find_symbol("finalize_list", &sizes[1]);
  void *sym3 = find_symbol("rb_gc_force_recycle", &sizes[2]);
  void *freelist_callers[]= { sym1, sym2, sym3 };
  int max = 3;
  size_t i = 0;
  char *byte = freelist_callers[0];
  void *freelist = find_symbol("freelist", NULL);
  uint32_t mov_target =  0;
  void *aligned_addr = NULL;
  size_t count = 0;

  /* This is the stage 1 trampoline for hooking the inlined add_freelist
   * function .
   *
   * NOTE: The original instruction mov %reg, freelist is 7 bytes wide,
   * whereas jmpq $displacement is only 5 bytes wide. We *must* pad out
   * the next two bytes. This will be important to remember below.
   */
  struct tramp_inline tramp = {
    .jmp           = {'\xe9'},
    .displacement  = 0,
    .pad           = {'\x90', '\x90'},
  };

  struct inline_tramp_tbl_entry *inl_tramp_st2 = NULL;

  for (;i < max;) {
    /* make sure it is a mov instruction */
    if (byte[1] == '\x89') {

      /* Read the REX byte to make sure it is a mov that we care about */
      if ((byte[0] == '\x48') ||
          (byte[0] == '\x4c')) {

        /* Grab the target of the mov. REMEMBER: in this case the target is 
         * a 32bit displacment that gets added to RIP (where RIP is the adress of
         * the next instruction).
         */
        mov_target = *(uint32_t *)(byte + 3);

        /* Sanity check. Ensure that the displacement from freelist to the next
         * instruction matches the mov_target. If so, we know this mov is
         * updating freelist.
         */
        if ((freelist - (void *)(byte+7)) == mov_target) {
          /* Before the stage 1 trampoline gets written, we need to generate
           * the code for the stage 2 trampoline. Let's copy over the REX byte
           * and the byte which mentions the source register into the stage 2
           * trampoline.
           */
          inl_tramp_st2 = inline_tramp_table + entry;
          inl_tramp_st2->rex[0] = byte[0];
          inl_tramp_st2->src_reg[0] = byte[2];

          /* Setup the stage 1 trampoline. Calculate the displacement to
           * the stage 2 trampoline from the next instruction.
           *
           * REMEMBER!!!! The next instruction will be NOP after our stage 1
           * trampoline is written. This is 5 bytes into the structure, even
           * though the original instruction we overwrote was 7 bytes.
           */
          tramp.displacement = (uint32_t)((void *)(inl_tramp_st2) - (void *)(byte+5));

          /* Figure out what page the stage 1 tramp is gonna be written to, mark
           * it WRITE, write the trampoline in, and then remove WRITE permission.
           */
          aligned_addr = (void*)(((long)byte)&~(0xffff));
          mprotect(aligned_addr, (((void *)byte) - aligned_addr) + 10, PROT_READ|PROT_WRITE|PROT_EXEC);
          memcpy(byte, &tramp, sizeof(struct tramp_inline));
          mprotect(aligned_addr, (((void *)byte) - aligned_addr) + 10, PROT_READ|PROT_EXEC);

          /* Finish setting up the stage 2 trampoline. */

          /* calculate the displacement to freelist from the next instruction */
          inl_tramp_st2->mov_displacement = freelist - (void *)&(inl_tramp_st2->frame);

          /* jmp back to the instruction after stage 1 trampoline was inserted 
           *
           * This can be 5 or 7, it doesn't matter. If its 5, we'll hit our 2
           * NOPS. If its 7, we'll land directly on the next instruction.
           */
          inl_tramp_st2->jmp_displacement = (uint32_t)((void *)(byte + 7) -
                                                       (void *)(inline_tramp_table + entry + 1));

          /* write the address of our C level trampoline in to the structure */
          inl_tramp_st2->frame.addr = freelist_tramp;

          /* track the new entry and new trampoline size */
          entry++;
          inline_tramp_size++;
        }
      }
    }

    if (count >= sizes[i]) {
        count = 0;
        i ++;
        byte = freelist_callers[i];
    }
    count++;
    byte++;
  }
}

static void
insert_tramp(char *trampee, void *tramp) {
  void *trampee_addr = find_symbol(trampee, NULL);
  int entry = tramp_size;
  int inline_ent = inline_tramp_size;

  if (trampee_addr == NULL) {
    if (strcmp("add_freelist", trampee) == 0) {
      /* XXX super hack */
      inline_tramp_table[inline_tramp_size].frame.addr = tramp;
      inline_tramp_size++;
      hook_freelist(inline_ent);
    } else {
      return;
    }
  } else {
    tramp_table[tramp_size].addr = tramp;
    tramp_size++;
    update_image(entry, trampee_addr);
  }
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
  insert_tramp("add_freelist", freelist_tramp);
#if 0
  (void) elf_end(e);
  (void) close(fd);
#endif
  return;
#endif
}
