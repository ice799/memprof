#if !defined(_x86_gen_)
#define _x86_gen_

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

  /*
   *  XXX This has to be commented out because setting certian sections to
   *      readonly (.got.plt, et al.) will cause the rtld to die.
   *
   *      There is no way to get the current permissions bits for a page.
   *
   *      The way to solve this is:
   *
   *        1.) copy_instructions can take a final_permissions mask and each
   *            overwrite site can put in the 'Right Thing'
   *
   *        2.) Each overwrite site can look up the 'Right Thing' in the object
   *            header and pass it in, ensuring the desired permissions are
   *            set after.
   *
   *  mprotect(aligned_addr, (dest - aligned_addr) + count, PROT_READ|PROT_EXEC);
   */
  return;
}
#endif
