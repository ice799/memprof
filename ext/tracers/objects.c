#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arch.h"
#include "bin_api.h"
#include "json.h"
#include "tracer.h"
#include "tramp.h"
#include "util.h"
#include "ruby.h"

struct memprof_objects_stats {
  size_t newobj_calls;
  size_t types[T_MASK+1];
};

static struct tracer tracer;
static struct memprof_objects_stats stats;
static VALUE (*orig_rb_newobj)();

static VALUE last_obj = 0;
static VALUE gc_hook = 0;

static void
record_last_obj()
{
  if (last_obj) {
    stats.types[BUILTIN_TYPE(last_obj)]++;
    last_obj = 0;
  }
}

static VALUE
objects_tramp() {
  record_last_obj();
  stats.newobj_calls++;
  last_obj = orig_rb_newobj();
  return last_obj;
}

static void
objects_trace_start() {
  orig_rb_newobj = bin_find_symbol("rb_newobj", NULL, 0);
  assert(orig_rb_newobj != NULL);
  dbg_printf("orig_rb_newobj: %p\n", orig_rb_newobj);

  insert_tramp("rb_newobj", objects_tramp);
}

static void
objects_trace_stop() {
  // TODO: figure out how to undo the tramp
}

static void
objects_trace_reset() {
  memset(&stats, 0, sizeof(stats));
  last_obj = 0;
}

static inline char*
type_string(int type) {
  switch (type) {
    case T_NONE:
      return "none";
    case T_NIL:
      return "nil";
    case T_OBJECT:
      return "object";
    case T_CLASS:
      return "class";
    case T_ICLASS:
      return "iclass";
    case T_MODULE:
      return "module";
    case T_FLOAT:
      return "float";
    case T_STRING:
      return "string";
    case T_REGEXP:
      return "regexp";
    case T_ARRAY:
      return "array";
    case T_FIXNUM:
      return "fixnum";
    case T_HASH:
      return "hash";
    case T_STRUCT:
      return "struct";
    case T_BIGNUM:
      return "bignum";
    case T_FILE:
      return "file";
    case T_TRUE:
      return "true";
    case T_FALSE:
      return "false";
    case T_DATA:
      return "data";
    case T_MATCH:
      return "match";
    case T_SYMBOL:
      return "symbol";
    case T_BLKTAG:
      return "blktag";
    case T_UNDEF:
      return "undef";
    case T_VARMAP:
      return "varmap";
    case T_SCOPE:
      return "scope";
    case T_NODE:
      return "node";
    default:
      return "unknown";
  }
}

static void
objects_trace_dump(yajl_gen gen) {
  int i;
  record_last_obj();

  yajl_gen_cstr(gen, "created");
  yajl_gen_integer(gen, stats.newobj_calls);

  yajl_gen_cstr(gen, "types");
  yajl_gen_map_open(gen);
  for (i=0; i<T_MASK+1; i++) {
    if (stats.types[i] > 0) {
      yajl_gen_cstr(gen, type_string(i));
      yajl_gen_integer(gen, stats.types[i]);
    }
  }
  yajl_gen_map_close(gen);

  // fprintf(stderr, "================ Objs =====================================\n");
  // fprintf(stderr, " # objs created: %zd\n", stats.newobj_calls);
  // fprintf(stderr, "===========================================================\n\n");
}

void install_objects_tracer()
{
  if (!gc_hook) {
    gc_hook = Data_Wrap_Struct(rb_cObject, record_last_obj, NULL, NULL);
    rb_global_variable(&gc_hook);
  }

  tracer.start = objects_trace_start;
  tracer.stop = objects_trace_stop;
  tracer.reset = objects_trace_reset;
  tracer.dump = objects_trace_dump;
  tracer.id = "objects";

  trace_insert(&tracer);
}
