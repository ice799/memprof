#if !defined(__TRACER__H_)
#define __TRACER__H_

struct tracer {
  char *id;
  void (*start)();
  void (*stop)();
  void (*reset)();
  void (*dump)();
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

/* for now, these will live here */
extern void install_malloc_tracer();
#endif
