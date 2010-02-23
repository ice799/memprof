#if defined(HAVE_MACH)

#include "bin_api.h"

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

void *text_segment;
size_t text_segment_len;

static void
set_text_segment(const struct mach_header *header, const char *sectname)
{
  text_segment = getsectdatafromheader_64((const struct mach_header_64*)header, "__TEXT", sectname, (uint64_t*)&text_segment_len);
  if (!text_segment)
    errx(EX_SOFTWARE, "Failed to locate the %s section", sectname);
}

//static void
//update_dyld_stubs(int entry, void *trampee_addr)
//{
//  char *byte = text_segment;
//  size_t count = 0;
//
//  for(; count < text_segment_len; count++) {
//    if (*byte == '\xff') {
//      int off = *(int *)(byte+2);
//      if (trampee_addr == *((void**)(byte + 6 + off))) {
//        *((void**)(byte + 6 + off)) = tramp_table[entry].addr;
//      }
//    }
//    byte++;
//  }
//}

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

void
bin_update_image(int entry, char *trampee, struct tramp_st2_entry *tramp)
{
  int i, j, k;
  int header_count = _dyld_image_count();
  void *trampee_addr = bin_find_symbol(trampee, NULL);

  // Go through all the mach objects that are loaded into this process
  for (i=0; i < header_count; i++) {
    const struct mach_header *current_hdr = _dyld_get_image_header(i);

    // Modify any callsites residing inside the text segment
    set_text_segment(current_hdr, "__text");
    text_segment += _dyld_get_image_vmaddr_slide(i);

    unsigned char *byte = text_segment;
    size_t count = 0;

    for(; count < text_segment_len; byte++, count++) {
      if (arch_insert_st1_tramp(byte, trampee_addr, tramp)) {
        // printf("tramped %p for %s\n", byte, trampee);
      }
    }

  //  int lc_count = current_hdr->ncmds;
  //
  //  // this as a char* because we need to step it forward by an arbitrary number of bytes
  //  const char *lc = ((const char*) current_hdr) + sizeof(struct mach_header_64);
  //
  //  // Check all the load commands in the object to see if they are segment commands
  //  for (j = 0; j < lc_count; j++) {
  //    if (((struct load_command*)lc)->cmd == LC_SEGMENT_64) {
  //      const struct segment_command_64 *seg = (const struct segment_command_64 *) lc;
  //      const struct section_64 * sect = (const struct section_64*)(lc + sizeof(struct segment_command_64));
  //      int section_count = (seg->cmdsize - sizeof(struct segment_command_64)) / sizeof(struct section_64);
  //
  //      // Search the segment for a section containing dyld stub functions
  //      for (k=0; k < section_count; k++) {
  //        if (strncmp(sect->sectname, "__symbol_stub", 13) == 0) {
  //          set_text_segment((struct mach_header*)current_hdr, sect->sectname);
  //          text_segment += _dyld_get_image_vmaddr_slide(i);
  //          update_dyld_stubs(entry, trampee_addr);
  //        }
  //        sect++;
  //      }
  //    }
  //    lc += ((struct load_command*)lc)->cmdsize;
  //  }
  }
}


// This function takes a pointer to a loaded *in memory* mach_header_64
// and returns it's image index. This will NOT work for mach objects read
// from a file.

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


// This function returns a buffer containing the file that is presumed
// to be either the Ruby executable or libruby. (Wherever rb_newobj is found.)
//
// The passed pointer index is set to the image index for the associated
// in-memory mach image.
//
// !!! The pointer returned by this function must be freed !!!

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


// This function compares two nlist_64 structures by their n_value field (address)
// used by qsort in build_sorted_nlist_table

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


// This function returns an array of pointers to nlist_64 entries in the symbol table
// of the file pointed to by the passed mach header, sorted by address, and sets the
// passed uint32_t pointers nsyms and stroff to those fields found in the symtab_command structure.
//
// !!! The pointer returned by this function must be freed !!!

const struct nlist_64 **
build_sorted_nlist_table(const struct mach_header_64 *hdr, uint32_t *nsyms, uint32_t *stroff) {
  const struct nlist_64 **base;
  uint32_t i, j;

  const char *lc = (const char*) hdr + sizeof(struct mach_header_64);

  for (i = 0; i < hdr->ncmds; i++) {
    if (((const struct load_command*)lc)->cmd == LC_SYMTAB) {
      const struct symtab_command *sc = (const struct symtab_command*) lc;
      const struct nlist_64 *symbol_table = (const struct nlist_64*)((const char*)hdr + sc->symoff);

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

void *
bin_find_symbol(char *symbol, size_t *size) {
  // Correctly prefix the symbol with a '_' (whats a prettier way to do this?)
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
  const char *string_table = (const char*)hdr + stroff;

  for (i=0; i < nsyms; i++) {
    const struct nlist_64 *nlist_entry = nlist_table[i];
    const char *string = string_table + nlist_entry->n_un.n_strx;

    if (strcmp(real_symbol, string) == 0) {
      const uint64_t addr = nlist_entry->n_value;
      ptr = (void*)(addr + _dyld_get_image_vmaddr_slide(index));

      if (size) {
        const struct nlist_64 *next_entry = NULL;

        j = 1;
        while (next_entry == NULL) {
          const struct nlist_64 *tmp_entry = nlist_table[i + j];
          if (nlist_entry->n_value != tmp_entry->n_value)
            next_entry = tmp_entry;
          j++;
        }

        *size = (next_entry->n_value - addr);
      }
      break;
    }
  }

  free(nlist_table);
  free(file);
  return ptr;
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
