#if !defined(_x86_gen_)
#define _x86_gen_

#include <assert.h>
#include <sys/mman.h>
#include <stdint.h>
#include "arch.h"

/*
 * st1_base - stage 1 base instruction sequence
 *
 * This struct is intended to be "laid onto" a piece of memory to ease the
 * parsing, use, and length calculation of call instructions that use a 32bit
 * displacement.
 *
 * For example:   callq <0xdeadbeef> #rb_newobj
 */
static struct st1_base {
  unsigned char call;
  int32_t displacement;
} __attribute__((__packed__)) st1_mov = {
  .call         =  0xe8,
  .displacement =  0,
};

/*
 * page_align - given an address, return a page aligned form
 *
 * TODO Don't assume page size, get it from sysconf and cache the result
 */
static inline void *
page_align(void *addr)
{
  assert(addr != NULL);
  return (void *)((size_t)addr & ~(0xFFFF));
}

/*
 * copy_instructions - copy count bytes from src to dest, taking care to use
 * mprotect to mark the section read/write.
 */
static void
copy_instructions(void *dest, void *src, size_t count)
{
  assert(dest != NULL);
  assert(src != NULL);

  void *aligned_addr = page_align(dest);

  /* I add "+ count" here to guard against the possibility of the instructions
   * laying across a page boundary
   */

  mprotect(aligned_addr, (dest - aligned_addr) + count, PROT_READ|PROT_WRITE|PROT_EXEC);
  memcpy(dest, src, count);
 // mprotect(aligned_addr, (dest - aligned_addr) + count, PROT_READ|PROT_EXEC);

  return;
}

/*
 * WRITE_INSTRUCTIONS - page align start, recalculate len to take into account
 * alignment, set the read/write permissions and execute the code stmt.
 */
#define WRITE_INSTRUCTIONS(start, len, stmt) do { \
  void *aligned_addr = page_align((void *)start); \
  int count = ((void *)start) - aligned_addr + len; \
  mprotect(aligned_addr, count, PROT_READ | PROT_WRITE | PROT_EXEC); \
  stmt; \
  mprotect(aligned_addr, count, PROT_READ | PROT_EXEC); \
} while (0)

#endif
