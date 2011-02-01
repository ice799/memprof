#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "arch.h"
#include "bin_api.h"
#include "json.h"
#include "tracer.h"
#include "tramp.h"
#include "util.h"

struct memprof_gc_stats {
  size_t gc_calls;
  uint32_t gc_time;
  uint32_t gc_utime;
  uint32_t gc_stime;
};

static struct tracer tracer;
static struct memprof_gc_stats stats;
static void (*orig_garbage_collect)();

static void
gc_tramp()
{
  uint64_t millis = 0;
  struct rusage usage_start, usage_end;

  millis = timeofday_ms();
  getrusage(RUSAGE_SELF, &usage_start);
  orig_garbage_collect();
  getrusage(RUSAGE_SELF, &usage_end);
  millis = timeofday_ms() - millis;

  stats.gc_time += millis;
  stats.gc_calls++;

  stats.gc_utime += TVAL_TO_INT64(usage_end.ru_utime) - TVAL_TO_INT64(usage_start.ru_utime);
  stats.gc_stime += TVAL_TO_INT64(usage_end.ru_stime) - TVAL_TO_INT64(usage_start.ru_stime);
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
  json_gen_integer(gen, stats.gc_time);

  json_gen_cstr(gen, "utime");
  json_gen_integer(gen, stats.gc_utime);

  json_gen_cstr(gen, "stime");
  json_gen_integer(gen, stats.gc_stime);
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
