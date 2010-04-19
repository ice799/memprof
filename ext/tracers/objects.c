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

static VALUE
objects_tramp() {
  if (last_obj) {
    stats.types[BUILTIN_TYPE(last_obj)]++;
    last_obj = 0;
  }

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

static void
objects_trace_dump(yajl_gen gen) {
  int i;

  yajl_gen_cstr(gen, "created");
  yajl_gen_integer(gen, stats.newobj_calls);

  yajl_gen_cstr(gen, "types");
  yajl_gen_map_open(gen);
  for (i=0; i<T_MASK+1; i++) {
    if (stats.types[i] > 0) {
      yajl_gen_format(gen, "%d", i);
      yajl_gen_integer(gen, stats.types[i]);
    }
  }
  yajl_gen_map_close(gen);

  // fprintf(stderr, "================ Objs =====================================\n");
  // fprintf(stderr, " # objs created: %zd\n", stats.newobj_calls);
  // fprintf(stderr, "===========================================================\n\n");
}

static void
gc_hooker()
{
  if (last_obj) {
    stats.types[BUILTIN_TYPE(last_obj)]++;
    last_obj = 0;
  }
}

void install_objects_tracer()
{
  if (!gc_hook) {
    gc_hook = Data_Wrap_Struct(rb_cObject, gc_hooker, NULL, NULL);
    rb_global_variable(&gc_hook);
  }

  tracer.start = objects_trace_start;
  tracer.stop = objects_trace_stop;
  tracer.reset = objects_trace_reset;
  tracer.dump = objects_trace_dump;
  tracer.id = "objects";

  trace_insert(&tracer);
}
