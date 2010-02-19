#if !defined(_x86_gen_)
#define _x86_gen_

#include <sys/mman.h>
#include <stdint.h>
#include "arch.h"

/* This structure makes it easier to find and update call instructions that
 * will become the stage 1 trampoline
 */
struct st1_base {
  unsigned char call;
  int32_t displacement;
} __attribute__((__packed__)) st1_mov = {
  .call        =  '\xe8',
  .displacement =  0,
};

struct plt_entry {
    unsigned char jmp[2];
    uint32_t jmp_disp;
    unsigned char pad[10];
} __attribute__((__packed__));

static inline void *
page_align(void *addr)
{
  return (void *)((size_t)addr & ~(0xFFFF));
}

static void
copy_instructions(void *to, void *from, size_t count)
{
  void *aligned_addr = page_align(to);
  mprotect(aligned_addr, (to - aligned_addr) + 10, PROT_READ|PROT_WRITE|PROT_EXEC);
  memcpy(to, from, count);
  mprotect(aligned_addr, (to - aligned_addr) + 10, PROT_READ|PROT_EXEC);

  return;
}

#define WRITE_INSTRUCTIONS(start, len, stmt) do { \
  mprotect(start, len, PROT_READ | PROT_WRITE | PROT_EXEC); \
  stmt; \
  mprotect(start, len, PROT_READ | PROT_EXEC); \
} while (0)

int
arch_insert_st1_tramp(void *start, void *trampee, void *tramp)
{
  int32_t fn_addr = 0;
  struct st1_base *check = (struct st1_base *)start;
  void *aligned_addr = page_align(&(check->displacement));

  if (check->call == 0xe8) {
    fn_addr = check->displacement;
    if ((trampee - (void *)(check + 1)) == fn_addr) {
      WRITE_INSTRUCTIONS(aligned_addr,
                         ((void *)&(check->displacement) - aligned_addr) + 10,
                         (check->displacement = (tramp - (void *)(check + 1))));
      return 1;
    }
  }

  return 0;
}

static void *
get_got_addr(struct plt_entry *plt)
{
  return (void *)&(plt->pad) + plt->jmp_disp;
}

void
arch_overwrite_got(void *plt, void *tramp)
{
  memcpy(get_got_addr(plt), &tramp, sizeof(void *));
  return;
}

void *
arch_get_st2_tramp(size_t *size)
{
  if (size) {
    *size = sizeof(struct tramp_st2_entry);
  }

  return &default_st2_tramp;
}


void *
arch_get_inline_st2_tramp(size_t *size)
{
  if (size) {
    *size = sizeof(struct inline_tramp_st2_entry);
  }

  return &default_inline_st2_tramp;
}
#endif
