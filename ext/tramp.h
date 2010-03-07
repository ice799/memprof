#if !defined(TRAMP__)
#define TRAMP__

/*
 * create_tramp_table - create the trampoline tables.
 */
void
create_tramp_table();

/*
 * insert_tramp - insert a trampoline.
 *
 * Given:
 *  - trampee: function in which we want to install the trampoline.
 *  - tramp:   pointer to the function to be called from the trampoline.
 *
 * This function is responsible for installing the requested trampoline
 * at the location of "trampee".  This results in tramp() being called
 * whenever trampee() is executed.
 */
void
insert_tramp(const char *trampee, void *tramp);
#endif
