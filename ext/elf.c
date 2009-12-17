#if defined(HAVE_ELF)

#include "bin_api.h"
#define _GNU_SOURCE

#include <dwarf.h>
#include <err.h>
#include <fcntl.h>
#include <gelf.h>
#include <libdwarf.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include <sys/mman.h>

static ElfW(Shdr) symtab_shdr;
static Elf *elf = NULL;
static Elf_Data *symtab_data = NULL;

static Dwarf_Debug dwrf = NULL;

void *
bin_allocate_page()
{
  void * ret = NULL;
  ret = mmap(NULL, pagesize, PROT_WRITE|PROT_READ|PROT_EXEC, MAP_ANON|MAP_PRIVATE|MAP_32BIT, -1, 0);

  if (ret != MAP_FAILED) {
    memset(ret, 0x90, pagesize);
  }

  return ret;
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

static Dwarf_Die
check_die(Dwarf_Die die, char *search, Dwarf_Half type)
{
  char *name = 0;
  Dwarf_Error error = 0;
  Dwarf_Half tag = 0;
  int ret = 0;
  int res = dwarf_diename(die,&name,&error);
  if (res == DW_DLV_ERROR) {
    printf("Error in dwarf_diename\n");
    exit(1);
  }
  if (res == DW_DLV_NO_ENTRY) {
    return 0;
  }

  res = dwarf_tag(die,&tag,&error);
  if (res != DW_DLV_OK) {
    printf("Error in dwarf_tag\n");
    exit(1);
  }

  if (tag == type && strcmp(name, search) == 0){
    //printf("tag: %d name: '%s' die: %p\n",tag,name,die);
    ret = 1;
  }

  dwarf_dealloc(dwrf,name,DW_DLA_STRING);

  return ret ? die : 0;
}

static Dwarf_Die
search_dies(Dwarf_Die die, char *name, Dwarf_Half type)
{
  int res = DW_DLV_ERROR;
  Dwarf_Die cur_die=die;
  Dwarf_Die child = 0;
  Dwarf_Error error;
  Dwarf_Die ret = 0;

  ret = check_die(cur_die, name, type);
  if (ret)
    return ret;

  for(;;) {
    Dwarf_Die sib_die = 0;
    res = dwarf_child(cur_die,&child,&error);
    if (res == DW_DLV_ERROR) {
      printf("Error in dwarf_child\n");
      exit(1);
    }
    if (res == DW_DLV_OK) {
      ret = search_dies(child,name,type);
      if (ret) {
        if (cur_die != die && cur_die != ret)
          dwarf_dealloc(dwrf,cur_die,DW_DLA_DIE);
        return ret;
      }
    }
    /* res == DW_DLV_NO_ENTRY */

    res = dwarf_siblingof(dwrf,cur_die,&sib_die,&error);
    if (res == DW_DLV_ERROR) {
      printf("Error in dwarf_siblingof\n");
      exit(1);
    }
    if (res == DW_DLV_NO_ENTRY) {
      /* Done at this level. */
      break;
    }
    /* res == DW_DLV_OK */

    if (cur_die != die)
      dwarf_dealloc(dwrf,cur_die,DW_DLA_DIE);

    cur_die = sib_die;
    ret = check_die(cur_die, name, type);
    if (ret)
      return ret;
  }
  return 0;
}

static Dwarf_Die
find_die(char *name, Dwarf_Half type)
{
  Dwarf_Die ret = 0;
  Dwarf_Unsigned cu_header_length = 0;
  Dwarf_Half version_stamp = 0;
  Dwarf_Unsigned abbrev_offset = 0;
  Dwarf_Half address_size = 0;
  Dwarf_Unsigned next_cu_header = 0;
  Dwarf_Error error;
  int cu_number = 0;

  Dwarf_Die no_die = 0;
  Dwarf_Die cu_die = 0;
  int res = DW_DLV_ERROR;

  for (;;++cu_number) {
    no_die = 0;
    cu_die = 0;
    res = DW_DLV_ERROR;

    res = dwarf_next_cu_header(dwrf, &cu_header_length, &version_stamp, &abbrev_offset, &address_size, &next_cu_header, &error);

    if (res == DW_DLV_ERROR) {
      printf("Error in dwarf_next_cu_header\n");
      exit(1);
    }
    if (res == DW_DLV_NO_ENTRY) {
      /* Done. */
      return 0;
    }

    /* The CU will have a single sibling, a cu_die. */
    res = dwarf_siblingof(dwrf,no_die,&cu_die,&error);

    if (res == DW_DLV_ERROR) {
      printf("Error in dwarf_siblingof on CU die \n");
      exit(1);
    }
    if (res == DW_DLV_NO_ENTRY) {
      /* Impossible case. */
      printf("no entry! in dwarf_siblingof on CU die \n");
      exit(1);
    }

    ret = search_dies(cu_die,name,type);

    if (cu_die != ret)
      dwarf_dealloc(dwrf,cu_die,DW_DLA_DIE);

    if (ret)
      break;
  }

  /* traverse to the end to reset */
  while ((dwarf_next_cu_header(dwrf, &cu_header_length, &version_stamp, &abbrev_offset, &address_size, &next_cu_header, &error)) != DW_DLV_NO_ENTRY);

  return ret ? ret : 0;
}

int
bin_type_size(char *name)
{
  Dwarf_Unsigned size = 0;
  Dwarf_Error error;
  int res = DW_DLV_ERROR;
  Dwarf_Die die = 0;

  die = find_die(name, DW_TAG_structure_type);

  if (die) {
    res = dwarf_bytesize(die, &size, &error);
    dwarf_dealloc(dwrf,die,DW_DLA_DIE);
    if (res == DW_DLV_OK)
      return (int)size;
  }

  return -1;
}

int
bin_type_member_offset(char *type, char *member)
{
  Dwarf_Error error;
  int res = DW_DLV_ERROR;
  Dwarf_Die die = 0, child = 0;
  Dwarf_Attribute attr = 0;

  die = find_die(type, DW_TAG_structure_type);

  if (die) {
    child = search_dies(die, member, DW_TAG_member);
    dwarf_dealloc(dwrf,die,DW_DLA_DIE);

    if (child) {
      res = dwarf_attr(child, DW_AT_data_member_location, &attr, &error);
      if (res == DW_DLV_OK) {
        Dwarf_Locdesc *locs = 0;
        Dwarf_Signed num = 0;

        res = dwarf_loclist(attr, &locs, &num, &error);
        if (res == DW_DLV_OK && num > 0) {
          return locs[0].ld_s[0].lr_number;
        }
      }
    }
  }

  return -1;
}

void
bin_init()
{
  int fd;
  ElfW(Shdr) shdr;
  size_t shstrndx;
  char *filename = "/proc/self/exe";
  Elf_Scn *scn;
  Dwarf_Error dwrf_err;

  if (elf_version(EV_CURRENT) == EV_NONE)
    errx(EX_SOFTWARE, "ELF library initialization failed: %s",
        elf_errmsg(-1));

  if ((fd = open(filename, O_RDONLY, 0)) < 0)
    err(EX_NOINPUT, "open \%s\" failed", filename);

  if ((elf = elf_begin(fd, ELF_C_READ, NULL)) == NULL)
    errx(EX_SOFTWARE, "elf_begin() failed: %s.",
        elf_errmsg(-1));

  if (elf_kind(elf) != ELF_K_ELF)
    errx(EX_DATAERR, "%s is not an ELF object.", filename);

  if (elf_getshdrstrndx(elf, &shstrndx) == -1)
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
                           "memprof only works on binaries that are not stripped!");
        }
    }
  }

  if (!symtab_data) {
    errx(EX_DATAERR, "binary is stripped. memprof only works on binaries that are not stripped!");
  }

  if (dwarf_elf_init(elf, DW_DLC_READ, NULL, NULL, &dwrf, &dwrf_err) != DW_DLV_OK) {
    errx(EX_DATAERR, "unable to read debugging data from binary. was it compiled with -g? is it unstripped?");
  }
}
#endif
