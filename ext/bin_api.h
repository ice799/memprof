#if !defined(BIN_API__)
#define BIN_API__
#include <stddef.h>
#include <stdint.h>

/* generic file format stuff */
extern void *text_segment;
extern unsigned long text_segment_len;
extern size_t pagesize;

/*
 *  trampoline specific stuff
 */
extern struct tramp_st2_entry *tramp_table;
extern size_t tramp_size;

/*
 *  inline trampoline specific stuff
 */
extern struct inline_tramp_st2_entry *inline_tramp_table;
extern size_t inline_tramp_size;

void
update_callqs(int entry, void *trampee_addr, void *tramp);

/*
 *  EXPORTED API.
 */
void
bin_init();

void *
bin_find_symbol(char *sym, size_t *size);

void
bin_update_image(int entry, void *trampee_addr, void *tramp);

void *
bin_allocate_page();

#endif
