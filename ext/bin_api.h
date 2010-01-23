#if !defined(BIN_API__)
#define BIN_API__
#include <stddef.h>
#include <stdint.h>

/* XXX get rid of this */
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

/*
 *  EXPORTED API.
 */
void
bin_init();

void *
bin_find_symbol(char *sym, size_t *size);

void *
bin_find_got_addr(char *sym, void *cookie);

void *
bin_allocate_page();

int
bin_type_size(char *type);

int
bin_type_member_offset(char *type, char *member);

void
bin_update_image(int entry, char *trampee_addr, struct tramp_st2_entry *tramp);
#endif
