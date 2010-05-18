#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arch.h"
#include "bin_api.h"
#include "json.h"
#include "tracer.h"
#include "tramp.h"
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
static size_t (*malloc_usable_size)(void *ptr);

static void *
malloc_tramp(size_t size)
{
  void *ret = NULL;
  int err;

  ret = malloc(size);
  err = errno;

  stats.malloc_bytes_requested += size;
  stats.malloc_calls++;

  if (ret)
    stats.malloc_bytes_actual += malloc_usable_size(ret);

  errno = err;
  return ret;
}

static void *
calloc_tramp(size_t nmemb, size_t size)
{
  void *ret = NULL;
  int err;

  ret = calloc(nmemb, size);
  err = errno;

  stats.calloc_bytes_requested += (nmemb * size);
  stats.calloc_calls++;

  if (ret)
    stats.calloc_bytes_actual += malloc_usable_size(ret);

  errno = err;
  return ret;
}

static void *
realloc_tramp(void *ptr, size_t size)
{
  void *ret = NULL;
  int err;

  ret = realloc(ptr, size);
  err = errno;

  stats.realloc_bytes_requested += size;
  stats.realloc_calls++;

  if (ret)
    stats.realloc_bytes_actual += malloc_usable_size(ret);

  errno = err;
  return ret;
}

static void
free_tramp(void *ptr)
{
  if (ptr)
    stats.free_bytes_actual += malloc_usable_size(ptr);

  stats.free_calls++;

  free(ptr);
}

static void
malloc_trace_start()
{
  static int inserted = 0;

  if (!inserted)
    inserted = 1;
  else
    return;

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

  insert_tramp("malloc", malloc_tramp);
  insert_tramp("realloc", realloc_tramp);
  insert_tramp("calloc", calloc_tramp);
  insert_tramp("free", free_tramp);
}

static void
malloc_trace_stop()
{
}

static void
malloc_trace_reset()
{
  memset(&stats, 0, sizeof(stats));
}

static void
malloc_trace_dump(json_gen gen)
{
  if (stats.malloc_calls > 0) {
    json_gen_cstr(gen, "malloc");
    json_gen_map_open(gen);
    json_gen_cstr(gen, "calls");
    json_gen_integer(gen, stats.malloc_calls);
    json_gen_cstr(gen, "requested");
    json_gen_integer(gen, stats.malloc_bytes_requested);
    json_gen_cstr(gen, "actual");
    json_gen_integer(gen, stats.malloc_bytes_actual);
    json_gen_map_close(gen);
  }

  if (stats.realloc_calls > 0) {
    json_gen_cstr(gen, "realloc");
    json_gen_map_open(gen);
    json_gen_cstr(gen, "calls");
    json_gen_integer(gen, stats.realloc_calls);
    json_gen_cstr(gen, "requested");
    json_gen_integer(gen, stats.realloc_bytes_requested);
    json_gen_cstr(gen, "actual");
    json_gen_integer(gen, stats.realloc_bytes_actual);
    json_gen_map_close(gen);
  }

  if (stats.calloc_calls > 0) {
    json_gen_cstr(gen, "calloc");
    json_gen_map_open(gen);
    json_gen_cstr(gen, "calls");
    json_gen_integer(gen, stats.calloc_calls);
    json_gen_cstr(gen, "requested");
    json_gen_integer(gen, stats.calloc_bytes_requested);
    json_gen_cstr(gen, "actual");
    json_gen_integer(gen, stats.calloc_bytes_actual);
    json_gen_map_close(gen);
  }

  if (stats.free_calls > 0) {
    json_gen_cstr(gen, "free");
    json_gen_map_open(gen);
    json_gen_cstr(gen, "calls");
    json_gen_integer(gen, stats.free_calls);
    json_gen_cstr(gen, "actual");
    json_gen_integer(gen, stats.free_bytes_actual);
    json_gen_map_close(gen);
  }
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
