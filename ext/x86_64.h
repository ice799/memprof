#if !defined(_x86_64_h_)
#define _x86_64_h_

#include <stdint.h>
#include "arch.h"

/*
 * This is the "normal" stage 2 trampoline with a default entry pre-filled
 */
static struct tramp_st2_entry {
  unsigned char push_rbx;
  unsigned char push_rbp;
  unsigned char save_rsp[3];
  unsigned char align_rsp[4];
  unsigned char mov[2];
  void *addr;
  unsigned char call[2];
  unsigned char leave;
  unsigned char rbx_restore;
  unsigned char ret;
} __attribute__((__packed__)) default_st2_tramp = {
  .push_rbx      = 0x53,                // push rbx
  .push_rbp      = 0x55,                // push rbp
  .save_rsp      = {0x48, 0x89, 0xe5},  // mov rsp, rbp
  .align_rsp     = {0x48, 0x83, 0xe4, 0xf0}, // andl ~0x1, rsp
  .mov           = {'\x48', '\xbb'},    // mov addr into rbx
  .addr          = 0,                   // ^^^
  .call          = {'\xff', '\xd3'},    // call rbx
  .rbx_restore   = 0x5b,                // pop rbx
  .leave         = 0xc9,                // leave
  .ret           = 0xc3,                // ret
};

/*
 * This is the inline stage 2 trampoline with a default entry pre-filled
 */
static struct inline_tramp_st2_entry {
  unsigned char rex;
  unsigned char mov;
  unsigned char src_reg;
  uint32_t mov_displacement;

  struct {
    unsigned char push_rdi;
    unsigned char mov_rdi[3];
    uint32_t rdi_source_displacement;
    unsigned char push_rbx;
    unsigned char push_rbp;
    unsigned char save_rsp[3];
    unsigned char align_rsp[4];
    unsigned char mov[2];
    void *addr;
    unsigned char call[2];
    unsigned char leave;
    unsigned char rbx_restore;
    unsigned char rdi_restore;
  } __attribute__((__packed__)) frame;

  unsigned char jmp;
  uint32_t jmp_displacement;
} __attribute__((__packed__)) default_inline_st2_tramp = {
  .rex     = 0x48,
  .mov     = 0x89,
  .src_reg = 0x05,
  .mov_displacement = 0,

  .frame = {
    .push_rdi = 0x57,
    .mov_rdi = {'\x48', '\x8b', '\x3d'},
    .rdi_source_displacement = 0,
    .push_rbx = 0x53,
    .push_rbp = 0x55,
    .save_rsp = {'\x48', '\x89', '\xe5'},
    .align_rsp = {'\x48', '\x83', '\xe4', '\xf0'},
    .mov = {'\x48', '\xbb'},
    .addr = 0,
    .call = {'\xff', '\xd3'},
    .leave = 0xc9,
    .rbx_restore = 0x5b,
    .rdi_restore = 0x5f,
  },

  .jmp  = 0xe9,
  .jmp_displacement = 0,
};
#endif
