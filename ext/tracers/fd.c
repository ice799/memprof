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
static struct memprof_fd_stats memprof_fd_stats;

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

  memprof_fd_stats.read_time += secs;
  memprof_fd_stats.read_calls++;
  memprof_fd_stats.read_requested_bytes += nbyte;
  if (ret > 0)
    memprof_fd_stats.read_actual_bytes += ret;

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

  memprof_fd_stats.connect_time += secs;
  memprof_fd_stats.connect_calls++;

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
  memset(&memprof_fd_stats, 0, sizeof(memprof_fd_stats));
}

static void
fd_trace_dump() {
  fprintf(stderr, "================ FDs ======================================\n");
  fprintf(stderr, " # read calls: %zd\n", memprof_fd_stats.read_calls);
  fprintf(stderr, " time in read: %fs\n", memprof_fd_stats.read_time);
  fprintf(stderr, " bytes: %zd requested / %zd actual\n", memprof_fd_stats.read_requested_bytes, memprof_fd_stats.read_actual_bytes);
  fprintf(stderr, "===========================================================\n");
  fprintf(stderr, " # connect calls: %zd\n", memprof_fd_stats.connect_calls);
  fprintf(stderr, " time in connect: %fs\n", memprof_fd_stats.connect_time);
  fprintf(stderr, "===========================================================\n\n");
}

void install_fd_tracer()
{
  tracer.start = fd_trace_start;
  tracer.stop = fd_trace_stop;
  tracer.reset = fd_trace_reset;
  tracer.dump = fd_trace_dump;
  tracer.id = strdup("fd_tracer");

  trace_insert(&tracer);
}
