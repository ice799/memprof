#if defined (_ARCH_x86_64_)

#include <stdint.h>
#include <string.h>

#include "arch.h"
#include "x86_gen.h"

/* This is the stage 1 inline trampoline for hooking the inlined add_freelist
 * function .
 *
 * NOTE: The original instruction mov %reg, freelist is 7 bytes wide,
 * whereas jmpq $displacement is only 5 bytes wide. We *must* pad out
 * the next two bytes. This will be important to remember below.
 */
struct inline_st1_tramp {
  unsigned char jmp;
  int32_t displacement;
  unsigned char pad[2];
} __attribute__((__packed__)) inline_st1_tramp = {
  .jmp  = '\xe9',
  .displacement = 0,
  .pad = {'\x90','\x90'},
};

struct inline_st1_base {
  unsigned char rex;
  unsigned char mov;
  unsigned char src_reg;
  int32_t displacement;
} __attribute__((__packed__)) inline_st1_mov = {
  .rex = 0,
  .mov = '\x89',
  .src_reg = 0,
  .displacement = 0
};

/*
 *  inline tramp stuff below
 */
static int
arch_check_ins(struct inline_st1_base *base)
{
  /* is it a mov instruction? */
  if (base->mov == 0x89 &&

      /* maybe. read the REX byte to find out for sure */
      (base->rex == 0x48 ||
       base->rex == 0x4c)) {

      /* success */
      return 1;
  }

  return 0;
}

int
arch_insert_inline_st2_tramp(void *addr, void *marker, void *trampoline, void *table_entry)
{
  struct inline_st1_base *base = addr;
  struct inline_tramp_st2_entry *entry = table_entry;

  if (!arch_check_ins(base))
    return 1;

  /* Sanity check. Ensure that the displacement from freelist to the next
   * instruction matches the mov_target. If so, we know this mov is
   * updating freelist.
   */
  if (marker - (void *)(base + 1) == base->displacement) {
    /* Before the stage 1 trampoline gets written, we need to generate
     * the code for the stage 2 trampoline. Let's copy over the REX byte
     * and the byte which mentions the source register into the stage 2
     * trampoline.
     */
    default_inline_st2_tramp.rex = base->rex;
    default_inline_st2_tramp.src_reg = base->src_reg;

    /* Setup the stage 1 trampoline. Calculate the displacement to
     * the stage 2 trampoline from the next instruction.
     *
     * REMEMBER!!!! The next instruction will be NOP after our stage 1
     * trampoline is written. This is 5 bytes into the structure, even
     * though the original instruction we overwrote was 7 bytes.
     */
    inline_st1_tramp.displacement = table_entry - (void *)(addr + 5);

    copy_instructions(addr, &inline_st1_tramp, sizeof(inline_st1_tramp));

    /* Finish setting up the stage 2 trampoline. */

    /* calculate the displacement to freelist from the next instruction.
     *
     * This is used to replicate the original instruction we overwrote.
     */
    default_inline_st2_tramp.mov_displacement = marker - (void *)&(entry->frame);

    /* fill in the displacement to freelist from the next instruction.
     *
     * This is to arrange for the new value in freelist to be in %rdi, and as such
     * be the first argument to the C handler. As per the amd64 ABI.
     */
    default_inline_st2_tramp.frame.rdi_source_displacement = marker - (void *)&(entry->frame.push_rbx);

    /* jmp back to the instruction after stage 1 trampoline was inserted
     *
     * This can be 5 or 7, it doesn't matter. If its 5, we'll hit our 2
     * NOPS. If its 7, we'll land directly on the next instruction.
     */
    default_inline_st2_tramp.jmp_displacement = (addr + sizeof(*base)) -
                                                 (table_entry + sizeof(default_inline_st2_tramp));

    /* write the address of our C level trampoline in to the structure */
    default_inline_st2_tramp.frame.addr = trampoline;

    memcpy(table_entry, &default_inline_st2_tramp, sizeof(default_inline_st2_tramp));

    return 0;
  }

  return 1;
}
#endif
