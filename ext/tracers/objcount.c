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

struct memprof_objcount_stats {
  size_t newobj_calls;
};

static struct tracer tracer;
static struct memprof_objcount_stats stats;
static VALUE (*orig_rb_newobj)();

static VALUE
objcount_tramp() {
  stats.newobj_calls++;
  return orig_rb_newobj();
}

static void
objcount_trace_start() {
  orig_rb_newobj = bin_find_symbol("rb_newobj", NULL, 0);
  assert(orig_rb_newobj != NULL);
  dbg_printf("orig_rb_newobj: %p\n", orig_rb_newobj);

  insert_tramp("rb_newobj", objcount_tramp);
}

static void
objcount_trace_stop() {
  // TODO: figure out how to undo the tramp
}

static void
objcount_trace_reset() {
  memset(&stats, 0, sizeof(stats));
}

static void
objcount_trace_dump(yajl_gen gen) {
  yajl_gen_cstr(gen, "objects");
  yajl_gen_integer(gen, stats.newobj_calls);

  // fprintf(stderr, "================ Objs =====================================\n");
  // fprintf(stderr, " # objs created: %zd\n", stats.newobj_calls);
  // fprintf(stderr, "===========================================================\n\n");
}

void install_objcount_tracer()
{
  tracer.start = objcount_trace_start;
  tracer.stop = objcount_trace_stop;
  tracer.reset = objcount_trace_reset;
  tracer.dump = objcount_trace_dump;
  tracer.id = "objcount";

  trace_insert(&tracer);
}
