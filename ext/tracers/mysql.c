#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "arch.h"
#include "bin_api.h"
#include "tracer.h"
#include "tramp.h"
#include "util.h"

struct memprof_mysql_stats {
  size_t query_calls;
  double query_time;
};

static struct tracer tracer;
static struct memprof_mysql_stats memprof_mysql_stats;
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

  memprof_mysql_stats.query_time += secs;
  memprof_mysql_stats.query_calls++;

  return ret;
}

static void
mysql_trace_start() {
  struct tramp_st2_entry tmp;
  tmp.addr = real_query_tramp;
  bin_update_image("mysql_real_query", &tmp, (void **)(&orig_real_query));
}

static void
mysql_trace_stop() {
  // TODO: figure out how to undo the tramp
}

static void
mysql_trace_reset() {
  memset(&memprof_mysql_stats, 0, sizeof(memprof_mysql_stats));
}

static void
mysql_trace_dump() {
  fprintf(stderr, "================ Mysql ====================================\n");
  fprintf(stderr, " # queries: %zd\n", memprof_mysql_stats.query_calls);
  fprintf(stderr, " time querying: %fs\n", memprof_mysql_stats.query_time);
  fprintf(stderr, "===========================================================\n\n");
}

void install_mysql_tracer()
{
  tracer.start = mysql_trace_start;
  tracer.stop = mysql_trace_stop;
  tracer.reset = mysql_trace_reset;
  tracer.dump = mysql_trace_dump;
  tracer.id = strdup("mysql_tracer");

  trace_insert(&tracer);
}
