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

struct memprof_mysql_stats {
  size_t query_calls;
  double query_time;
};

static struct tracer tracer;
static struct memprof_mysql_stats stats;
static int (*orig_real_query)(void *mysql, const char *stmt_str, unsigned long length);

static int
real_query_tramp(void *mysql, const char *stmt_str, unsigned long length) {
  struct timeval start, end;
  double secs = 0;
  int ret;

  gettimeofday (&start, NULL);
  ret = orig_real_query(mysql, stmt_str, length);
  gettimeofday (&end, NULL);

  secs += end.tv_sec - start.tv_sec;
  secs += (end.tv_usec - start.tv_usec) / 1000000.0;

  stats.query_time += secs;
  stats.query_calls++;

  return ret;
}

static void
mysql_trace_start() {
  orig_real_query = bin_find_symbol("mysql_real_query", NULL, 1);
  if (orig_real_query) {
    struct tramp_st2_entry tmp;
    tmp.addr = real_query_tramp;
    bin_update_image("mysql_real_query", &tmp, NULL);
  }
}

static void
mysql_trace_stop() {
  if (orig_real_query) {
    struct tramp_st2_entry tmp;
    tmp.addr = orig_real_query;
    bin_update_image("mysql_real_query", &tmp, NULL);
  }
}

static void
mysql_trace_reset() {
  memset(&stats, 0, sizeof(stats));
}

static void
mysql_trace_dump(yajl_gen gen) {
  if (stats.query_calls > 0) {
    yajl_gen_cstr(gen, "queries");
    yajl_gen_integer(gen, stats.query_calls);

    yajl_gen_cstr(gen, "time");
    yajl_gen_double(gen, stats.query_time);
  }

  // fprintf(stderr, "================ Mysql ====================================\n");
  // fprintf(stderr, " # queries: %zd\n", stats.query_calls);
  // fprintf(stderr, " time querying: %fs\n", stats.query_time);
  // fprintf(stderr, "===========================================================\n\n");
}

void install_mysql_tracer()
{
  tracer.start = mysql_trace_start;
  tracer.stop = mysql_trace_stop;
  tracer.reset = mysql_trace_reset;
  tracer.dump = mysql_trace_dump;
  tracer.id = "mysql";

  trace_insert(&tracer);
}
