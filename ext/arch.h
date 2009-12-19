#if !defined (_ARCH_H_)
#define _ARCH_H_

#if defined(_ARCH_i386_) || defined(_ARCH_i686_)
#include "i386.h"
#elif defined(_ARCH_x86_64_)
#include "x86_64.h"
#else
#error "Unsupported architecture! Cannot continue compilation."
#endif

/*
 *  "normal" trampoline
 */
void *
arch_get_st2_tramp(size_t *size);

void
arch_insert_st1_tramp(void *start, void *trampee, void *tramp);

/*
 *	inline trampoline
 */
void *
arch_get_inline_st2_tramp(size_t *size);

int
arch_insert_inline_st2_tramp(void *addr, void *marker, void *trampoline, void *table_entry);

#endif
