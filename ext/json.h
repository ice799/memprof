#if !defined(__JSON__H_)
#define __JSON__H_

#include <stdarg.h>
#include <yajl/yajl_gen.h>

/* HAX: copied from internal yajl_gen.c (PATCH yajl before building instead)
 */

typedef enum {
    yajl_gen_start,
    yajl_gen_map_start,
    yajl_gen_map_key,
    yajl_gen_map_val,
    yajl_gen_array_start,
    yajl_gen_in_array,
    yajl_gen_complete,
    yajl_gen_error
} yajl_gen_state;

struct yajl_gen_t
{
    unsigned int depth;
    unsigned int pretty;
    const char * indentString;
    yajl_gen_state state[YAJL_MAX_DEPTH];
    yajl_print_t print;
    void * ctx; /* yajl_buf */
    /* memory allocation routines */
    yajl_alloc_funcs alloc;
};

/* END HAX
 */

void
yajl_gen_reset(yajl_gen gen);

yajl_gen_status
yajl_gen_cstr(yajl_gen gen, const char * str);

yajl_gen_status
yajl_gen_format(yajl_gen gen, char *format, ...);

yajl_gen_status
yajl_gen_pointer(yajl_gen gen, void* ptr);

#endif