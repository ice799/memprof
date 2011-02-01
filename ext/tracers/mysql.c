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
#include "tracers/sql.h"
#include "tramp.h"
#include "util.h"

struct memprof_mysql_stats {
  size_t query_calls;
  uint32_t query_time;

  size_t query_calls_by_type[sql_UNKNOWN];
  uint32_t query_time_by_type[sql_UNKNOWN];
};

static struct tracer tracer;
static struct memprof_mysql_stats stats;

static int (*orig_real_query)(void *mysql, const char *stmt_str, unsigned long length);
static int (*orig_send_query)(void *mysql, const char *stmt_str, unsigned long length);

static int
real_query_tramp(void *mysql, const char *stmt_str, unsigned long length) {
  enum memprof_sql_type type;
  uint64_t millis = 0;
  int ret;

  millis = timeofday_ms();
  ret = orig_real_query(mysql, stmt_str, length);
  millis = timeofday_ms() - millis;

  stats.query_time += millis;
  stats.query_calls++;

  type = memprof_sql_query_type(stmt_str, length);
  stats.query_time_by_type[type] += millis;
  stats.query_calls_by_type[type]++;

  return ret;
}

static int
send_query_tramp(void *mysql, const char *stmt_str, unsigned long length) {
  enum memprof_sql_type type;
  int ret;

  ret = orig_send_query(mysql, stmt_str, length);
  stats.query_calls++;

  type = memprof_sql_query_type(stmt_str, length);
  stats.query_calls_by_type[type]++;

  return ret;
}

static void
mysql_trace_start() {
  static int inserted = 0;

  if (!inserted)
    inserted = 1;
  else
    return;

  orig_real_query = bin_find_symbol("mysql_real_query", NULL, 1);
  if (orig_real_query)
    insert_tramp("mysql_real_query", real_query_tramp);

  orig_send_query = bin_find_symbol("mysql_send_query", NULL, 1);
  if (orig_send_query)
    insert_tramp("mysql_send_query", send_query_tramp);
}

static void
mysql_trace_stop() {
}

static void
mysql_trace_reset() {
  memset(&stats, 0, sizeof(stats));
}

static void
mysql_trace_dump(json_gen gen) {
  enum memprof_sql_type i;

  if (stats.query_calls > 0) {
    json_gen_cstr(gen, "queries");
    json_gen_integer(gen, stats.query_calls);

    json_gen_cstr(gen, "time");
    json_gen_integer(gen, stats.query_time);

    json_gen_cstr(gen, "types");
    json_gen_map_open(gen);
    for (i=0; i<=sql_UNKNOWN; i++) {
      json_gen_cstr(gen, memprof_sql_type_str(i));
      json_gen_map_open(gen);

      json_gen_cstr(gen, "queries");
      json_gen_integer(gen, stats.query_calls_by_type[i]);

      json_gen_cstr(gen, "time");
      json_gen_integer(gen, stats.query_time_by_type[i]);

      json_gen_map_close(gen);
    }
    json_gen_map_close(gen);
  }
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
