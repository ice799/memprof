#if !defined(__TRACER__H_)
#define __TRACER__H_

#include "json.h"

struct tracer {
  char *id;
  void (*start)();
  void (*stop)();
  void (*reset)();
  void (*dump)(json_gen);
};

typedef enum {
  TRACE_START,
  TRACE_STOP,
  TRACE_RESET,
  TRACE_DUMP,
} trace_fn;

int
trace_insert(struct tracer *trace);

int
trace_remove(const char *id);

int
trace_invoke_all(trace_fn fn);

int
trace_invoke(const char *id, trace_fn fn);

void
trace_set_output(json_gen gen);

json_gen
trace_get_output();

/* for now, these will live here */
extern void install_malloc_tracer();
extern void install_gc_tracer();
extern void install_fd_tracer();
extern void install_mysql_tracer();
extern void install_postgres_tracer();
extern void install_objects_tracer();
extern void install_memcache_tracer();
extern void install_resources_tracer();
#endif
