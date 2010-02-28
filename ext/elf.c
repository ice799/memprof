#if defined(HAVE_ELF)
#define _GNU_SOURCE
#include "bin_api.h"
#include "arch.h"

#include <assert.h>
#include <dwarf.h>
#include <err.h>
#include <error.h>
#include <fcntl.h>
#include <libdwarf.h>
#include <libelf/gelf.h>
#include <limits.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include <sys/mman.h>

/* The size of a PLT entry */
#define PLT_ENTRY_SZ  (16)

/* Keep track of whether this ruby is built with a shared library or not */
static int libruby = 0;

static Dwarf_Debug dwrf = NULL;

/* Set of ELF specific state about this Ruby binary/shared object */
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

/*
 * do_bin_allocate_page - internal page allocation routine
 *
 * This function allocates a page suitable for stage 2 trampolines. This page
 * is allocated based on the location of the text segment of the Ruby binary
 * or libruby.
 *
 * The page has to be located in a 32bit window from the Ruby code so that
 * jump and call instructions can redirect execution there.
 *
 * This function returns the address of the page found or NULL if no page was
 * found.
 */
static void *
do_bin_allocate_page(struct elf_info *info)
{
  void * ret = NULL, *addr = NULL;
  uint16_t count = 0;

  if (!info)
    return NULL;

  if (libruby) {
    /* There is a libruby. Start at the end of the text segment and search for
     * a page.
     */
    addr = info->text_segment + info->text_segment_len;
    for (; count < UINT_MAX; addr += pagesize, count += pagesize) {
      ret = mmap(addr, pagesize, PROT_WRITE|PROT_READ|PROT_EXEC, MAP_ANON|MAP_PRIVATE, -1, 0);
      if (ret != MAP_FAILED) {
        memset(ret, 0x90, pagesize);
        return ret;
      }
    }
  } else {
    /* if there is no libruby, use the linux specific MAP_32BIT flag which will
     * grab a page in the lower 4gb of the address space.
     */
    assert((size_t)info->text_segment <= UINT_MAX);
    return mmap(NULL, pagesize, PROT_WRITE|PROT_READ|PROT_EXEC, MAP_ANON|MAP_PRIVATE|MAP_32BIT, -1, 0);
  }

  return NULL;
}

/*
 * bin_allocate_page - allocate a page suitable for holding stage 2 trampolines
 *
 * This function is just a wrapper which passes through some internal state.
 */
void *
bin_allocate_page()
{
  return do_bin_allocate_page(ruby_info);
}

/*
 * get_plt_addr - architecture specific PLT entry retrieval
 *
 * Given the internal data and an index, this function returns the address of
 * the PLT entry at that address.
 *
 * A PLT entry takes the form:
 *
 * jmpq *0xfeedface(%rip)
 * pushq $0xaa
 * jmpq 17110
 */
static inline GElf_Addr
get_plt_addr(struct elf_info *info, size_t ndx) {
  assert(info != NULL);
  return info->base_addr + info->plt_addr + (ndx + 1) * 16;
}

/*
 * find_got_addr - find the global offset table entry for specific symbol name.
 *
 * Given:
 *  - syname - the symbol name
 *  - info   - internal information about the ELF object to search
 *
 * This function searches the .rela.plt section of an ELF binary, searcing for
 * entries that match the symbol name passed in. If one is found, the address
 * of corresponding entry in .plt is returned.
 */
static void *
find_plt_addr(const char *symname, struct elf_info *info)
{
  assert(symname != NULL);

  size_t i = 0;

  if (info == NULL) {
    info = ruby_info;
  }

  /* Search through each of the .rela.plt entries */
  for (i = 0; i < info->relplt_count; i++) {
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

      /* The name matches the name of the symbol passed in, so get the PLT entry
       * address and return it.
       */
      if (strcmp(symname, name) == 0) {
        addr = get_plt_addr(info, i);
        return (void *)addr;
      }
    }
  }

  return NULL;
}

/*
 * do_bin_find_symbol - internal symbol lookup function.
 *
 * Given:
 *  - sym - the symbol name to look up
 *  - size - an optional out argument holding the size of the symbol
 *  - elf - an elf information structure
 *
 * This function will return the address of the symbol (setting size if desired)
 * or NULL if nothing can be found.
 */
static void *
do_bin_find_symbol(const char *sym, size_t *size, struct elf_info *elf)
{
  char *name = NULL;

  assert(sym != NULL);
  assert(elf != NULL);

  assert(elf->symtab_data != NULL);
  assert(elf->symtab_data->d_buf != NULL);

  ElfW(Sym) *esym = (ElfW(Sym)*) elf->symtab_data->d_buf;
  ElfW(Sym) *lastsym = (ElfW(Sym)*) ((char*) elf->symtab_data->d_buf + elf->symtab_data->d_size);

  assert(esym <= lastsym);

  for (; esym < lastsym; esym++){
    /* ignore weak/numeric/empty symbols */
    if ((esym->st_value == 0) ||
        (ELF32_ST_BIND(esym->st_info)== STB_WEAK) ||
        (ELF32_ST_BIND(esym->st_info)== STB_NUM))
      continue;

    name = elf_strptr(elf->elf, elf->symtab_shdr.sh_link, (size_t)esym->st_name);

    assert(name != NULL);

    if (strcmp(name, sym) == 0) {
      if (size) {
        *size = esym->st_size;
      }
      return elf->base_addr + (void *)esym->st_value;
    }
  }
  return NULL;
}

/*
 * bin_find_symbol - find the address of a given symbol and set its size if
 * desired.
 *
 * This function is just a wrapper for the internal symbol lookup function.
 */
void *
bin_find_symbol(const char *sym, size_t *size)
{
  return do_bin_find_symbol(sym, size, ruby_info);
}

/*
 * bin_update_image - update the ruby binary image in memory.
 *
 * Given -
 *  trampee - the name of the symbol to hook
 *  tramp - the stage 2 trampoline entry
 *
 * This function will update the ruby binary image so that all calls to trampee
 * will be routed to tramp.
 *
 * Returns 0 on success
 */
int
bin_update_image(const char *trampee, struct tramp_st2_entry *tramp)
{
  void *trampee_addr = NULL;

  assert(trampee != NULL);
  assert(tramp != NULL);
  assert(tramp->addr != NULL);

  if (!libruby) {
    unsigned char *byte = ruby_info->text_segment;
    trampee_addr = bin_find_symbol(trampee, NULL);
    size_t count = 0;
    int num = 0;

    assert(byte != NULL);
    assert(trampee_addr != NULL);

    for(; count < ruby_info->text_segment_len; byte++, count++) {
      if (arch_insert_st1_tramp(byte, trampee_addr, tramp) == 0) {
        num++;
      }
    }
  } else {
    trampee_addr = find_plt_addr(trampee, NULL);
    assert(trampee_addr != NULL);
    arch_overwrite_got(trampee_addr, tramp->addr);
  }
  return 0;
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

/*
 * has_libruby - check if this ruby binary is linked against libruby.so
 *
 * This function checks if the curreny binary is linked against libruby. If
 * so, it sets libruby = 1, and fill internal state in the elf_info structure.
 *
 * Returns 1 if this binary is linked to libruby.so, 0 if not.
 */
static int
has_libruby(struct elf_info *lib)
{
  struct link_map *map = _r_debug.r_map;

  libruby = 0;
  while (map) {
    if (strstr(map->l_name, "libruby.so")) {
      if (lib) {
        lib->base_addr = (GElf_Addr)map->l_addr;
        lib->filename = strdup(map->l_name);
      }
      libruby = 1;
      break;
    }
    map = map->l_next;
  }

  return libruby;
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

/*
 * open_elf - Opens a file from disk and gets the elf reader started.
 *
 * Given a filename, this function attempts to open the file and start the
 * elf reader.
 *
 * Returns an Elf object.
 */
static Elf *
open_elf(const char *filename)
{
  Elf *ret = NULL;
  int fd = 0;

  assert(filename != NULL);
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

  assert(ret != NULL);
  return ret;
}

/*
 * dissect_elf - Parses and stores internal data about an ELF object.
 *
 * Given an elf_info structure, this function will attempt to parse the object
 * and store important state needed to rewrite the object later.
 */
static void
dissect_elf(struct elf_info *info)
{
  assert(info != NULL);

  size_t shstrndx = 0;
  Elf *elf = info->elf;
  Elf_Scn *scn = NULL;
  GElf_Shdr shdr;
  size_t j = 0;

  if (elf_getshdrstrndx(elf, &shstrndx) == -1) {
    errx(EX_SOFTWARE, "getshstrndx() failed: %s.", elf_errmsg(-1));
  }

  if (gelf_getehdr(elf, &(info->ehdr)) == NULL) {
    errx(EX_SOFTWARE, "Couldn't get elf header.");
  }

  /* search each ELF header and store important data for each header... */
  while ((scn = elf_nextscn(elf, scn)) != NULL) {
    if (gelf_getshdr(scn, &shdr) != &shdr)
      errx(EX_SOFTWARE, "getshdr() failed: %s.",
          elf_errmsg(-1));


    /*
     * The .dynamic section contains entries that are important to memprof.
     * Specifically, the .rela.plt section information. The .rela.plt section
     * indexes the .plt, which will be important for hooking functions in
     * shared objects.
     */
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
    }
    /*
     * The .dynsym section has useful pieces, too, like the dynamic symbol
     * table. This table is used when walking the .rela.plt section.
     */
    else if (shdr.sh_type == SHT_DYNSYM) {
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
    }
    /*
     * Pull out information (start address and length) of the .text section.
     */
    else if (shdr.sh_type == SHT_PROGBITS &&
        (shdr.sh_flags == (SHF_ALLOC | SHF_EXECINSTR)) &&
        strcmp(elf_strptr(elf, shstrndx, shdr.sh_name), ".text") == 0) {

      info->text_segment = (void *)shdr.sh_addr + info->base_addr;
      info->text_segment_len = shdr.sh_size;
    }
    /*
     * Pull out information (start address) of the .plt section.
     */
    else if (shdr.sh_type == SHT_PROGBITS) {
	    if (strcmp(elf_strptr(elf, shstrndx, shdr.sh_name), ".plt") == 0) {
		    info->plt_addr = shdr.sh_addr;
	    }
    }
    /*
     * The symbol table is also needed for bin_find_symbol
     */
    else if (shdr.sh_type == SHT_SYMTAB) {
      info->symtab_shdr = shdr;
      if ((info->symtab_data = elf_getdata(scn, info->symtab_data)) == NULL ||
          info->symtab_data->d_size == 0) {
        errx(EX_DATAERR, "shared lib has a broken symbol table. Is it stripped? "
            "memprof only works on shared libs that are not stripped!");
      }
    }
  }

  /* If this object has no symbol table there's nothing else to do but fail */
  if (!info->symtab_data) {
    errx(EX_DATAERR, "binary is stripped. memprof only works on binaries that are not stripped!");
  }


  /*
   * Walk the sections, pull out, and store the .plt section
   */
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

/*
 * bin_init - initialize the binary parsing/modification layer.
 *
 * This function starts the elf parser and sets up internal state.
 */
void
bin_init()
{
  Dwarf_Error dwrf_err;

  ruby_info = calloc(1, sizeof(*ruby_info));

  if (!ruby_info) {
    errx(EX_UNAVAILABLE, "Unable to allocate memory to start binary parsing layer");
  }

  if (!has_libruby(ruby_info)) {
    ruby_info->filename = "/proc/self/exe";
    ruby_info->base_addr = 0;
  }

  ruby_info->elf = open_elf(ruby_info->filename);
  dissect_elf(ruby_info);

  if (dwarf_elf_init(ruby_info->elf, DW_DLC_READ, NULL, NULL, &dwrf, &dwrf_err) != DW_DLV_OK) {
    errx(EX_DATAERR, "unable to read debugging data from binary. was it compiled with -g? is it unstripped?");
  }
}
#endif
