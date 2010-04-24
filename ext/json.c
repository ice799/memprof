#include "json.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void
yajl_gen_reset(yajl_gen gen)
{
  yajl_gen_clear(gen);
  assert (gen->state[gen->depth] == yajl_gen_complete);
  gen->state[gen->depth] = yajl_gen_start;
  gen->print(gen->ctx, "\n", 1);
}

yajl_gen_status
yajl_gen_cstr(yajl_gen gen, const char * str)
{
  if (!str || str[0] == 0)
    return yajl_gen_null(gen);
  else
    return yajl_gen_string(gen, (unsigned char *)str, strlen(str));
}

yajl_gen_status
yajl_gen_format(yajl_gen gen, char *format, ...)
{
  va_list args;
  char *str;
  int bytes_printed = 0;

  yajl_gen_status ret;

  va_start(args, format);
  bytes_printed = vasprintf(&str, format, args);
  assert(bytes_printed != -1);
  va_end(args);

  ret = yajl_gen_string(gen, (unsigned char *)str, strlen(str));
  free(str);
  return ret;
}

yajl_gen_status
yajl_gen_pointer(yajl_gen gen, void* ptr)
{
  return yajl_gen_format(gen, "0x%x", ptr);
}
