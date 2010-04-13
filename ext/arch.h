#if !defined (_ARCH_H_)
#define _ARCH_H_

/*
 * The only supported architectures at this time are i{3-6}86 and amd64. All
 * other architectures should fail.
 */
#if defined(_ARCH_i386_) || defined(_ARCH_i686_)
#include "i386.h"
#elif defined(_ARCH_x86_64_)
#include "x86_64.h"
#else
#error "Unsupported architecture! Cannot continue compilation."
#endif

#include "x86_gen.h"

/*
 * arch_get_st2_tramp - architecture specific stage 2 trampoline getter
 *
 * This function will return a pointer to an area of memory that contains
 * the stage 2 trampoline for this architecture.
 *
 * This function will also set the out parameter size equal to the size of this
 * region, if size is not NULL.
 */
void *
arch_get_st2_tramp(size_t *size);

/*
 * arch_insert_st1_tramp - architecture specific stage 1 trampoline insert
 *
 * Given:
 *  start - a start address to attempt to insert a trampoline at
 *  trampee - the address of the function to trampoline
 *  tramp - a pointer to the trampoline
 *
 * This function will inspect the start address to determine if an instruction
 * which transfers execution to the trampee is present. If so, this function
 * will rewrite the instruction to ensure that tramp is called instead.
 *
 * Returns 0 on success, 1 otherwise.
 */
int
arch_insert_st1_tramp(void *start, void *trampee, void *tramp);

/*
 * arch_get_inline_st2_tramp - architecture specific inline stage 2 tramp getter
 *
 * This function will return a pointer to an area of memory that contains the
 * stage 2 inline trampoline for this architecture.
 *
 * This function will also set the out parameter size equal to the size of this
 * region, if size is not NULL.
 */
void *
arch_get_inline_st2_tramp(size_t *size);

/*
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
arch_insert_inline_st2_tramp(void *addr, void *marker, void *trampoline, void *table_entry);
#endif
