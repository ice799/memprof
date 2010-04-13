#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "arch.h"
#include "bin_api.h"
#include "tracer.h"
#include "tramp.h"
#include "util.h"

struct memprof_gc_stats {
  size_t gc_calls;
  double gc_time;
};

static struct tracer tracer;
static struct memprof_gc_stats memprof_gc_stats;
static void (*orig_garbage_collect)();

static void
gc_tramp()
{
  struct timeval start, end;
  double secs = 0;

  gettimeofday (&start, NULL);
  orig_garbage_collect();
  gettimeofday (&end, NULL);

  secs += end.tv_sec - start.tv_sec;
  secs += (end.tv_usec - start.tv_usec) / 1000000.0;

  memprof_gc_stats.gc_time += secs;
  memprof_gc_stats.gc_calls++;
}

static void
gc_trace_start() {
  orig_garbage_collect = bin_find_symbol("garbage_collect", NULL, 0);
  assert(orig_garbage_collect != NULL);
  dbg_printf("orig_garbage_collect: %p\n", orig_garbage_collect);

  insert_tramp("garbage_collect", gc_tramp);
}

static void
gc_trace_stop() {
  // TODO: figure out how to undo the tramp
}

static void
gc_trace_reset() {
  memset(&memprof_gc_stats, 0, sizeof(memprof_gc_stats));
}

static void
gc_trace_dump() {
  fprintf(stderr, "================ GC =======================================\n");
  fprintf(stderr, " # calls: %zd\n time: %fs\n", memprof_gc_stats.gc_calls, memprof_gc_stats.gc_time);
  fprintf(stderr, "===========================================================\n\n");
}

void install_gc_tracer()
{
  tracer.start = gc_trace_start;
  tracer.stop = gc_trace_stop;
  tracer.reset = gc_trace_reset;
  tracer.dump = gc_trace_dump;
  tracer.id = strdup("gc_tracer");

  trace_insert(&tracer);
}
