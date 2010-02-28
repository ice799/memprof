#if defined(HAVE_MACH)

#include "bin_api.h"
#include "arch.h"

#include <limits.h>
#include <string.h>
#include <sysexits.h>
#include <sys/mman.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <stdlib.h>

#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach-o/ldsyms.h>
#include <mach-o/nlist.h>

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

/*
 * Search all entries in a stub table for the stub that corresponds to trampee_addr,
 * and overwrite to point at our trampoline code.
 */

static void
update_dyld_stub_table(void *table, uint64_t len, void *trampee_addr, struct tramp_st2_entry *tramp)
{
  struct dyld_stub_entry *entry = (struct dyld_stub_entry*) table;
  void *max_addr = table + len;

  for(; (void*)entry < max_addr; entry++) {
    void *target = get_dyld_stub_target(entry);
    if (trampee_addr == target) {
      set_dyld_stub_target(entry, tramp->addr);
    }
  }
}

/*
 * This function tells us if the passed stub table address
 * is something that we should try to update (by looking at it's filename)
 * Only try to update dyld stub entries in files that match "libruby.dylib" or "*.bundle" (other C gems)
 */

static inline int
should_update_stub_table(void *addr) {
  Dl_info info;

  if (dladdr(addr, &info)) {
    size_t len = strlen(info.dli_fname);

    if (len >= 6) {
      const char *possible_bundle = (info.dli_fname + len - 6);
      if (strcmp(possible_bundle, "bundle") == 0)
        return 1;
    }

    if (len >= 13) {
      const char *possible_libruby = (info.dli_fname + len - 13);
      if (strcmp(possible_libruby, "libruby.dylib") == 0)
        return 1;
    }
  }
  return 0;
}

/*
 * Attempts to update all necessary code in a given 'section' of a Mach-O image, to redirect
 * the given function to the trampoline. This function takes care of both normal calls as well as
 * shared library cases.
 */

static void
update_mach_section(const struct mach_header *header, const struct section_64 *sect, intptr_t slide, void *trampee_addr, struct tramp_st2_entry *tramp) {
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
    if (should_update_stub_table(section))
      update_dyld_stub_table(section, sect->size, trampee_addr, tramp);
    return;
  }

  /* TODO: check the filename just like we do above for stub sections. No reason to look at unrelated files. */
  if (strcmp(sect->sectname, "__text") == 0) {
    size_t count = 0;
    for(; count < len; section++, count++) {
      arch_insert_st1_tramp(section, trampee_addr, tramp);
    }
  }
}

/*
 * For a given Mach-O image, iterates over all segments and their sections, passing
 * the sections to update_mach_section for potential tramping.
 */

static void
update_bin_for_mach_header(const struct mach_header *header, intptr_t slide, void *trampee_addr, struct tramp_st2_entry *tramp) {
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
        update_mach_section(header, sect, slide, trampee_addr, tramp);
      }
    }
  }
}

/* This function takes a pointer to an *in process* mach_header_64
 * and returns it's image index, which is required to specify the image
 * in many dyld functions.
 *
 * This will NOT work for mach objects read manually from a file, since
 * that's just arbitrary data and the dyld system knows nothing about it.
 */

int
find_dyld_image_index(const struct mach_header_64 *hdr) {
  int i;

  for (i = 0; i < _dyld_image_count(); i++) {
    const struct mach_header_64 *tmphdr = (const struct mach_header_64*) _dyld_get_image_header(i);
    if (hdr == tmphdr)
      return i;
  }
  errx(EX_SOFTWARE, "Could not find image index");
}

/*
 * This function returns a buffer containing the file that is presumed
 * to be either the Ruby executable or libruby. (Wherever rb_newobj is found.)
 *
 * The passed pointer index is set to the dyld image index for the associated
 * in-process mach image.
 *
 * The reason that we read in the file is because the symbol table is not loaded
 * into memory with everything else at load time (at least not anywhere I can find).
 *
 * !!! The pointer returned by this function must be freed !!!
 */

void *
get_ruby_file_and_header_index(int *index) {
  void *ptr = NULL;
  void *buf = NULL;
  Dl_info info;
  struct stat filestat;

  // We can use this is a reasonably sure method of finding the file
  // that the Ruby junk resides in.
  ptr = dlsym(RTLD_DEFAULT, "rb_newobj");

  if (!ptr)
    errx(EX_SOFTWARE, "Could not find rb_newobj in this process. WTF???");

  if (!dladdr(ptr, &info) || !info.dli_fname)
    errx(EX_SOFTWARE, "Could not find the Mach object associated with rb_newobj.");

  FILE *file = fopen(info.dli_fname, "r");
  if (!file)
    errx(EX_OSFILE, "Failed to open Ruby file %s", info.dli_fname);

  stat(info.dli_fname, &filestat);
  buf = malloc(filestat.st_size);

  if (fread(buf, filestat.st_size, 1, file) != 1)
    errx(EX_OSFILE, "Failed to fread() Ruby file %s", info.dli_fname);

  fclose(file);

  *index = find_dyld_image_index((const struct mach_header_64*) info.dli_fbase);
  return buf;
}

/*
 * This function compares two nlist_64 structures by their n_value field (address, usually).
 * It is used by qsort in build_sorted_nlist_table.
 */

int
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
 * This function returns an array of pointers to nlist_64 entries in the symbol table
 * of the file buffer pointed to by the passed mach header, sorted by address, and sets the
 * passed uint32_t pointers nsyms and stroff to those fields found in the symtab_command structure.
 *
 * !!! The pointer returned by this function must be freed !!!
 */

const struct nlist_64 **
build_sorted_nlist_table(const struct mach_header_64 *hdr, uint32_t *nsyms, uint32_t *stroff) {
  const struct nlist_64 **base;
  uint32_t i, j;

  const char *lc = (const char*) hdr + sizeof(struct mach_header_64);

  for (i = 0; i < hdr->ncmds; i++) {
    if (((const struct load_command*)lc)->cmd == LC_SYMTAB) {
      const struct symtab_command *sc = (const struct symtab_command*) lc;
      const struct nlist_64 *symbol_table = (const struct nlist_64*)((const char*)hdr + sc->symoff);

      /* TODO: qsort this table *IN PLACE* instead of making a copy of it. */
      base = malloc(sc->nsyms * sizeof(struct nlist_64*));

      for (j = 0; j < sc->nsyms; j++)
        base[j] = symbol_table + j;

      qsort(base, sc->nsyms, sizeof(struct nlist_64*), &nlist_cmp);

      *nsyms = sc->nsyms;
      *stroff = sc->stroff;
      return base;
    }

    lc += ((const struct load_command*)lc)->cmdsize;
  }
  errx(EX_SOFTWARE, "Unable to find LC_SYMTAB");
}

/*
 * The workflow for bin_find_symbol is as follows:
 *
 * 1. Prefix the symbol with a "_", since Apple is weird
 * 2. Figure out what file we think contains the symbol table (either the executable, or libruby),
 *    and read it into memory. We have to do this because the symbol table is not loaded
 *    into the process as part of the normal image loading. While we're doing this,
 *    figure out the dyld image index of the corresponding *in-process* image.
 * 3. Work up a copy of the symbol table, sorted by address.
 * 4. Iterate over the sorted table and find the symbol that matches our search name
 * 5. Obtain the rough (padded to 16 byte alignment) size of the symbol by subtracting it's
 *    address from the address of the symbol *after* it.
 * 6. Calculate the *real* address of the symbol by adding the image's "slide"
 *    (offset at which the image was loaded into the process) to the table entry's address.
 */

void *
bin_find_symbol(char *symbol, size_t *size) {
  /* Correctly prefix the symbol with a '_' (whats a prettier way to do this?) */
  size_t len = strlen(symbol);
  char real_symbol[len + 2];
  memcpy(real_symbol, "_", 1);
  memcpy((real_symbol + 1), symbol, len);
  memcpy((real_symbol + len + 1), "\0", 1);

  void *ptr = NULL;
  void *file = NULL;

  uint32_t i, j, k;
  uint32_t stroff, nsyms = 0;
  int index = 0;

  file = get_ruby_file_and_header_index(&index);

  const struct mach_header_64 *hdr = (const struct mach_header_64*) file;
  if (hdr->magic != MH_MAGIC_64)
    errx(EX_SOFTWARE, "Magic for Ruby Mach-O file doesn't match");

  const struct nlist_64 **nlist_table = build_sorted_nlist_table(hdr, &nsyms, &stroff);
  /*
   * The string names of symbols are stored separately from the symbol table.
   * The symbol table entries contain a string 'index', which is an offset into this region.
   */
  const char *string_table = (const char*)hdr + stroff;

  for (i=0; i < nsyms; i++) {
    const struct nlist_64 *nlist_entry = nlist_table[i];
    const char *string = string_table + nlist_entry->n_un.n_strx;

    if (strcmp(real_symbol, string) == 0) {
      const uint64_t addr = nlist_entry->n_value;
      /* Add the slide to get the *real* address in the process. */
      ptr = (void*)(addr + _dyld_get_image_vmaddr_slide(index));

      if (size) {
        const struct nlist_64 *next_entry = NULL;

        /*
         * There can be multiple entries in the symbol table with the same n_value (address).
         * This means that the 'next' one isn't always good enough. We have to make sure it's
         * really a different symbol.
         */
        j = 1;
        while (next_entry == NULL) {
          const struct nlist_64 *tmp_entry = nlist_table[i + j];
          if (nlist_entry->n_value != tmp_entry->n_value)
            next_entry = tmp_entry;
          j++;
        }

        /*
         * Subtract our address from the address of the next symbol to get it's rough size.
         * My observation is that the start of the next symbol will be padded to 16 byte alignment from the end of this one.
         * This should be fine, since the point of getting the size is just to minimize scan area for tramp insertions.
         */
        *size = (next_entry->n_value - addr);
      }
      break;
    }
  }

  free(nlist_table);
  free(file);
  return ptr;
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

void
bin_update_image(int entry, char *trampee, struct tramp_st2_entry *tramp)
{
  int i;
  int header_count = _dyld_image_count();
  void *trampee_addr = bin_find_symbol(trampee, NULL);

  // Go through all the mach objects that are loaded into this process
  for (i=0; i < header_count; i++) {
    const struct mach_header *current_hdr = _dyld_get_image_header(i);
    if ((void*)current_hdr == &_mh_bundle_header)
      continue;

    update_bin_for_mach_header(current_hdr, _dyld_get_image_vmaddr_slide(i), trampee_addr, tramp);
  }
}

void *
bin_allocate_page()
{
  void *ret = NULL;
  size_t i = 0;

  for (i = pagesize; i < INT_MAX - pagesize; i += pagesize) {
    ret = mmap((void*)(NULL + i), pagesize, PROT_WRITE|PROT_READ|PROT_EXEC,
               MAP_ANON|MAP_PRIVATE, -1, 0);

    if (ret != MAP_FAILED) {
      memset(ret, 0x90, pagesize);
      return ret;
    }
  }
  return NULL;
}

int
bin_type_size(char *type)
{
  return -1;
}

int
bin_type_member_offset(char *type, char *member)
{
  return -1;
}

void
bin_init()
{
  /* mach-o is so cool it needs no initialization */
}
#endif
