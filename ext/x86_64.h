#if !defined(_x86_64_h_)
#define _x86_64_h_

#include <stdint.h>
#include "arch.h"

/*
 * tramp_st2_entry - stage 2 trampoline entry
 *
 * This trampoline calls a handler function via the callee saved register %rbx.
 * The handler function is stored in the field 'addr'.
 *
 * A default pre-filled (except addr, of course) version of this trampoline is
 * provided so that the opcodes do not need to be filled in every time it is
 * used. You only need to set the addr field of default_st2_tramp and you are
 * ready to roll.
 *
 * This trampoline is the assembly code:
 *
 * push %rbx                      # save %rbx
 * push %rbp                      # save previous stack frame's %rbp
 * mov  %rsp, %rbp                # update %rbp to be current stack pointer
 * andl 0xFFFFFFFFFFFFFFF0, %rsp  # align stack pointer as per the ABI
 * mov  ADDR, %rbx                # move address of handler into %rbx
 * callq *%rbx                    # call handler
 * pop %rbx                       # restore %rbx
 * leave                          # restore %rbp, move stack pointer back
 * ret                            # return
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
  .push_rbx      = 0x53,
  .push_rbp      = 0x55,
  .save_rsp      = {0x48, 0x89, 0xe5},
  .align_rsp     = {0x48, 0x83, 0xe4, 0xf0},
  .mov           = {0x48, 0xbb},
  .addr          = 0,
  .call          = {0xff, 0xd3},
  .rbx_restore   = 0x5b,
  .leave         = 0xc9,
  .ret           = 0xc3,
};

/*
 * inline_tramp_st2_entry - stage 2 inline trampoline entry
 *
 * This trampoline calls a handler function via the callee saved register %rbx,
 * The handler function is stored in the field 'addr'.
 *
 * The major difference between this trampoline and the one above is that this
 * trampoline is intended to be used as the target of an 'inline trampoline',
 * that is code is redirected to this and the stack and registers may not be
 * 'ready' for a function call.
 *
 * This trampoline provides space to regenerate the overwritten mov instruction
 * and utmost care must be taken in order to recreate the overwritten
 * instruction.
 *
 * This trampoline is hit with a jmp (NOT A CALL), and as such must take care
 * to jmp back to resume execution.
 *
 * Like the above trampoline, this structure comes with a prefilled entry called
 * default_inline_st2_tramp that has most of the fields prepopulated.
 *
 * To use this structure you must fill in:
 *   - mov_displacement - should be set to the 32bit displacement from the next
 *     instruction (i.e. frame) to freelist. This is used to recreate the
 *     overwritten instruction.
 *
 *   - rdi_source_displacement - should be set to the 32bit displacement from
 *     the next instruction (i.e. push_rbx) to freelist. This is used to load
 *     freelist as the 1st argument to the handler.
 *
 *   - addr - the address of the handler function to call
 *
 *   - jmp_displacement - should be set to the 32bit displacement from the next
 *     instruction to the instruction after the stage 1 trampoline. This is
 *     used to resume execution after the handler has been hit.
 *
 *
 * This structure represents the assembly code:
 *
 * mov    SOURCE_REGISTER,-0x3f90d4a7(%rip)        # update freelist
 *
 * # save caller saved registers here
 *
 * push   %rax
 * push   %rcx
 * push   %rdx
 * push   %rsi
 * push   %rdi
 * push   %r8
 * push   %r9
 * push   %r10
 * push   %r11
 * push   %rbp
 * mov    %rsp,%rbp
 * mov    -0x3f90d4bf(%rip),%rdi        # move freelist into rdi as arg1
 * and    $0xfffffffffffffff0,%rsp      # align stack pointer
 * mov    $0x7ffff65e74e0,%rcx          # put handler function into position
 * callq  *%rcx                         # call handler
 *
 * # restore caller saved registers here
 *
 * leaveq
 * pop    %r11
 * pop    %r10
 * pop    %r9
 * pop    %r8
 * pop    %rdi
 * pop    %rsi
 * pop    %rdx
 * pop    %rcx
 * pop    %rax
 *
 * jmpq   0x433972 <gc_sweep+754>       # jump back
 */
static struct inline_tramp_st2_entry {
  unsigned char rex;
  unsigned char mov;
  unsigned char src_reg;
  uint32_t mov_displacement;

  struct {
    /*
     * XXX xmm0-xmm7 are caller saved, too.
     */
    unsigned char push_rax;
    unsigned char push_rcx;
    unsigned char push_rdx;
    unsigned char push_rsi;
    unsigned char push_rdi;
    unsigned char push_r8[2];
    unsigned char push_r9[2];
    unsigned char push_r10[2];
    unsigned char push_r11[2];

    unsigned char push_rbp;
    unsigned char mov_rsp_rbp[3];

    unsigned char mov_rdi[3];
    uint32_t rdi_source_displacement;

    unsigned char align_rsp[4];
    unsigned char mov[2];
    void *addr;
    unsigned char call[2];

    unsigned char leave;

    unsigned char pop_r11[2];
    unsigned char pop_r10[2];
    unsigned char pop_r9[2];
    unsigned char pop_r8[2];
    unsigned char pop_rdi;
    unsigned char pop_rsi;
    unsigned char pop_rdx;
    unsigned char pop_rcx;
    unsigned char pop_rax;
} __attribute__((__packed__)) frame;

  unsigned char jmp;
  uint32_t jmp_displacement;
} __attribute__((__packed__)) default_inline_st2_tramp = {
  .rex     = 0x48,
  .mov     = 0x89,
  .src_reg = 0x05,
  .mov_displacement = 0,

  .frame = {
    .push_rax = 0x50,
    .push_rcx = 0x51,
    .push_rdx = 0x52,
    .push_rsi = 0x56,
    .push_rdi = 0x57,
    .push_r8 = {0x41, 0x50},
    .push_r9 = {0x41, 0x51},
    .push_r10 = {0x41, 0x52},
    .push_r11 = {0x41, 0x53},
    .push_rbp = 0x55,
    .mov_rsp_rbp = {0x48, 0x89, 0xe5},
    .mov_rdi =  {0x48, 0x8b, 0x3d},
    .rdi_source_displacement = 0,
    .align_rsp = {0x48, 0x83, 0xe4, 0xf0},
    .mov = {0x48, 0xb9},
    .addr = 0,
    .call = {0xff, 0xd1},
    .leave = 0xc9,
    .pop_r11 = {0x41, 0x5b},
    .pop_r10 = {0x41, 0x5a},
    .pop_r9 = {0x41, 0x59},
    .pop_r8 = {0x41, 0x58},
    .pop_rdi = 0x5f,
    .pop_rsi = 0x5e,
    .pop_rdx = 0x5a,
    .pop_rcx = 0x59,
    .pop_rax = 0x58,
  },

  .jmp  = 0xe9,
  .jmp_displacement = 0,
};
#endif
