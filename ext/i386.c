#if defined (_ARCH_i386_) || defined(_ARCH_i686_)

#include <stdint.h>
#include <string.h>

#include <sys/mman.h>

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
  uint32_t displacement;
  unsigned char pad;
} __attribute__((__packed__)) inline_st1_tramp = {
  .jmp  = '\xe9',
  .displacement = 0,
  .pad = '\x90',
};

struct inline_st1_base_short {
  unsigned char mov;
  void *addr;
} __attribute__((__packed__)) inline_st1_short = {
  .mov = '\xa3',
  .addr = 0,
};

struct inline_st1_base_long {
  unsigned char mov;
  unsigned char src_reg;
  void *addr;
} __attribute__((__packed__)) inline_st1_long = {
  .mov = '\x89',
  .src_reg = 0,
  .addr = 0
};

static int
arch_check_ins(unsigned char *base)
{

  /* if the byte is 0xa3 then we're moving from %eax, so
   * the length is only 5, so we don't need the pad.
   *
   * otherwise, we're moving from something else, so the
   * length is going to be 6 and we need a NOP.
   */

  /* is it a mov instruction? */
  if (*base == 0xa3)
    return 0;
  else if (*base == 0x89)
    return 1;

  return -1;
}

int
arch_insert_inline_st2_tramp(void *addr, void *marker, void *trampoline, void *table_entry)
{
  struct inline_st1_base_long *long_base = addr;
  struct inline_st1_base_short *short_base = addr;
  struct inline_st1_tramp *st1_tramp = addr;
  void *mov_target = NULL;
  size_t pad_length = 0;

  if ((pad_length = arch_check_ins(addr)) == -1)
    return 1;

  if (pad_length == 0) {
    mov_target = short_base->addr;
    default_inline_st2_tramp.mov = 0x90;
    default_inline_st2_tramp.src_reg = 0xa3;
    inline_st1_tramp.displacement = table_entry - (void *)(short_base + 1);
    default_inline_st2_tramp.jmp_displacement = (void *)(short_base + 1) - (table_entry + sizeof(default_inline_st2_tramp));
  } else {
    mov_target = long_base->addr;
    default_inline_st2_tramp.mov = long_base->mov;
    default_inline_st2_tramp.src_reg = long_base->src_reg;
    inline_st1_tramp.displacement = table_entry - (void *)(long_base + 1) + 1;
    default_inline_st2_tramp.jmp_displacement = (void *)(long_base + 1) - (table_entry + sizeof(default_inline_st2_tramp));
  }

  if (marker == mov_target) {
    default_inline_st2_tramp.mov_addr= default_inline_st2_tramp.frame.freelist = marker;
    default_inline_st2_tramp.frame.fn_addr = trampoline;
    if (pad_length) {
      copy_instructions(addr, &inline_st1_tramp, sizeof(inline_st1_tramp));
    } else {
      copy_instructions(addr, &inline_st1_tramp, sizeof(inline_st1_tramp) - 1);
    }
      memcpy(table_entry, &default_inline_st2_tramp, sizeof(default_inline_st2_tramp));
    return 0;
  }

  return 1;
}
#endif
