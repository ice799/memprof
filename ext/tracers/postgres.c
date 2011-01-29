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

struct memprof_postgres_stats {
  size_t query_calls;
};

static struct tracer tracer;
static struct memprof_postgres_stats stats;
static void * (*orig_PQexec)(void *postgres, const char *stmt);

static void *
PQexec_tramp(void *postgres, const char *stmt) {
  void *ret;

  ret = orig_PQexec(postgres, stmt);
  stats.query_calls++;

  return ret;
}

static void
postgres_trace_start() {
  static int inserted = 0;

  if (!inserted)
    inserted = 1;
  else
    return;

  orig_PQexec = bin_find_symbol("PQexec", NULL, 1);
  if (orig_PQexec)
    insert_tramp("PQexec", PQexec_tramp);
}

static void
postgres_trace_stop() {
}

static void
postgres_trace_reset() {
  memset(&stats, 0, sizeof(stats));
}

static void
postgres_trace_dump(json_gen gen) {
  if (stats.query_calls > 0) {
    json_gen_cstr(gen, "queries");
    json_gen_integer(gen, stats.query_calls);
  }
}

void install_postgres_tracer()
{
  tracer.start = postgres_trace_start;
  tracer.stop = postgres_trace_stop;
  tracer.reset = postgres_trace_reset;
  tracer.dump = postgres_trace_dump;
  tracer.id = "postgres";

  trace_insert(&tracer);
}
