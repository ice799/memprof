#if defined(HAVE_MACH)

#include "bin_api.h"

#include <limits.h>
#include <string.h>
#include <sysexits.h>
#include <sys/mman.h>

#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach-o/ldsyms.h>

static void
set_text_segment(const struct mach_header *header, const char *sectname)
{
  text_segment = getsectdatafromheader_64((const struct mach_header_64*)header, "__TEXT", sectname, (uint64_t*)&text_segment_len);
  if (!text_segment)
    errx(EX_UNAVAILABLE, "Failed to locate the %s section", sectname);
}

static void
update_dyld_stubs(int entry, void *trampee_addr)
{
  char *byte = text_segment;
  size_t count = 0;

  for(; count < text_segment_len; count++) {
    if (*byte == '\xff') {
      int off = *(int *)(byte+2);
      if (trampee_addr == *((void**)(byte + 6 + off))) {
        *((void**)(byte + 6 + off)) = tramp_table[entry].addr;
      }
    }
    byte++;
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

    if (tramp_table != MAP_FAILED) {
      memset(tramp_table, 0x90, pagesize);
      return ret;
    }
  }
  return NULL;
}

void
bin_update_image(int entry, void *trampee_addr)
{
  int i, j, k;
  int header_count = _dyld_image_count();

  // Go through all the mach objects that are loaded into this process
  for (i=0; i < header_count; i++) {
    const struct mach_header *current_hdr = _dyld_get_image_header(i);

    // Modify any callsites residing inside the text segment
    set_text_segment(current_hdr, "__text");
    text_segment += _dyld_get_image_vmaddr_slide(i);
    update_callqs(entry, trampee_addr);

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
}

void  *
bin_find_symbol(char *sym,  size_t *size) {
  void *ptr = NULL;
  _dyld_lookup_and_bind((const char*)sym, &ptr, NULL);
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
