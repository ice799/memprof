#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

void
json_gen_reset(json_gen gen)
{
  json_gen_clear(gen);
  assert (gen->state[gen->depth] == json_gen_complete);
  gen->state[gen->depth] = json_gen_start;
  gen->print(gen->ctx, "\n", 1);
}

json_gen_status
json_gen_cstr(json_gen gen, const char * str)
{
  if (!str || str[0] == 0)
    return json_gen_null(gen);
  else
    return json_gen_string(gen, (unsigned char *)str, strlen(str));
}

json_gen_status
json_gen_format(json_gen gen, char *format, ...)
{
  va_list args;
  char *str;
  int bytes_printed = 0;

  json_gen_status ret;

  va_start(args, format);
  bytes_printed = vasprintf(&str, format, args);
  assert(bytes_printed != -1);
  va_end(args);

  ret = json_gen_string(gen, (unsigned char *)str, strlen(str));
  free(str);
  return ret;
}

json_gen_status
json_gen_pointer(json_gen gen, void* ptr)
{
  return json_gen_format(gen, "0x%x", ptr);
}
