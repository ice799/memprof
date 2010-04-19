#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "arch.h"
#include "bin_api.h"
#include "json.h"
#include "tracer.h"
#include "tramp.h"
#include "util.h"

struct memprof_fd_stats {
  size_t read_calls;
  double read_time;
  size_t read_requested_bytes;
  size_t read_actual_bytes;

  size_t connect_calls;
  double connect_time;

  size_t readv_calls;
  size_t write_calls;
  size_t writev_calls;
};

static struct tracer tracer;
static struct memprof_fd_stats stats;

static ssize_t
read_tramp(int fildes, void *buf, size_t nbyte) {
  struct timeval start, end;
  double secs = 0;
  int err;
  ssize_t ret;

  gettimeofday (&start, NULL);
  ret = read(fildes, buf, nbyte);
  err = errno;
  gettimeofday (&end, NULL);

  secs += end.tv_sec - start.tv_sec;
  secs += (end.tv_usec - start.tv_usec) / 1000000.0;

  stats.read_time += secs;
  stats.read_calls++;
  stats.read_requested_bytes += nbyte;
  if (ret > 0)
    stats.read_actual_bytes += ret;

  errno = err;
  return ret;
}

static int
connect_tramp(int socket, const struct sockaddr *address, socklen_t address_len) {
  struct timeval start, end;
  double secs = 0;
  int err, ret;

  gettimeofday (&start, NULL);
  ret = connect(socket, address, address_len);
  err = errno;
  gettimeofday (&end, NULL);

  secs += end.tv_sec - start.tv_sec;
  secs += (end.tv_usec - start.tv_usec) / 1000000.0;

  stats.connect_time += secs;
  stats.connect_calls++;

  errno = err;
  return ret;
}

static void
fd_trace_start() {
  struct tramp_st2_entry tmp;

  tmp.addr = read_tramp;
  bin_update_image("read", &tmp, NULL);

  tmp.addr = connect_tramp;
  bin_update_image("connect", &tmp, NULL);
}

static void
fd_trace_stop() {
  // TODO: figure out how to undo the tramp
}

static void
fd_trace_reset() {
  memset(&stats, 0, sizeof(stats));
}

static void
fd_trace_dump(yajl_gen gen) {
  if (stats.read_calls > 0) {
    yajl_gen_cstr(gen, "read");
    yajl_gen_map_open(gen);
    yajl_gen_cstr(gen, "calls");
    yajl_gen_integer(gen, stats.read_calls);
    yajl_gen_cstr(gen, "time");
    yajl_gen_double(gen, stats.read_time);
    yajl_gen_cstr(gen, "requested");
    yajl_gen_integer(gen, stats.read_requested_bytes);
    yajl_gen_cstr(gen, "actual");
    yajl_gen_integer(gen, stats.read_actual_bytes);
    yajl_gen_map_close(gen);
  }

  if (stats.connect_calls > 0) {
    yajl_gen_cstr(gen, "connect");
    yajl_gen_map_open(gen);
    yajl_gen_cstr(gen, "calls");
    yajl_gen_integer(gen, stats.connect_calls);
    yajl_gen_cstr(gen, "time");
    yajl_gen_double(gen, stats.connect_time);
    yajl_gen_map_close(gen);
  }

  // fprintf(stderr, "================ FDs ======================================\n");
  // fprintf(stderr, " # read calls: %zd\n", stats.read_calls);
  // fprintf(stderr, " time in read: %fs\n", stats.read_time);
  // fprintf(stderr, " bytes: %zd requested / %zd actual\n", stats.read_requested_bytes, stats.read_actual_bytes);
  // fprintf(stderr, "===========================================================\n");
  // fprintf(stderr, " # connect calls: %zd\n", stats.connect_calls);
  // fprintf(stderr, " time in connect: %fs\n", stats.connect_time);
  // fprintf(stderr, "===========================================================\n\n");
}

void install_fd_tracer()
{
  tracer.start = fd_trace_start;
  tracer.stop = fd_trace_stop;
  tracer.reset = fd_trace_reset;
  tracer.dump = fd_trace_dump;
  tracer.id = "fd";

  trace_insert(&tracer);
}
