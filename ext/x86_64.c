#if defined (_ARCH_x86_64_)

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "arch.h"
#include "x86_gen.h"

/*
 * inline_st1_tramp - inline stage 1 trampoline
 *
 * This is the stage 1 inline trampoline that will replace the mov instruction
 * that updates freelist from the inlined function add_freelist.
 *
 * Note that the mov instruction is 7 bytes wide, so this trampoline needs two
 * bytes of NOPs to keep it 7 bytes wide.
 *
 * In order to use this structure, you must set the displacement field to a
 * 32bit displacement from the next instruction to the stage 2 trampoline.
 *
 * TODO replace the 2, 1 byte NOPs with a wider 16bit NOP.
 *
 * Original code:
 *
 *  mov REGISTER, freelist  # update the head of the freelist
 *
 *  size = 7 bytes
 *
 * Code after tramp:
 *
 *  jmp 0xfeedface(%rip)    # jump to stage 2 trampoline
 *  nop                     # 1 byte NOP pad
 *  nop                     # 1 byte NOP pad
 *
 *  size = 7 bytes
 */
struct inline_st1_tramp {
  unsigned char jmp;
  int32_t displacement;
  unsigned char pad[2];
} __attribute__((__packed__)) inline_st1_tramp = {
  .jmp  = 0xe9,
  .displacement = 0,
  .pad = {0x90, 0x90},
};

/*
 * inline_st1_base - inline stage 1 base instruction
 *
 * This structure is designed to be "laid onto" a piece of memory to ease the
 * parsing, modification, and length calculation of the original instruction
 * that will be overwritten with a jmp to the stage 2 trampoline.
 *
 * In order to use this structure, you must set the displacement, rex, and
 * rex bytes to accurately represent the original instruction.
 */
struct inline_st1_base {
  unsigned char rex;
  unsigned char mov;
  unsigned char src_reg;
  int32_t displacement;
} __attribute__((__packed__)) inline_st1_mov = {
  .rex = 0,
  .mov = 0x89,
  .src_reg = 0,
  .displacement = 0
};

/*
 * arch_check_ins - architecture specific instruction check
 *
 * This function checks the opcodes at a specific adderss to see if
 * they could be a move instruction.
 *
 * Returns 1 if the address matches a mov, 0 otherwise.
 */
static int
arch_check_ins(struct inline_st1_base *base)
{
  assert(base != NULL);

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

/*
 * arch_insert_inline_st2_tramp - architecture specific stage 2 tramp insert
 *
 * Given:
 *    - addr - The base address of an instruction sequence.
 *
 *    - marker - This is the marker to search for which will indicate that the
 *      instruction sequence has been located.
 *
 *    - trampoline - The address of the handler to redirect execution to.
 *
 *    - table_entry - Address of where the stage 2 trampoline code will reside
 *
 * This function will:
 *    Insert and setup the stage 1 and stage 2 trampolines if addr points to an
 *    instruction that could be from the inlined add_freelist function.
 *
 * This function returns 1 on failure and 0 on success.
 */
int
arch_insert_inline_st2_tramp(void *addr, void *marker, void *trampoline, void *table_entry)
{
  assert(addr != NULL);
  assert(marker != NULL);
  assert(trampoline != NULL);
  assert(table_entry != NULL);

  struct inline_st1_base *base = addr;
  struct inline_tramp_st2_entry *entry = table_entry;

  /* TODO make this a compile time assert */
  assert(sizeof(struct inline_st1_base) ==
         sizeof(struct inline_st1_tramp));

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
