#if defined(HAVE_ELF)

#include "bin_api.h"

#include <stdio.h>
#include <fcntl.h>
#include <gelf.h>
#include <link.h>
#include <sysexits.h>
#include <unistd.h>

#include <sys/mman.h>

static ElfW(Shdr) symtab_shdr;
static Elf *elf = NULL;
static Elf_Data *symtab_data = NULL;

void *
bin_allocate_page()
{
  return mmap(NULL, pagesize, PROT_WRITE|PROT_READ|PROT_EXEC, MAP_ANON|MAP_PRIVATE|MAP_32BIT, -1, 0);
}

void
bin_update_image(int entry, void *trampee_addr)
{
  update_callqs(entry, trampee_addr);
}

void *
bin_find_symbol(char *sym, size_t *size)
{
  char *name = NULL;

  ElfW(Sym) *esym = (ElfW(Sym)*) symtab_data->d_buf;
  ElfW(Sym) *lastsym = (ElfW(Sym)*) ((char*) symtab_data->d_buf + symtab_data->d_size);

  for (; esym < lastsym; esym++){
    /* ignore weak/numeric/empty symbols */
    if ((esym->st_value == 0) ||
        (ELF32_ST_BIND(esym->st_info)== STB_WEAK) ||
        (ELF32_ST_BIND(esym->st_info)== STB_NUM))
      continue;


    name = elf_strptr(elf, symtab_shdr.sh_link, (size_t)esym->st_name);
    if (strcmp(name, sym) == 0) {
      if (size) {
        *size = esym->st_size;
      }
      return (void *)esym->st_value;
    }
  }
  return NULL;
}


void
bin_init()
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
        if ((symtab_data = elf_getdata(scn,symtab_data)) == NULL || symtab_data->d_size == 0) {
          errx(EX_DATAERR, "ruby has a broken symbol table. Is it stripped? "
                          "memprof only works on binaries that are not stripped!\n", filename);
        }
    }
  }

  if (!symtab_data) {
    errx(EX_DATAERR, "binary is stripped. memprof only works on binaries that are not stripped!", filename);
  }
}
#endif
