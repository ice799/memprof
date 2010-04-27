#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
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

  size_t write_calls;
  double write_time;
  size_t write_requested_bytes;
  size_t write_actual_bytes;

  size_t connect_calls;
  double connect_time;

  size_t select_calls;
  double select_time;
};

static struct tracer tracer;
static struct memprof_fd_stats stats;

static ssize_t
read_tramp(int fildes, void *buf, size_t nbyte) {
  double secs = 0;
  int err;
  ssize_t ret;

  secs = trace_get_time();
  ret = read(fildes, buf, nbyte);
  err = errno;
  secs = trace_get_time() - secs;

  stats.read_time += secs;
  stats.read_calls++;
  stats.read_requested_bytes += nbyte;
  if (ret > 0)
    stats.read_actual_bytes += ret;

  errno = err;
  return ret;
}

static ssize_t
write_tramp(int fildes, const void *buf, size_t nbyte) {
  double secs = 0;
  int err;
  ssize_t ret;

  secs = trace_get_time();
  ret = write(fildes, buf, nbyte);
  err = errno;
  secs = trace_get_time() - secs;

  stats.write_time += secs;
  stats.write_calls++;
  stats.write_requested_bytes += nbyte;
  if (ret > 0)
    stats.write_actual_bytes += ret;

  errno = err;
  return ret;
}

static int
connect_tramp(int socket, const struct sockaddr *address, socklen_t address_len) {
  double secs = 0;
  int err, ret;

  secs = trace_get_time();
  ret = connect(socket, address, address_len);
  err = errno;
  secs = trace_get_time() - secs;

  stats.connect_time += secs;
  stats.connect_calls++;

  errno = err;
  return ret;
}

static int
select_tramp(int nfds, fd_set *readfds, fd_set *writefds, fd_set *errorfds, struct timeval *timeout)
{
  double secs = 0;
  int ret, err;

  secs = trace_get_time();
  ret = select(nfds, readfds, writefds, errorfds, timeout);
  err = errno;
  secs = trace_get_time() - secs;

  stats.select_time += secs;
  stats.select_calls++;

  errno = err;
  return ret;
}

static void
fd_trace_start() {
  static int inserted = 0;

  if (!inserted)
    inserted = 1;
  else
    return;

  insert_tramp("read", read_tramp);
  insert_tramp("write", write_tramp);
  insert_tramp("connect", connect_tramp);
  #ifdef HAVE_MACH
  insert_tramp("select$DARWIN_EXTSN", select_tramp);
  #else
  insert_tramp("select", select_tramp);
  #endif
}

static void
fd_trace_stop() {
}

static void
fd_trace_reset() {
  memset(&stats, 0, sizeof(stats));
}

static void
fd_trace_dump(json_gen gen) {
  if (stats.read_calls > 0) {
    json_gen_cstr(gen, "read");
    json_gen_map_open(gen);
    json_gen_cstr(gen, "calls");
    json_gen_integer(gen, stats.read_calls);
    json_gen_cstr(gen, "time");
    json_gen_double(gen, stats.read_time);
    json_gen_cstr(gen, "requested");
    json_gen_integer(gen, stats.read_requested_bytes);
    json_gen_cstr(gen, "actual");
    json_gen_integer(gen, stats.read_actual_bytes);
    json_gen_map_close(gen);
  }

  if (stats.write_calls > 0) {
    json_gen_cstr(gen, "write");
    json_gen_map_open(gen);
    json_gen_cstr(gen, "calls");
    json_gen_integer(gen, stats.write_calls);
    json_gen_cstr(gen, "time");
    json_gen_double(gen, stats.write_time);
    json_gen_cstr(gen, "requested");
    json_gen_integer(gen, stats.write_requested_bytes);
    json_gen_cstr(gen, "actual");
    json_gen_integer(gen, stats.write_actual_bytes);
    json_gen_map_close(gen);
  }

  if (stats.connect_calls > 0) {
    json_gen_cstr(gen, "connect");
    json_gen_map_open(gen);
    json_gen_cstr(gen, "calls");
    json_gen_integer(gen, stats.connect_calls);
    json_gen_cstr(gen, "time");
    json_gen_double(gen, stats.connect_time);
    json_gen_map_close(gen);
  }

  if (stats.select_calls > 0) {
    json_gen_cstr(gen, "select");
    json_gen_map_open(gen);
    json_gen_cstr(gen, "calls");
    json_gen_integer(gen, stats.select_calls);
    json_gen_cstr(gen, "time");
    json_gen_double(gen, stats.select_time);
    json_gen_map_close(gen);
  }
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
