#if defined(HAVE_ELF)
#define _GNU_SOURCE
#include "bin_api.h"
#include "arch.h"

#include <dwarf.h>
#include <err.h>
#include <error.h>
#include <fcntl.h>
#include <libdwarf.h>
#include <libelf/gelf.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include <sys/mman.h>

/* ruby binary info */
static int has_libruby = 0;

static Dwarf_Debug dwrf = NULL;

static struct elf_info *ruby_info = NULL;

struct elf_info {
  Elf *elf;

  GElf_Addr base_addr;

  void *text_segment;
  size_t text_segment_len;

  GElf_Addr relplt_addr;
  Elf_Data *relplt;
  size_t relplt_count;

  GElf_Addr plt_addr;
  Elf_Data *plt;
  size_t plt_size;
  size_t plt_count;

  GElf_Ehdr ehdr;

  Elf_Data *dynsym;
  size_t dynsym_count;
  const char *dynstr;

  GElf_Shdr symtab_shdr;
  Elf_Data *symtab_data;

  const char *filename;
};


static void *
do_bin_allocate_page(void *cookie)
{
  void * ret = NULL, *addr = NULL;
  struct elf_info *info = cookie;
  uint16_t max = ~0, count = 0;

  if (!info)
    return NULL;

  if (has_libruby) {
    addr = info->text_segment + info->text_segment_len;
    for (; count < max;addr += pagesize, count += pagesize) {
      ret = mmap(addr, pagesize, PROT_WRITE|PROT_READ|PROT_EXEC, MAP_ANON|MAP_PRIVATE, -1, 0);
      if (ret != MAP_FAILED) {
        memset(ret, 0x90, pagesize);
        return ret;
      }
    }
  } else {
    return mmap(NULL, pagesize, PROT_WRITE|PROT_READ|PROT_EXEC, MAP_ANON|MAP_PRIVATE|MAP_32BIT, -1, 0);
  }

  return NULL;
}

void *
bin_allocate_page()
{
  return do_bin_allocate_page(ruby_info);
}

static inline GElf_Addr
arch_plt_sym_val(struct elf_info *info, size_t ndx) {
  return info->base_addr + info->plt_addr + (ndx + 1) * 16;
}

static void *
find_got_addr(char *symname, void *cookie)
{
  size_t i = 0;
  struct elf_info *info = cookie;

  if (cookie == NULL) {
    info = ruby_info;
  }

  for (i = 0; i < info->relplt_count; ++i) {
    GElf_Rela rela;
    GElf_Sym sym;
    GElf_Addr addr;
    void *ret;
    const char *name;

    if (info->relplt->d_type == ELF_T_RELA) {
      ret = gelf_getrela(info->relplt, i, &rela);

      if (ret == NULL
          || ELF64_R_SYM(rela.r_info) >= info->dynsym_count
          || gelf_getsym(info->dynsym, ELF64_R_SYM(rela.r_info), &sym) == NULL)
        return NULL;

      name = info->dynstr + sym.st_name;
      if (strcmp(symname, name) == 0) {
        addr = arch_plt_sym_val(info, i);
        return (void *)addr;
      }
    }
  }

  return NULL;
}

static void *
do_bin_find_symbol(char *sym, size_t *size, struct elf_info *elf)
{
  char *name = NULL;

  ElfW(Sym) *esym = (ElfW(Sym)*) elf->symtab_data->d_buf;
  ElfW(Sym) *lastsym = (ElfW(Sym)*) ((char*) elf->symtab_data->d_buf + elf->symtab_data->d_size);

  for (; esym < lastsym; esym++){
    /* ignore weak/numeric/empty symbols */
    if ((esym->st_value == 0) ||
        (ELF32_ST_BIND(esym->st_info)== STB_WEAK) ||
        (ELF32_ST_BIND(esym->st_info)== STB_NUM))
      continue;

    name = elf_strptr(elf->elf, elf->symtab_shdr.sh_link, (size_t)esym->st_name);
    if (strcmp(name, sym) == 0) {
      if (size) {
        *size = esym->st_size;
      }
      return elf->base_addr + (void *)esym->st_value;
    }
  }
  return NULL;
}

void *
bin_find_symbol(char *sym, size_t *size)
{
  return do_bin_find_symbol(sym, size, ruby_info);
}

void
bin_update_image(int entry, char *trampee, struct tramp_st2_entry *tramp)
{
  void *trampee_addr = NULL;

  if (!has_libruby) {
    unsigned char *byte = ruby_info->text_segment;
    trampee_addr = bin_find_symbol(trampee, NULL);
    size_t count = 0;
    int num = 0;

    for(; count < ruby_info->text_segment_len; byte++, count++) {
      if (arch_insert_st1_tramp(byte, trampee_addr, tramp)) {
        // printf("tramped %x\n", byte);
        num++;
      }
    }
  } else {
    trampee_addr = find_got_addr(trampee, NULL);
    arch_overwrite_got(trampee_addr, tramp->addr);
  }
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

static int
bin_has_libruby(struct elf_info *cookie)
{
  struct link_map *map = _r_debug.r_map;
  struct elf_info *lib = cookie;

  if (has_libruby != -1) {
    has_libruby = 0;
    while (map) {
      if (strstr(map->l_name, "libruby.so")) {
        if (lib) {
          lib->base_addr = (GElf_Addr)map->l_addr;
          lib->filename = strdup(map->l_name);
        }
        has_libruby = 1;
        break;
      }
      map = map->l_next;
    }
  }

  return has_libruby;
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

static Elf *
open_elf(const char *filename)
{
  Elf *ret = NULL;
  int fd = 0;

  if (elf_version(EV_CURRENT) == EV_NONE)
    errx(EX_SOFTWARE, "ELF library initialization failed: %s",
        elf_errmsg(-1));

  if ((fd = open(filename, O_RDONLY, 0)) < 0)
    err(EX_NOINPUT, "open \%s\" failed", filename);

  if ((ret = elf_begin(fd, ELF_C_READ, NULL)) == NULL)
    errx(EX_SOFTWARE, "elf_begin() failed: %s.",
        elf_errmsg(-1));

  if (elf_kind(ret) != ELF_K_ELF)
    errx(EX_DATAERR, "%s is not an ELF object.", filename);

  return ret;
}

static void
dissect_elf(struct elf_info *info)
{
  size_t shstrndx = 0;
  Elf *elf = info->elf;
  Elf_Scn *scn = NULL;
  GElf_Shdr shdr;
  size_t j = 0;

  if (elf_getshdrstrndx(elf, &shstrndx) == -1)
    errx(EX_SOFTWARE, "getshstrndx() failed: %s.", elf_errmsg(-1));

  if (gelf_getehdr(elf, &(info->ehdr)) == NULL)
    errx(EX_SOFTWARE, "Couldn't get elf header.");

  while ((scn = elf_nextscn(elf, scn)) != NULL) {
    if (gelf_getshdr(scn, &shdr) != &shdr)
      errx(EX_SOFTWARE, "getshdr() failed: %s.",
          elf_errmsg(-1));

    /* if there is a dynamic section ... */
    if (shdr.sh_type == SHT_DYNAMIC) {
      Elf_Data *data;
      data = elf_getdata(scn, NULL);
      /* for each entry in the dyn section...  */
      for (j = 0; j < shdr.sh_size / shdr.sh_entsize; ++j) {
        GElf_Dyn dyn;
        if (gelf_getdyn(data, j, &dyn) == NULL) {
          error(EXIT_FAILURE, 0,
              "Couldn't get .dynamic data from loaded library.");
        }

        if (dyn.d_tag == DT_JMPREL) {
          info->relplt_addr = dyn.d_un.d_ptr;
        }
        else if (dyn.d_tag == DT_PLTRELSZ) {
          info->plt_size = dyn.d_un.d_val;
        }
      }
    } else if (shdr.sh_type == SHT_DYNSYM) {
      Elf_Data *data;

      info->dynsym = elf_getdata(scn, NULL);
      info->dynsym_count = shdr.sh_size / shdr.sh_entsize;
      if (info->dynsym == NULL
          || elf_getdata(scn, info->dynsym) != NULL)
        error(EXIT_FAILURE, 0,
              "Couldn't get .dynsym data ");

      scn = elf_getscn(elf, shdr.sh_link);
      if (scn == NULL || gelf_getshdr(scn, &shdr) == NULL)
        error(EXIT_FAILURE, 0,
              "Couldn't get section header from");

      data = elf_getdata(scn, NULL);
      if (data == NULL || elf_getdata(scn, data) != NULL
          || shdr.sh_size != data->d_size || data->d_off)
        error(EXIT_FAILURE, 0,
              "Couldn't get .dynstr data");

      info->dynstr = data->d_buf;
    } else if (shdr.sh_type == SHT_PROGBITS &&
        (shdr.sh_flags == (SHF_ALLOC | SHF_EXECINSTR)) &&
        strcmp(elf_strptr(elf, shstrndx, shdr.sh_name), ".text") == 0) {

      info->text_segment = (void *)shdr.sh_addr + info->base_addr;
      info->text_segment_len = shdr.sh_size;
    } else if (shdr.sh_type == SHT_PROGBITS) {
	    if (strcmp(elf_strptr(elf, shstrndx, shdr.sh_name), ".plt") == 0) {
		    info->plt_addr = shdr.sh_addr;
	    }
    } else if (shdr.sh_type == SHT_SYMTAB) {
      info->symtab_shdr = shdr;
      if ((info->symtab_data = elf_getdata(scn, info->symtab_data)) == NULL ||
          info->symtab_data->d_size == 0) {
        errx(EX_DATAERR, "shared lib has a broken symbol table. Is it stripped? "
            "memprof only works on shared libs that are not stripped!");
      }
    }
  }

  if (!info->symtab_data) {
    errx(EX_DATAERR, "binary is stripped. memprof only works on binaries that are not stripped!");
  }


  for (j = 1; j < info->ehdr.e_shnum; j++) {
    scn =  elf_getscn(elf, j);
    if (scn == NULL || gelf_getshdr(scn, &shdr) == NULL)
        error(EXIT_FAILURE, 0,
              "Couldn't get section header from library.");

    if (shdr.sh_addr == info->relplt_addr
        && shdr.sh_size == info->plt_size) {
      info->relplt = elf_getdata(scn, NULL);
      info->relplt_count = shdr.sh_size / shdr.sh_entsize;
      if (info->relplt == NULL
          || elf_getdata(scn, info->relplt) != NULL)
        error(EXIT_FAILURE, 0,
            "Couldn't get .rel*.plt data from");
      break;
    }
  }

  return;
}

void
bin_init()
{
  Dwarf_Error dwrf_err;

  ruby_info = calloc(1, sizeof(*ruby_info));

  if (!ruby_info)
    return;

  if (!bin_has_libruby(ruby_info)) {
    ruby_info->filename = strdup("/proc/self/exe");
    ruby_info->base_addr = 0;
  }

  ruby_info->elf = open_elf(ruby_info->filename);
  dissect_elf(ruby_info);

  if (dwarf_elf_init(ruby_info->elf, DW_DLC_READ, NULL, NULL, &dwrf, &dwrf_err) != DW_DLV_OK) {
    errx(EX_DATAERR, "unable to read debugging data from binary. was it compiled with -g? is it unstripped?");
  }
}
#endif
