#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arch.h"
#include "bin_api.h"
#include "tracer.h"
#include "util.h"

struct memprof_malloc_stats {
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
static struct memprof_malloc_stats memprof_malloc_stats;
static void *(*orig_malloc)(size_t), *(*orig_realloc)(void *, size_t),
            *(*orig_calloc)(size_t, size_t), (*orig_free)(void *);
static size_t (*malloc_usable_size)(void *ptr);

static void *
malloc_tramp(size_t size)
{
  void *ret = NULL;
  memprof_malloc_stats.malloc_bytes_requested += size;
  memprof_malloc_stats.malloc_calls++;
  ret = orig_malloc(size);
  memprof_malloc_stats.malloc_bytes_actual += malloc_usable_size(ret);
  return ret;
}

static void *
calloc_tramp(size_t nmemb, size_t size)
{
  void *ret = NULL;
  memprof_malloc_stats.calloc_bytes_requested += (nmemb * size);
  memprof_malloc_stats.calloc_calls++;
  ret = (*orig_calloc)(nmemb, size);
  memprof_malloc_stats.calloc_bytes_actual += malloc_usable_size(ret);
  return ret;
}

static void *
realloc_tramp(void *ptr, size_t size)
{
  /* TODO need to check malloc_usable_size of before/after i guess? */
  void *ret = NULL;
  memprof_malloc_stats.realloc_bytes_requested += size;
  memprof_malloc_stats.realloc_calls++;
  ret = orig_realloc(ptr, size);
  memprof_malloc_stats.realloc_bytes_actual += malloc_usable_size(ptr);
  return ret;
}

static void
free_tramp(void *ptr)
{
  memprof_malloc_stats.free_bytes_actual += malloc_usable_size(ptr);
  memprof_malloc_stats.free_calls++;
  orig_free(ptr);
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
  memset(&memprof_malloc_stats, 0, sizeof(memprof_malloc_stats));
}

static void
malloc_trace_dump()
{
  fprintf(stderr, "================ Requested ====================\n");
  fprintf(stderr, "Malloced: %zd, Realloced: %zd, Calloced: %zd\n",
      memprof_malloc_stats.malloc_bytes_requested, memprof_malloc_stats.realloc_bytes_requested,
      memprof_malloc_stats.calloc_bytes_requested);
  fprintf(stderr, "================ Actual ====================\n");
  fprintf(stderr, "Malloced: %zd, Realloced: %zd, Calloced: %zd, Freed: %zd\n",
      memprof_malloc_stats.malloc_bytes_actual, memprof_malloc_stats.realloc_bytes_actual,
      memprof_malloc_stats.calloc_bytes_actual, memprof_malloc_stats.free_bytes_actual);
  fprintf(stderr, "================ Call count ====================\n");
  fprintf(stderr, "Calls to malloc: %zd, realloc: %zd, calloc: %zd, free: %zd\n",
      memprof_malloc_stats.malloc_calls,
      memprof_malloc_stats.realloc_calls,
      memprof_malloc_stats.calloc_calls,
      memprof_malloc_stats.free_calls);
}

static void
malloc_trace_start()
{
  struct tramp_st2_entry tmp;

  if (!malloc_usable_size) {
    if ((malloc_usable_size =
          bin_find_symbol("MallocExtension_GetAllocatedSize", NULL, 1)) == NULL) {
      malloc_usable_size = bin_find_symbol("malloc_usable_size", NULL, 1);
      dbg_printf("tcmalloc was not found...\n");
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

void install_malloc_tracer()
{
  tracer.start = malloc_trace_start;
  tracer.stop = malloc_trace_stop;
  tracer.reset = malloc_trace_reset;
  tracer.dump = malloc_trace_dump;
  tracer.id = strdup("malloc_tracer");

  trace_insert(&tracer);
}
