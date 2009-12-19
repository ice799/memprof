#define _GNU_SOURCE
#include <err.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sysexits.h>
#include <sys/mman.h>
#include <err.h>

#include <st.h>
#include <ruby.h>
#include <intern.h>
#include <node.h>

#include "arch.h"
#include "bin_api.h"

size_t pagesize;
void *text_segment = NULL;
unsigned long text_segment_len = 0;

/*
   trampoline specific stuff
 */
struct tramp_st2_entry *tramp_table = NULL;
size_t tramp_size = 0;

/*
   inline trampoline specific stuff
 */
size_t inline_tramp_size = 0;
struct inline_tramp_st2_entry *inline_tramp_table = NULL;

/*
 * bleak_house stuff
 */
static int track_objs = 0;
static st_table *objs = NULL;

struct obj_track {
  VALUE obj;
  char *source;
  int line;
};

static VALUE
newobj_tramp()
{
  VALUE ret = rb_newobj();
  struct obj_track *tracker = NULL;

  if (track_objs) {
    tracker = malloc(sizeof(*tracker));

    if (tracker) {
      if (ruby_current_node && ruby_current_node->nd_file && *ruby_current_node->nd_file) {
        tracker->source = strdup(ruby_current_node->nd_file);
        tracker->line = nd_line(ruby_current_node);
      } else if (ruby_sourcefile) {
        tracker->source = strdup(ruby_sourcefile);
        tracker->line = ruby_sourceline;
      } else {
        tracker->source = strdup("__null__");
        tracker->line = 0;
      }

      tracker->obj = ret;
      st_insert(objs, (st_data_t)ret, (st_data_t)tracker);
    } else {
      fprintf(stderr, "Warning, unable to allocate a tracker. You are running dangerously low on RAM!\n");
    }
  }

  return ret;
}

static void
freelist_tramp(unsigned long rval)
{
  struct obj_track *tracker = NULL;
  if (track_objs) {
    st_delete(objs, (st_data_t *) &rval, (st_data_t *) &tracker);
    if (tracker) {
      free(tracker->source);
      free(tracker);
    }
  }
}

static int
objs_free(st_data_t key, st_data_t record, st_data_t arg)
{
  struct obj_track *tracker = (struct obj_track *)record;
  free(tracker->source);
  free(tracker);
  return ST_DELETE;
}

static int
objs_tabulate(st_data_t key, st_data_t record, st_data_t arg)
{
  st_table *table = (st_table *)arg;
  struct obj_track *tracker = (struct obj_track *)record;
  char *source_key = NULL;
  unsigned long count = 0;
  char *type = NULL;

  switch (TYPE(tracker->obj)) {
    case T_NONE:
      type = "__none__"; break;
    case T_BLKTAG:
      type = "__blktag__"; break;
    case T_UNDEF:
      type = "__undef__"; break;
    case T_VARMAP:
      type = "__varmap__"; break;
    case T_SCOPE:
      type = "__scope__"; break;
    case T_NODE:
      type = "__node__"; break;
    default:
      if (RBASIC(tracker->obj)->klass) {
        type = (char*) rb_obj_classname(tracker->obj);
      } else {
        type = "__unknown__";
      }
  }

  asprintf(&source_key, "%s:%d:%s", tracker->source, tracker->line, type);
  st_lookup(table, (st_data_t)source_key, (st_data_t *)&count);
  if (st_insert(table, (st_data_t)source_key, ++count)) {
    free(source_key);
  }

  return ST_CONTINUE;
}

struct results {
  char **entries;
  unsigned long num_entries;
};

static int
objs_to_array(st_data_t key, st_data_t record, st_data_t arg)
{
  struct results *res = (struct results *)arg;
  unsigned long count = (unsigned long)record;
  char *source = (char *)key;
  
  asprintf(&(res->entries[res->num_entries++]), "%7d %s", count, source);

  free(source);
  return ST_DELETE;
}

static VALUE
memprof_start(VALUE self)
{
  if (track_objs == 1)
    return Qfalse;

  track_objs = 1;
  return Qtrue;
}

static VALUE
memprof_stop(VALUE self)
{
  if (track_objs == 0)
    return Qfalse;

  track_objs = 0;
  st_foreach(objs, objs_free, (st_data_t)0);
  return Qtrue;
}

static int
memprof_strcmp(const void *obj1, const void *obj2)
{
  char *str1 = *(char **)obj1;
  char *str2 = *(char **)obj2;
  return strcmp(str2, str1);
}

static VALUE
memprof_stats(int argc, VALUE *argv, VALUE self)
{
  st_table *tmp_table;
  struct results res;
  int i;
  VALUE str;
  FILE *out = NULL;

  if (!track_objs)
    rb_raise(rb_eRuntimeError, "object tracking disabled, call Memprof.start first");

  rb_scan_args(argc, argv, "01", &str);

  if (RTEST(str)) {
    out = fopen(StringValueCStr(str), "w");
    if (!out)
      rb_raise(rb_eArgError, "unable to open output file");
  }

  track_objs = 0;

  tmp_table = st_init_strtable();
  st_foreach(objs, objs_tabulate, (st_data_t)tmp_table);

  res.num_entries = 0;
  res.entries = malloc(sizeof(char*) * tmp_table->num_entries);

  st_foreach(tmp_table, objs_to_array, (st_data_t)&res);
  st_free_table(tmp_table);

  qsort(res.entries, res.num_entries, sizeof(char*), &memprof_strcmp);

  for (i=0; i < res.num_entries; i++) {
    fprintf(out ? out : stderr, "%s\n", res.entries[i]);
    free(res.entries[i]);
  }
  free(res.entries);

  track_objs = 1;
  return Qnil;
}

static VALUE
memprof_stats_bang(int argc, VALUE *argv, VALUE self)
{
  memprof_stats(argc, argv, self);
  st_foreach(objs, objs_free, (st_data_t)0);
  return Qnil;
}

static VALUE
memprof_track(int argc, VALUE *argv, VALUE self)
{
  if (!rb_block_given_p())
    rb_raise(rb_eArgError, "block required");

  memprof_start(self);
  rb_yield(Qnil);
  memprof_stats(argc, argv, self);
  memprof_stop(self);
  return Qnil;
}

static void
create_tramp_table()
{
  int i = 0;
  void *region = NULL;
   size_t tramp_sz = 0, inline_tramp_sz = 0;

  void *ent = arch_get_st2_tramp(&tramp_sz);
  void *inline_ent = arch_get_inline_st2_tramp(&inline_tramp_sz);

  if ((region = bin_allocate_page()) == MAP_FAILED) {
    fprintf(stderr, "Failed to allocate memory for stage 1 trampolines.\n");
    return;
  }

  tramp_table = region;
  inline_tramp_table = region + pagesize/2;

  for (i = 0; i < (pagesize/2)/tramp_sz; i++) {
    memcpy(tramp_table + i, ent, tramp_sz);
  }

  for (i = 0; i < (pagesize/2)/inline_tramp_sz; i++) {
    memcpy(inline_tramp_table + i, inline_ent, inline_tramp_sz);
  }
}

void
update_callqs(int entry, void *trampee, void *tramp)
{
  unsigned char *byte = text_segment;
  size_t count = 0;

  for(; count < text_segment_len; byte++, count++) {
    arch_insert_st1_tramp(byte, trampee, tramp);
  }
}

#define FREELIST_INLINES (3)

static void
hook_freelist(int entry)
{
   size_t sizes[FREELIST_INLINES], i = 0;
  void *freelist_inliners[FREELIST_INLINES];
  void *freelist = NULL;
  unsigned char *byte = NULL;

  freelist_inliners[0] = bin_find_symbol("gc_sweep", &sizes[0]);
  /* sometimes gc_sweep gets inlined in garbage_collect */
  if (!freelist_inliners[0]) {
    freelist_inliners[0] = bin_find_symbol("garbage_collect", &sizes[0]);
    if (!freelist_inliners[0]) {
      /* couldn't find garbage_collect either. */
      fprintf(stderr, "Couldn't find gc_sweep or garbage_collect!\n");
      return;
    }
  }

  freelist_inliners[1] = bin_find_symbol("finalize_list", &sizes[1]);
  if (!freelist_inliners[1]) {
    fprintf(stderr, "Couldn't find finalize_list!\n");
    /* XXX continue or exit? */
  }

  freelist_inliners[2] = bin_find_symbol("rb_gc_force_recycle", &sizes[2]);
  if (!freelist_inliners[2]) {
    fprintf(stderr, "Couldn't find rb_gc_force_recycle!\n");
    /* XXX continue or exit? */
  }

  freelist = bin_find_symbol("freelist", NULL);
  if (!freelist) {
    fprintf(stderr, "Couldn't find freelist!\n");
    return;
  }

  /* start the search for places to insert the inline tramp */

  byte = freelist_inliners[i];

  while (i < FREELIST_INLINES) {
    if (arch_insert_inline_st2_tramp(byte, freelist, freelist_tramp,
        &inline_tramp_table[entry]) == 0) {
      /* insert occurred, so increment internal counters for the tramp table */
      entry++;
      inline_tramp_size++;
    }

    /* if we've looked at all the bytes in this function... */
    if (((void *)byte - freelist_inliners[i]) >= sizes[i]) {
      /* move on to the next function */
      i++;
      byte = freelist_inliners[i];
    }
    byte++;
  }
}

static void
insert_tramp(char *trampee, void *tramp)
{
  void *trampee_addr = bin_find_symbol(trampee, NULL);
  int entry = tramp_size;
  int inline_ent = inline_tramp_size;

  if (trampee_addr == NULL) {
    if (strcmp("add_freelist", trampee) == 0) {
      /* XXX super hack */
      inline_tramp_size++;
      hook_freelist(inline_ent);
    } else {
      return;
    }
  } else {
    tramp_table[tramp_size].addr = tramp;
    bin_update_image(entry, trampee_addr, &tramp_table[tramp_size]);
    tramp_size++;
  }
}

void
Init_memprof()
{
  VALUE memprof = rb_define_module("Memprof");
  rb_define_singleton_method(memprof, "start", memprof_start, 0);
  rb_define_singleton_method(memprof, "stop", memprof_stop, 0);
  rb_define_singleton_method(memprof, "stats", memprof_stats, -1);
  rb_define_singleton_method(memprof, "stats!", memprof_stats_bang, -1);
  rb_define_singleton_method(memprof, "track", memprof_track, -1);

  pagesize = getpagesize();
  objs = st_init_numtable();
  bin_init();
  create_tramp_table();

#if defined(HAVE_MACH)
  insert_tramp("_rb_newobj", newobj_tramp);
#elif defined(HAVE_ELF)
  insert_tramp("rb_newobj", newobj_tramp);
  insert_tramp("add_freelist", freelist_tramp);
#endif

  return;
}
