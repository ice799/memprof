#if defined(HAVE_MACH)

#include "bin_api.h"
#include "arch.h"
#include "util.h"

#include <assert.h>
#include <dlfcn.h>
#include <err.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach-o/ldsyms.h>
#include <mach-o/nlist.h>
#include <mach-o/dyld_images.h>

struct mach_config {
  const struct mach_header *hdr;
  const struct nlist_64 *symbol_table;
  const struct nlist_64 **sorted_symbol_table;
  const struct section_64 *symstub_sect;
  const char *string_table;
  uint32_t symbol_count;
  uint32_t string_table_size;
  intptr_t image_offset;
  uint32_t nindirectsyms;
  uint32_t indirectsymoff;
  void *file;
};

struct symbol_data {
  const char *name;
  void *address;
  uint32_t size;
  uint32_t index;
};

static struct mach_config ruby_img_cfg;
extern struct memprof_config memprof_config;

/*
 * The jmp instructions in the dyld stub table are 6 bytes,
 * 2 bytes for the instruction and 4 bytes for the offset operand
 *
 * This jmp does not jump to the offset operand, but instead
 * looks up an absolute address stored at the offset and jumps to that.
 * Offset is the offset from the address of the _next_ instruction sequence.
 *
 * We need to deference the address at this offset to find the real
 * target of the dyld stub entry.
 */

struct dyld_stub_entry {
  unsigned char jmp[2];
  uint32_t offset;
} __attribute((__packed__));

/* Return the jmp target of a stub entry */

static inline void*
get_dyld_stub_target(struct dyld_stub_entry *entry) {
  // If the instructions match up, then dereference the address at the offset
  if (entry->jmp[0] == 0xff && entry->jmp[1] == 0x25)
    return *((void**)((void*)(entry + 1) + entry->offset));

  return NULL;
}

/* Set the jmp target of a stub entry */

static inline void
set_dyld_stub_target(struct dyld_stub_entry *entry, void *addr) {
  *((void**)((void*)(entry + 1) + entry->offset)) = addr;
}

static inline const char*
get_symtab_string(struct mach_config *img_cfg, uint32_t stroff);

static void
extract_symbol_data(struct mach_config *img_cfg, struct symbol_data *sym_data);

static void *
find_stub_addr(const char *symname, struct mach_config *img_cfg)
{
  uint64_t i = 0, nsyms = 0;
  uint32_t symindex = 0;
  assert(img_cfg && symname);
  const struct section_64 *sect = img_cfg->symstub_sect;

  nsyms = sect->size / sect->reserved2;

  for (; i < nsyms; i ++) {
    uint32_t currsym = sect->reserved1 + i;
    uint64_t stubaddr = sect->addr + (i * sect->reserved2);
    uint32_t symoff = 0;

    assert(currsym <= img_cfg->nindirectsyms);

    /* indirect sym entries are just 32bit indexes into the symbol table to the
     * symbol the stub is referring to.
     */
    symoff = img_cfg->indirectsymoff + (i * 4);
    memcpy(&symindex, img_cfg->file + symoff, 4);
    symindex = symindex & ((uint32_t) ~(INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS));

    const struct nlist_64 *ent = img_cfg->symbol_table + symindex;
    const char *string = get_symtab_string(img_cfg, ent->n_un.n_strx);

    if (strcmp(symname, string+1) == 0) {
      if (stubaddr) {
        dbg_printf("address of stub for %s is %" PRId64 "\n", string, stubaddr);
        return (void *)stubaddr;
      }
    }
  }
  dbg_printf("couldn't find address of stub: %s\n", symname);
  return NULL;
}

/*
 * Search all entries in a stub table for the stub that corresponds to trampee_addr,
 * and overwrite to point at our trampoline code.
 * Returns 0 if any tramps were successfully inserted.
 */

static int
update_dyld_stub_table(void *table, uint64_t len, void *trampee_addr, struct tramp_st2_entry *tramp)
{
  int ret = -1;
  struct dyld_stub_entry *entry = (struct dyld_stub_entry*) table;
  void *max_addr = table + len;

  for(; (void*)entry < max_addr; entry++) {
    void *target = get_dyld_stub_target(entry);
    if (trampee_addr == target) {
      set_dyld_stub_target(entry, tramp->addr);
      ret = 0;
    }
  }
  return ret;
}

/*
 * This function tells us if the passed header index is something
 * that we should try to update (by looking at it's filename)
 * Only try to update the running executable, or files that match
 * "libruby.dylib" or "*.bundle" (other C gems)
 */

static const struct mach_header
*should_update_image(int index) {
  const struct mach_header *hdr = _dyld_get_image_header(index);

  /* Don't update if it's the memprof bundle */
  if ((void*)hdr == &_mh_bundle_header)
    return NULL;

  /* If it's the ruby executable, do it! */
  if ((void*)hdr == &_mh_execute_header)
    return hdr;

  /* otherwise, check to see if its a bundle or libruby */
  const struct dyld_all_image_infos* (*_dyld_get_all_image_infos)() = NULL;
  const struct dyld_all_image_infos *images = NULL;

  _dyld_lookup_and_bind("__dyld_get_all_image_infos", (void**)&_dyld_get_all_image_infos,NULL);
  assert(_dyld_get_all_image_infos != NULL);

  images = _dyld_get_all_image_infos();
  assert(images!= NULL);

  const struct dyld_image_info image = images->infoArray[index];

  size_t len = strlen(image.imageFilePath);

  if (len >= 6) {
    const char *possible_bundle = (image.imageFilePath + len - 6);
    if (strcmp(possible_bundle, "bundle") == 0)
      return hdr;
  }

  if (len >= 13) {
    const char *possible_libruby = (image.imageFilePath + len - 13);
    if (strcmp(possible_libruby, "libruby.dylib") == 0)
      return hdr;
  }
  return NULL;
}

/*
 * Attempts to update all necessary code in a given 'section' of a Mach-O image, to redirect
 * the given function to the trampoline. This function takes care of both normal calls as well as
 * shared library cases.
 * Returns 0 if any tramps were successfully inserted.
 */

static int
update_mach_section(const struct mach_header *header, const struct section_64 *sect, intptr_t slide, void *trampee_addr, struct tramp_st2_entry *tramp) {
  int ret = -1;
  uint64_t len = 0;
  /*
   * We should be able to calculate this information from the section_64 struct ourselves,
   * but I encountered problems going that route, so using this helper function is fine.
   *
   * The segment "__TEXT" means "executable code and other read-only data." Segments have "sections", like "__text", "__const", etc.
   * We want "__text" for normal function calls, and "__symbol_stub" (with variations like "__symbol_stub1") for shared lib stubs.
   */
  void *section = getsectdatafromheader_64((const struct mach_header_64*)header, "__TEXT", sect->sectname, &len) + slide;

  if (strncmp(sect->sectname, "__symbol_stub", 13) == 0) {
    if (update_dyld_stub_table(section, sect->size, trampee_addr, tramp) == 0) {
      ret = 0;
    }
    return ret;
  }

  if (strcmp(sect->sectname, "__text") == 0) {
    size_t count = 0;
    for(; count < len; section++, count++) {
      if (arch_insert_st1_tramp(section, trampee_addr, tramp) == 0) {
        ret = 0;
      }
    }
  }
  return ret;
}

/*
 * For a given Mach-O image, iterates over all segments and their sections, passing
 * the sections to update_mach_section for potential tramping.
 * Returns 0 if any tramps were successfully inserted.
 */

static int
update_bin_for_mach_header(const struct mach_header *header, intptr_t slide, void *trampee_addr, struct tramp_st2_entry *tramp) {
  int ret = -1;
  int i, j;
  int lc_count = header->ncmds;

  /* Load commands start immediately after the Mach header.
   * This as a char* because we need to step it forward by an arbitrary number of bytes,
   * specified in the 'cmdsize' field of each load command.
   */
  const char *lc = ((const char*) header) + sizeof(struct mach_header_64);

  /* Check all the load commands in the object to see if they are segment commands */
  for (i = 0; i < lc_count; i++, lc += ((struct load_command*)lc)->cmdsize) {
    if (((struct load_command*)lc)->cmd == LC_SEGMENT_64) {
      /* If it's a segment command, we want to iterate over each of it's sections. */
      const struct segment_command_64 *seg = (const struct segment_command_64 *) lc;
      const struct section_64 * sect = (const struct section_64*)(lc + sizeof(struct segment_command_64));
      /* Sections start immediately after the segment_command, and are included in the segment's "cmdsize" */
      int section_count = (seg->cmdsize - sizeof(struct segment_command_64)) / sizeof(struct section_64);

      /* Attempt to tramp the sections */
      for (j=0; j < section_count; j++, sect++) {
        if (update_mach_section(header, sect, slide, trampee_addr, tramp) == 0) {
          ret = 0;
        }
      }
    }
  }
  return ret;
}

/* This function takes a pointer to an *in process* mach_header_64
 * and returns it's image index, which is required to specify the image
 * in many dyld functions.
 *
 * This will NOT work for mach objects read manually from a file, since
 * that's just arbitrary data and the dyld system knows nothing about it.
 */

static int
find_dyld_image_index(const struct mach_header_64 *hdr) {
  uint32_t i;

  for (i = 0; i < _dyld_image_count(); i++) {
    const struct mach_header_64 *tmphdr = (const struct mach_header_64*) _dyld_get_image_header(i);
    if (hdr == tmphdr)
      return i;
  }

  errx(EX_SOFTWARE, "Could not find image index");

  /* this is to quiet a GCC warning. might be a bug because errx is marked
   * __dead2/noreturn
   */
  return -1;
}

/*
 * Reads a file from the filesystem and returns a pointer to it's buffer
 *
 * !!! The pointer returned by this function must be freed !!!
 */

static void *
read_file(const char *filename) {
  void *buf = NULL;
  FILE *file = NULL;
  struct stat filestat;

  assert(filename);

  file = fopen(filename, "r");
  if (!file)
    errx(EX_OSFILE, "Failed to open file %s", filename);

  stat(filename, &filestat);
  buf = malloc(filestat.st_size);

  if (fread(buf, filestat.st_size, 1, file) != 1)
    errx(EX_OSFILE, "Failed to fread() file %s", filename);

  fclose(file);

  return buf;
}

/*
 * This function compares two nlist_64 structures by their n_value field (address, usually).
 * It is used by qsort in extract_symbol_table.
 */

static int
nlist_cmp(const void *obj1, const void *obj2) {
  const struct nlist_64 *nlist1 = *(const struct nlist_64**) obj1;
  const struct nlist_64 *nlist2 = *(const struct nlist_64**) obj2;

  if (nlist1->n_value == nlist2->n_value)
    return 0;
  else if (nlist1->n_value < nlist2->n_value)
    return -1;
  else
    return 1;
}

/*
 * This function sets the passed pointers to a buffer containing the nlist_64 entries,
 * a buffer containing the string data, the number of entries, and the size of the string buffer.
 *
 * The string names of symbols are stored separately from the symbol table.
 * The symbol table entries contain a string 'index', which is an offset into this region.
 *
 * !!! This function allocates memory. symbol_table and string_table should be freed when no longer used !!!
 */

static void
extract_symbol_table(const struct mach_header_64 *hdr, struct mach_config *img_cfg) {
  const struct nlist_64 **new_symtbl;
  char *new_strtbl;
  uint32_t i, j;

  assert(hdr);
  assert(img_cfg);

  const struct load_command *lc = (const struct load_command *)(hdr + 1);

  for (i = 0; i < hdr->ncmds; i++, (lc = (const struct load_command *)((char *)lc + lc->cmdsize))) {
    if (lc->cmd == LC_SYMTAB) {
      dbg_printf("found an LC_SYMTAB load command.\n");
      const struct symtab_command *sc = (const struct symtab_command*) lc;
      const struct nlist_64 *file_symtbl = (const struct nlist_64*)((const char*)hdr + sc->symoff);

      new_symtbl = malloc(sc->nsyms * sizeof(struct nlist_64*));
      new_strtbl = malloc(sc->strsize);

      memcpy(new_strtbl, (char*)hdr + sc->stroff, sc->strsize);

      for (j = 0; j < sc->nsyms; j++)
        new_symtbl[j] = file_symtbl + j;

      qsort(new_symtbl, sc->nsyms, sizeof(struct nlist_64*), &nlist_cmp);

      img_cfg->symbol_table = file_symtbl;
      img_cfg->sorted_symbol_table = new_symtbl;
      img_cfg->string_table = new_strtbl;
      img_cfg->symbol_count = sc->nsyms;
      img_cfg->string_table_size = sc->strsize;
    } else if (lc->cmd == LC_DYSYMTAB) {
      dbg_printf("found an LC_DYSYMTAB load command.\n");
      const struct dysymtab_command *dynsym = (const struct dysymtab_command *) lc;
      img_cfg->nindirectsyms = dynsym->nindirectsyms;
      img_cfg->indirectsymoff = dynsym->indirectsymoff;
    } else if (lc->cmd == LC_SEGMENT_64) {
      dbg_printf("found an LC_SEGMENT_64 load command.\n");
      const struct segment_command_64 *seg = (const struct segment_command_64 *) lc;
      uint32_t i = 0;
      const struct section_64 *asect = (const struct section_64 *)(seg + 1);
      for(; i < seg->nsects; i++, asect++) {
        /*
         * setting up data to find the indirect symbol tables.
         */

        /* if this section hsa no symbol stubs, then we don't care about it */
        if ((asect->flags & SECTION_TYPE) != S_SYMBOL_STUBS)
          continue;

        if (asect->reserved2 == 0) {
          dbg_printf("!!! Found an LC_SEGMET_64 which was marked as having stubs,"
              " but does not have reserved2 set!! %16s.%16s (skipping)\n", asect->segname, asect->sectname);
          continue;
        }

        dbg_printf("Found a section with symbol stubs: %16s.%16s.\n", asect->segname, asect->sectname);
        img_cfg->symstub_sect = asect;
      }
    } else {
      dbg_printf("found another load command that is not being tracked: %" PRId32 "\n", lc->cmd);
    }
  }

  assert(img_cfg->symbol_table && img_cfg->string_table);
}

/*
 * Return the string at the given offset into the symbol table's string buffer
 */

static inline const char*
get_symtab_string(struct mach_config *img_cfg, uint32_t stroff) {
  assert(img_cfg);
  assert(img_cfg->string_table != NULL);
  assert(stroff < img_cfg->string_table_size);
  return img_cfg->string_table + stroff;
}

/*
 * Lookup the address, size, and symbol table index of a symbol given a symbol_data
 * If sym_data is passed with the name set, this function will attempt to fill
 * in the address, etc. If it is passed with the address set, it will attempt
 * to fill in the name.
 */

static void
extract_symbol_data(struct mach_config *img_cfg, struct symbol_data *sym_data)
{
  uint32_t i, j;

  assert(img_cfg->symbol_table != NULL);
  assert(img_cfg->symbol_count > 0);

  for (i=0; i < img_cfg->symbol_count; i++) {
    const struct nlist_64 *nlist_entry = img_cfg->sorted_symbol_table[i];
    const char *string = NULL;

    string = get_symtab_string(img_cfg, nlist_entry->n_un.n_strx);

    /* Add the slide to get the *real* address in the process. */
    const uint64_t addr = nlist_entry->n_value;
    void *ptr = (void*)(addr + img_cfg->image_offset);

    /*
     * If the user passes a name, match against the name
     * If the user passes an address, match against that.
     */
    if ((sym_data->name && string && strcmp(sym_data->name, string+1) == 0) || (sym_data->address && ptr == sym_data->address)) {
      if (!sym_data->address)
        sym_data->address = ptr;
      if (!sym_data->name)
        sym_data->name = string+1;

      sym_data->index = i;

      const struct nlist_64 *next_entry = NULL;

      /*
       * There can be multiple entries in the symbol table with the same n_value (address).
       * This means that the 'next' one isn't always good enough. We have to make sure it's
       * really a different symbol.
       */
      j = 1;
      while (next_entry == NULL) {
        const struct nlist_64 *tmp_entry = img_cfg->sorted_symbol_table[i + j];
        if (nlist_entry->n_value != tmp_entry->n_value)
          next_entry = tmp_entry;
        j++;
      }

      /*
       * Subtract our address from the address of the next symbol to get it's rough size.
       * My observation is that the start of the next symbol will be padded to 16 byte alignment from the end of this one.
       * This should be fine, since the point of getting the size is just to minimize scan area for tramp insertions.
       */
      sym_data->size = (next_entry->n_value - addr);
      break;
    }
  }
}

void *
bin_find_symbol(const char *symbol, size_t *size, int search_libs) {
  struct symbol_data sym_data;

  memset(&sym_data, 0, sizeof(struct symbol_data));
  sym_data.name = symbol;

  extract_symbol_data(&ruby_img_cfg, &sym_data);

  if (size)
    *size = sym_data.size;

  return sym_data.address;
}

/*
 * Do the same thing as in bin_find_symbol above, but compare addresses and return the string name.
 */
const char *
bin_find_symbol_name(void *symbol) {
  struct symbol_data sym_data;

  memset(&sym_data, 0, sizeof(struct symbol_data));
  sym_data.address = symbol;

  extract_symbol_data(&ruby_img_cfg, &sym_data);

  return sym_data.name;
}

/*
 * I will explain bin_update_image with imaginary Ruby code:
 *
 * Process.mach_images.each do |image|
 *   image.segments.each do |segment|
 *     segment.sections.each do |section|
 *       if section.name == "__text"
 *         tramp_normal_callsites(section)
 *       elsif section.name =~ /__symbol_stub/ && image.filename =~ /libruby\.dylib|bundle/
 *         tramp_dyld_stubs(section)
 *       end
 *     end
 *   end
 * end
 */

int
bin_update_image(const char *trampee, struct tramp_st2_entry *tramp, void **orig_function)
{
  int ret = -1;
  int i;
  int header_count = _dyld_image_count();
  void *trampee_addr = bin_find_symbol(trampee, NULL, 0);

  // Go through all the mach objects that are loaded into this process
  for (i=0; i < header_count; i++) {
    const struct mach_header *current_hdr = NULL;

    if ((current_hdr = should_update_image(i)) == NULL)
      continue;

    if (update_bin_for_mach_header(current_hdr, _dyld_get_image_vmaddr_slide(i), trampee_addr, tramp) == 0)
      ret = 0;
  }
  return ret;
}

void *
bin_allocate_page()
{
  void *ret = NULL;
  size_t i = 0;

  for (i = memprof_config.pagesize; i < INT_MAX - memprof_config.pagesize; i += memprof_config.pagesize) {
    ret = mmap((void*)(NULL + i), memprof_config.pagesize, PROT_WRITE|PROT_READ|PROT_EXEC,
               MAP_ANON|MAP_PRIVATE, -1, 0);

    if (ret != MAP_FAILED) {
      memset(ret, 0x90, memprof_config.pagesize);
      return ret;
    }
  }
  return NULL;
}

size_t
bin_type_size(const char *type)
{
  return 0;
}

int
bin_type_member_offset(const char *type, const char *member)
{
  return -1;
}

void
bin_init()
{
  void *ptr = NULL;
  int index = 0;
  Dl_info info;

  memset(&ruby_img_cfg, 0, sizeof(struct mach_config));

  // We can use this is a reasonably sure method of finding the file
  // that the Ruby junk resides in.
  ptr = dlsym(RTLD_DEFAULT, "rb_newobj");

  if (!ptr)
    errx(EX_SOFTWARE, "Could not find rb_newobj in this process. WTF???");

  if (!dladdr(ptr, &info) || !info.dli_fname)
    errx(EX_SOFTWARE, "Could not find the Mach object associated with rb_newobj.");

  ruby_img_cfg.file =  read_file(info.dli_fname);
  struct mach_header_64 *hdr = (struct mach_header_64*) ruby_img_cfg.file;
  assert(hdr);

  if (hdr->magic != MH_MAGIC_64)
    errx(EX_SOFTWARE, "Magic for Ruby Mach-O file doesn't match");

  index = find_dyld_image_index((const struct mach_header_64*) info.dli_fbase);
  ruby_img_cfg.image_offset = _dyld_get_image_vmaddr_slide(index);

  extract_symbol_table(hdr, &ruby_img_cfg);

  assert(ruby_img_cfg.symbol_table != NULL);
  assert(ruby_img_cfg.string_table != NULL);
  assert(ruby_img_cfg.symbol_count > 0);

  free(hdr);
}
#endif
