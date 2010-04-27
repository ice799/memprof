#if !defined(__JSON__H_)
#define __JSON__H_

#include <stdarg.h>
#include <json/json_gen.h>

/* HAX: copied from internal json_gen.c (PATCH json before building instead)
 */

typedef enum {
    json_gen_start,
    json_gen_map_start,
    json_gen_map_key,
    json_gen_map_val,
    json_gen_array_start,
    json_gen_in_array,
    json_gen_complete,
    json_gen_error
} json_gen_state;

struct json_gen_t
{
    unsigned int depth;
    unsigned int pretty;
    const char * indentString;
    json_gen_state state[YAJL_MAX_DEPTH];
    json_print_t print;
    void * ctx; /* json_buf */
    /* memory allocation routines */
    json_alloc_funcs alloc;
};

/* END HAX
 */

void
json_gen_reset(json_gen gen);

json_gen_status
json_gen_cstr(json_gen gen, const char * str);

json_gen_status
json_gen_format(json_gen gen, char *format, ...);

json_gen_status
json_gen_pointer(json_gen gen, void* ptr);

#endif