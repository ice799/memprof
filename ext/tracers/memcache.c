#include <assert.h>
#include <errno.h>
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

struct memprof_memcache_stats {
  size_t get_calls;
  size_t get_responses[45];
};

static struct tracer tracer;
static struct memprof_memcache_stats stats;
static const char* (*_memcached_lib_version)(void);
static char* (*_memcached_get)(void *ptr, const char *key, size_t key_length, size_t *value_length, uint32_t *flags, void *error);

static char*
memcached_get_tramp(void *ptr, const char *key, size_t key_length, size_t *value_length, uint32_t *flags, void *error)
{
  char* ret = _memcached_get(ptr, key, key_length, value_length, flags, error);
  stats.get_calls++;
  int err = *(int*)error;
  stats.get_responses[err > 42 ? 44 : err]++;
  return ret;
}

static void
memcache_trace_start() {
  static int inserted = 0;

  if (!inserted)
    inserted = 1;
  else
    return;

  _memcached_lib_version = bin_find_symbol("memcached_lib_version", NULL, 1);
  if (_memcached_lib_version) {
    const char *version = _memcached_lib_version();
    if (strcmp(version, "0.32") == 0) {
      _memcached_get = bin_find_symbol("memcached_get", NULL, 1);
      insert_tramp("memcached_get", memcached_get_tramp);
    }
  }
}

static void
memcache_trace_stop() {
}

static void
memcache_trace_reset() {
  memset(&stats, 0, sizeof(stats));
}

static void
memcache_trace_dump(yajl_gen gen) {
  int i;

  if (stats.get_calls > 0) {
    yajl_gen_cstr(gen, "get");

    yajl_gen_map_open(gen);
    yajl_gen_cstr(gen, "calls");
    yajl_gen_integer(gen, stats.get_calls);

    yajl_gen_cstr(gen, "responses");
    yajl_gen_map_open(gen);
    for (i=0; i < 45; i++) {
      if (stats.get_responses[i]) {
        switch (i) {
          case 0:
            yajl_gen_cstr(gen, "success");
            break;
          case 16:
            yajl_gen_cstr(gen, "notfound");
            break;
          default:
            yajl_gen_format(gen, "%d", i);
        }
        yajl_gen_integer(gen, stats.get_responses[i]);
      }
    }
    yajl_gen_map_close(gen);

    yajl_gen_map_close(gen);
  }
}

void install_memcache_tracer()
{
  tracer.start = memcache_trace_start;
  tracer.stop = memcache_trace_stop;
  tracer.reset = memcache_trace_reset;
  tracer.dump = memcache_trace_dump;
  tracer.id = "memcache";

  trace_insert(&tracer);
}
