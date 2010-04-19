#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arch.h"
#include "bin_api.h"
#include "json.h"
#include "tracer.h"
#include "util.h"

struct memprof_memory_stats {
  size_t malloc_bytes_requested;
  size_t calloc_bytes_requested;
  size_t realloc_bytes_requested;

  size_t malloc_bytes_actual;
  size_t calloc_bytes_actual;
  size_t realloc_bytes_actual;
  size_t free_bytes_actual;

  size_t malloc_calls;
  size_t calloc_calls;
  size_t realloc_calls;
  size_t free_calls;
};

static struct tracer tracer;
static struct memprof_memory_stats stats;
static void *(*orig_malloc)(size_t), *(*orig_realloc)(void *, size_t),
            *(*orig_calloc)(size_t, size_t), (*orig_free)(void *);
static size_t (*malloc_usable_size)(void *ptr);

static void *
malloc_tramp(size_t size)
{
  void *ret = NULL;
  stats.malloc_bytes_requested += size;
  stats.malloc_calls++;
  ret = orig_malloc(size);
  stats.malloc_bytes_actual += malloc_usable_size(ret);
  return ret;
}

static void *
calloc_tramp(size_t nmemb, size_t size)
{
  void *ret = NULL;
  stats.calloc_bytes_requested += (nmemb * size);
  stats.calloc_calls++;
  ret = (*orig_calloc)(nmemb, size);
  stats.calloc_bytes_actual += malloc_usable_size(ret);
  return ret;
}

static void *
realloc_tramp(void *ptr, size_t size)
{
  /* TODO need to check malloc_usable_size of before/after i guess? */
  void *ret = NULL;
  stats.realloc_bytes_requested += size;
  stats.realloc_calls++;
  ret = orig_realloc(ptr, size);
  stats.realloc_bytes_actual += malloc_usable_size(ptr);
  return ret;
}

static void
free_tramp(void *ptr)
{
  stats.free_bytes_actual += malloc_usable_size(ptr);
  stats.free_calls++;
  orig_free(ptr);
}

static void
malloc_trace_start()
{
  struct tramp_st2_entry tmp;

  if (!malloc_usable_size) {
    malloc_usable_size = bin_find_symbol("MallocExtension_GetAllocatedSize", NULL, 1);
    if (!malloc_usable_size) {
      dbg_printf("tcmalloc was not found...\n");
      malloc_usable_size = bin_find_symbol("malloc_usable_size", NULL, 1);
    }
    if (!malloc_usable_size) {
      dbg_printf("malloc_usable_size was not found...\n");
      malloc_usable_size = bin_find_symbol("malloc_size", NULL, 1);
    }
    assert(malloc_usable_size != NULL);
    dbg_printf("malloc_usable_size: %p\n", malloc_usable_size);
  }

  tmp.addr = malloc_tramp;
  bin_update_image("malloc", &tmp, (void **)(&orig_malloc));
  assert(orig_malloc != NULL);
  dbg_printf("orig_malloc: %p\n", orig_malloc);

  tmp.addr = realloc_tramp;
  bin_update_image("realloc", &tmp,(void **)(&orig_realloc));
  dbg_printf("orig_realloc: %p\n", orig_realloc);

  tmp.addr = calloc_tramp;
  bin_update_image("calloc", &tmp, (void **)(&orig_calloc));
  dbg_printf("orig_calloc: %p\n", orig_calloc);

  tmp.addr = free_tramp;
  bin_update_image("free", &tmp, (void **)(&orig_free));
  assert(orig_free != NULL);
  dbg_printf("orig_free: %p\n", orig_free);
}

static void
malloc_trace_stop()
{
  struct tramp_st2_entry tmp;

  tmp.addr = orig_malloc;
  bin_update_image("malloc", &tmp, NULL);

  tmp.addr = orig_realloc;
  bin_update_image("realloc", &tmp, NULL);

  if (orig_calloc) {
    tmp.addr = orig_calloc;
    bin_update_image("calloc", &tmp, NULL);
  }

  tmp.addr = orig_free;
  bin_update_image("free", &tmp, NULL);
}

static void
malloc_trace_reset()
{
  memset(&stats, 0, sizeof(stats));
}

static void
malloc_trace_dump(yajl_gen gen)
{
  if (stats.malloc_calls > 0) {
    yajl_gen_cstr(gen, "malloc");
    yajl_gen_map_open(gen);
    yajl_gen_cstr(gen, "calls");
    yajl_gen_integer(gen, stats.malloc_calls);
    yajl_gen_cstr(gen, "requested");
    yajl_gen_integer(gen, stats.malloc_bytes_requested);
    yajl_gen_cstr(gen, "actual");
    yajl_gen_integer(gen, stats.malloc_bytes_actual);
    yajl_gen_map_close(gen);
  }

  if (stats.realloc_calls > 0) {
    yajl_gen_cstr(gen, "realloc");
    yajl_gen_map_open(gen);
    yajl_gen_cstr(gen, "calls");
    yajl_gen_integer(gen, stats.realloc_calls);
    yajl_gen_cstr(gen, "requested");
    yajl_gen_integer(gen, stats.realloc_bytes_requested);
    yajl_gen_cstr(gen, "actual");
    yajl_gen_integer(gen, stats.realloc_bytes_actual);
    yajl_gen_map_close(gen);
  }

  if (stats.calloc_calls > 0) {
    yajl_gen_cstr(gen, "calloc");
    yajl_gen_map_open(gen);
    yajl_gen_cstr(gen, "calls");
    yajl_gen_integer(gen, stats.calloc_calls);
    yajl_gen_cstr(gen, "requested");
    yajl_gen_integer(gen, stats.calloc_bytes_requested);
    yajl_gen_cstr(gen, "actual");
    yajl_gen_integer(gen, stats.calloc_bytes_actual);
    yajl_gen_map_close(gen);
  }

  if (stats.free_calls > 0) {
    yajl_gen_cstr(gen, "free");
    yajl_gen_map_open(gen);
    yajl_gen_cstr(gen, "calls");
    yajl_gen_integer(gen, stats.free_calls);
    yajl_gen_cstr(gen, "actual");
    yajl_gen_integer(gen, stats.free_bytes_actual);
    yajl_gen_map_close(gen);
  }

  // fprintf(stderr, "================ Malloc ===================================\n");
  // fprintf(stderr, " Calls to malloc: %zd, realloc: %zd, calloc: %zd, free: %zd\n",
  //     stats.malloc_calls,
  //     stats.realloc_calls,
  //     stats.calloc_calls,
  //     stats.free_calls);
  // fprintf(stderr, "================ Requested ================================\n");
  // fprintf(stderr, " Malloced: %zd, Realloced: %zd, Calloced: %zd\n",
  //     stats.malloc_bytes_requested, stats.realloc_bytes_requested,
  //     stats.calloc_bytes_requested);
  // fprintf(stderr, "================ Actual ===================================\n");
  // fprintf(stderr, " Malloced: %zd, Realloced: %zd, Calloced: %zd, Freed: %zd\n",
  //     stats.malloc_bytes_actual, stats.realloc_bytes_actual,
  //     stats.calloc_bytes_actual, stats.free_bytes_actual);
  // fprintf(stderr, "===========================================================\n\n");
}

void install_malloc_tracer()
{
  tracer.start = malloc_trace_start;
  tracer.stop = malloc_trace_stop;
  tracer.reset = malloc_trace_reset;
  tracer.dump = malloc_trace_dump;
  tracer.id = "memory";

  trace_insert(&tracer);
}
