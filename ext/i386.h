#if !defined(_i386_h_)
#define _i386_h_

#include <stdint.h>
#include "arch.h"

/*
 * This is the "normal" stage 2 trampoline with a default entry pre-filled
 */
static struct tramp_st2_entry {
  unsigned char ebx_save;
  unsigned char mov;
  void *addr;
  unsigned char call[2];
  unsigned char ebx_restore;
  unsigned char ret;
} __attribute__((__packed__)) default_st2_tramp = {
  .ebx_save      = 0x53,            /* push ebx */
  .mov           = 0xbb,            /* mov addr into ebx */
  .addr          = 0,               /* this is filled in later */
  .call          = {0xff, 0xd3},    /* calll *ebx */
  .ebx_restore   = 0x5b,            /* pop ebx */
  .ret           = 0xc3,            /* ret */
};


/*
 * This is the inline stage 2 trampoline with a default entry pre-filled
 */
static struct inline_tramp_st2_entry {

  /* this block will be filled in at runtime to replicate the overwritten
   * instruction.
   */
  unsigned char mov;
  unsigned char src_reg;
  void *mov_addr;

  /* this frame will arrange freelist to be passed as an argument to
   * the third and final trampoline (C level).
   */
  struct {
    unsigned char push_ebx;
    unsigned char pushl[2];
    void * freelist;
    unsigned char mov_ebx;
    void * fn_addr;
    unsigned char call[2];
    unsigned char pop_ebx;
    unsigned char restore_ebx;
  } __attribute__((__packed__)) frame;

  /* this block jumps back to resume execution */
  unsigned char jmp;
  uint32_t jmp_displacement;
}  __attribute__((__packed__)) default_inline_st2_tramp = {
  .mov     = 0x89,
  .src_reg = 0,
  .mov_addr = 0,

  .frame = {
   .push_ebx = 0x53,
   .pushl = {0xff, 0x35},
   .freelist = 0,
   .mov_ebx = 0xbb,
   .fn_addr = 0,
   .call = {0xff, 0xd3},
   .pop_ebx = 0x5b,
   .restore_ebx = 0x5b,
  },

  .jmp  = 0xe9,
  .jmp_displacement = 0,
};
#endif
