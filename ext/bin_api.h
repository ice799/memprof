#if !defined(BIN_API__)
#define BIN_API__
#include <stddef.h>
#include <stdint.h>

/*
 * Instead of getting the pagesize from sysconf over and over again, pollute
 * the global namespace with a dangerous name.
 *
 * XXX change this
 */
extern size_t pagesize;

/*
 * Some useful foward declarations for function protoypes here and elsewhere.
 */
struct tramp_st2_entry;
struct inline_tramp_st2_entry;

/*
 * bin_init - Initialize the binary format specific layer.
 */
void
bin_init();

/*
 * bin_find_symbol - find a symbol
 *
 * Given:
 *  - sym - a symbol name
 *  - size - optional out parameter
 *
 * This function will search for the symbol sym and return its address if
 * found, or NULL if the symbol could not be found.
 *
 * Optionally, this function will set its out parameter size equal to the
 * size of the symbol.
 */
void *
bin_find_symbol(const char *sym, size_t *size);

/*
 * bin_find_symbol_name - find a symbol's name
 *
 * Given:
 *  - sym - a symbol address
 *
 * This function will search for the symbol sym and return its name if
 * found, or NULL if the symbol could not be found.
 */
const char *
bin_find_symbol_name(void *sym);

/*
 * bin_allocate_page - allocate a page suitable for trampolines
 *
 * This function will allocate a page of memory in the right area of the
 * virtual address space and with the appropriate permissions for stage 2
 * trampoline code to live and execute.
 */
void *
bin_allocate_page(void);

/*
 * bin_type_size - Return size (in bytes) of a given type.
 *
 * Given:
 *  - type - a string representation of a type
 *
 * This function will return the size (in bytes) of the type. If no such type
 * can be found, return 0.
 */
size_t
bin_type_size(const char *type);

/*
 * bin_type_member_offset - Return the offset (in bytes) of member in type.
 *
 * Given:
 *  - type - a string representation of a type
 *  - member - a string representation of a field int he type
 *
 * This function will return the offset (in bytes) of member in type.
 *
 * On failure, this function returns -1.
 */
int
bin_type_member_offset(const char *type, const char *member);

/*
 * bin_update_image - Update a binary image in memory
 *
 * Given:
 *  - trampee - the name of the symbol to hook
 *  - tramp - the stage 2 trampoline entry
 *
 * this function will update the binary image so that all calls to trampee will
 * be routed to tramp.
 *
 * Returns 0 on success.
 */
int
bin_update_image(const char *trampee_addr, struct tramp_st2_entry *tramp);
#endif
