#include <assert.h>
#include <stdint.h>
#include "arch.h"
#include "x86_gen.h"

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
  int32_t offset = 0;
  struct st1_base *check = start;

  if (check->call == 0xe8) {
    fn_addr = check->displacement;
    if ((trampee - (void *)(check + 1)) == fn_addr) {
      offset = tramp - (void *)(check + 1);
      copy_instructions(&check->displacement, &offset, sizeof(offset));
      return 0;
    }
  }

  return 1;
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
