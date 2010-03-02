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
struct st1_base {
  unsigned char call;
  int32_t displacement;
} __attribute__((__packed__)) st1_mov = {
  .call         =  0xe8,
  .displacement =  0,
};

/*
 * plt_entry - procedure linkage table entry
 *
 * This struct is intended to be "laid onto" a piece of memory to ease the
 * parsing, use, modification, and length calculation of PLT entries.
 *
 * For example:
 *    jmpq  *0xaaf00d(%rip)   # jump to GOT entry
 *
 *    # the following instructions are only hit if function has not been
 *    # resolved previously.
 *
 *    pushq  $0x4e            # push ID
 *    jmpq  0xcafebabefeedface # invoke the link linker
 */
struct plt_entry {
    unsigned char jmp[2];
    int32_t jmp_disp;

    /* There is no reason (currently) to represent the pushq and jmpq
     * instructions which invoke the linker.
     *
     * We don't need those to hook the GOT; we only need the jmp_disp above.
     *
     * TODO represent the extra instructions
     */
    unsigned char pad[10];
} __attribute__((__packed__));

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
  mprotect(aligned_addr, (dest - aligned_addr) + count, PROT_READ|PROT_EXEC);

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

/*
 * arch_insert_st1_tramp - architecture specific stage 1 trampoline insert
 *
 * Given:
 *      - a start address (start),
 *      - the absolute address of the function to intercept (trampee),
 *      - the absolute address of the code to execute instead (tramp),
 *
 * This function will:
 *    - interpret address start as a struct st1_base,
 *    - check that the instruction at call is actually a call
 *    - if so, check that the target of the call is trampee
 *    - and change the target to tramp
 *
 * Returns 0 on success, 1 otherwise.
 */
int
arch_insert_st1_tramp(void *start, void *trampee, void *tramp)
{
  assert(start != NULL);
  assert(trampee != NULL);
  assert(tramp != NULL);

  int32_t fn_addr = 0;
  struct st1_base *check = start;

  if (check->call == 0xe8) {
    fn_addr = check->displacement;
    if ((trampee - (void *)(check + 1)) == fn_addr) {
      WRITE_INSTRUCTIONS(&check->displacement,
                         sizeof(*check),
                         (check->displacement = (tramp - (void *)(check + 1))));
      return 0;
    }
  }

  return 1;
}

/*
 * get_got_addr - given a PLT entry, return the global offset table entry that
 * the entry uses.
 */
static void *
get_got_addr(struct plt_entry *plt)
{
  assert(plt != NULL);
  /* the jump is relative to the start of the next instruction. */
  return (void *)&(plt->pad) + plt->jmp_disp;
}

/*
 * arch_overwrite_got - given the address of a PLT entry, overwrite the address
 * in the GOT that the PLT entry uses with the address in tramp.
 */
void
arch_overwrite_got(void *plt, void *tramp)
{
  assert(plt != NULL);
  assert(tramp != NULL);
  memcpy(get_got_addr(plt), &tramp, sizeof(void *));
  return;
}

/*
 * arch_get_st2_tramp - architecture specific stage 2 tramp accessor. This
 * function returns a pointer to the default stage 2 trampoline setting size
 * if a non-NULL pointer was passed in.
 */
void *
arch_get_st2_tramp(size_t *size)
{
  if (size) {
    *size = sizeof(default_st2_tramp);
  }

  return &default_st2_tramp;
}

/*
 * arch_get_inline_st2_tramp - architecture specific inline stage 2 tramp
 * accessor. This function returns a pointer to the default inline stage 2
 *  trampoline setting size if a non-NULL pointer was passed in.
 */
void *
arch_get_inline_st2_tramp(size_t *size)
{
  if (size) {
    *size = sizeof(default_inline_st2_tramp);
  }

  return &default_inline_st2_tramp;
}
#endif
