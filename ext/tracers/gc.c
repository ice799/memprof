#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "arch.h"
#include "bin_api.h"
#include "json.h"
#include "tracer.h"
#include "tramp.h"
#include "util.h"

struct memprof_gc_stats {
  size_t gc_calls;
  double gc_time;
};

static struct tracer tracer;
static struct memprof_gc_stats stats;
static void (*orig_garbage_collect)();

static void
gc_tramp()
{
  double secs = 0;

  secs = timeofday();
  orig_garbage_collect();
  secs = timeofday() - secs;

  stats.gc_time += secs;
  stats.gc_calls++;
}

static void
gc_trace_start() {
  static int inserted = 0;

  if (!inserted)
    inserted = 1;
  else
    return;

  orig_garbage_collect = bin_find_symbol("garbage_collect", NULL, 0);
  assert(orig_garbage_collect != NULL);
  dbg_printf("orig_garbage_collect: %p\n", orig_garbage_collect);

  insert_tramp("garbage_collect", gc_tramp);
}

static void
gc_trace_stop() {
}

static void
gc_trace_reset() {
  memset(&stats, 0, sizeof(stats));
}

static void
gc_trace_dump(json_gen gen) {
  json_gen_cstr(gen, "calls");
  json_gen_integer(gen, stats.gc_calls);

  json_gen_cstr(gen, "time");
  json_gen_double(gen, stats.gc_time);

  // fprintf(stderr, "================ GC =======================================\n");
  // fprintf(stderr, " # calls: %zd\n time: %fs\n", stats.gc_calls, stats.gc_time);
  // fprintf(stderr, "===========================================================\n\n");
}

void install_gc_tracer()
{
  tracer.start = gc_trace_start;
  tracer.stop = gc_trace_stop;
  tracer.reset = gc_trace_reset;
  tracer.dump = gc_trace_dump;
  tracer.id = "gc";

  trace_insert(&tracer);
}
