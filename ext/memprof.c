#define _GNU_SOURCE
#include <err.h>
#include <fcntl.h>
#include <gelf.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <link.h>
#include <sysexits.h>
#include <sys/mman.h>

#include <ruby.h>
#include <intern.h>

struct tramp_tbl_entry {
  unsigned char mov[2];
  long long addr;
  unsigned char callq[2];
  unsigned char ret;
  unsigned char pad[3];
} __attribute__((__packed__));;


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
static ElfW(Shdr) symtab_shdr;
static Elf *elf = NULL;
static Elf_Data *symtab_data = NULL;


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
  int i = 0;

  struct tramp_tbl_entry ent = {
    .mov = {'\x48', '\xbb'},
    .addr = (long long)&error_tramp,
    .callq = { '\xff', '\xd3' },
    .ret = '\xc3',
    .pad =  { '\x90', '\x90', '\x90'},
  };

  tramp_table = mmap(NULL, 4096, PROT_WRITE|PROT_READ|PROT_EXEC, MAP_32BIT|MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
  if (tramp_table != MAP_FAILED) {
    for (; i < 4096/sizeof(struct tramp_tbl_entry); i ++ ) {
      memcpy(tramp_table + i, &ent, sizeof(struct tramp_tbl_entry));
    }
  }
}

static void
update_image(int entry, void *trampee_addr) {
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

static void *
find_symbol(char *sym) {
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
}

static void
insert_tramp(char *trampee, void *tramp) {
  void *trampee_addr = find_symbol(trampee);
  int entry = tramp_size;
  tramp_table[tramp_size].addr = (long long)tramp;
  tramp_size++;
  update_image(entry, trampee_addr);
}

void Init_memprof()
{
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


  create_tramp_table();

  insert_tramp("rb_newobj", newobj_tramp);
#if 0
  (void) elf_end(e);
  (void) close(fd);
#endif
  return;
}
