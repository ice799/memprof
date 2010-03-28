#include <ruby.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sysexits.h>

#include <st.h>
#include <intern.h>
#include <node.h>

#include "arch.h"
#include "bin_api.h"
#include "tramp.h"
#include "util.h"

/*
 * bleak_house stuff
 */
static VALUE eUnsupported;
static int track_objs = 0;
static int memprof_started = 0;
static st_table *objs = NULL;

/*
 * stuff needed for heap dumping
 */
static VALUE (*rb_classname)(VALUE);
static double (*rb_timeofday)();
static RUBY_DATA_FUNC *rb_bm_mark;
static RUBY_DATA_FUNC *rb_blk_free;
static RUBY_DATA_FUNC *rb_thread_mark;
struct memprof_config memprof_config;

/*
 * memprof config struct init
 */
static void init_memprof_config_base();
static void init_memprof_config_extended();

struct obj_track {
  VALUE obj;
  char *source;
  int line;
  int len;
  struct timeval time[];
};

static VALUE gc_hook;
static void **ptr_to_rb_mark_table_add_filename = NULL;
static void (*rb_mark_table_add_filename)(char*);
static void (*rb_add_freelist)(VALUE);

static int
ree_sourcefile_mark_each(st_data_t key, st_data_t val, st_data_t arg)
{
  struct obj_track *tracker = (struct obj_track *)val;
  assert(tracker != NULL);

  if (tracker->source)
    rb_mark_table_add_filename(tracker->source);
  return ST_CONTINUE;
}

static int
mri_sourcefile_mark_each(st_data_t key, st_data_t val, st_data_t arg)
{
  struct obj_track *tracker = (struct obj_track *)val;
  assert(tracker != NULL);

  if (tracker->source)
    (tracker->source)[-1] = 1;
  return ST_CONTINUE;
}

/* Accomodate the different source file marking techniques of MRI and REE.
 *
 * The function pointer for REE changes depending on whether COW is enabled,
 * which can be toggled at runtime. We need to deference it to get the
 * real function every time we come here, as it may have changed.
 */

static void
sourcefile_marker()
{
  if (ptr_to_rb_mark_table_add_filename) {
    rb_mark_table_add_filename = *ptr_to_rb_mark_table_add_filename;
    assert(rb_mark_table_add_filename != NULL);
    st_foreach(objs, ree_sourcefile_mark_each, (st_data_t)NULL);
  } else {
    st_foreach(objs, mri_sourcefile_mark_each, (st_data_t)NULL);
  }
}

static VALUE
newobj_tramp()
{
  VALUE ret = rb_newobj();
  struct obj_track *tracker = NULL;

  if (track_objs && objs) {
    tracker = malloc(sizeof(*tracker) + sizeof(struct timeval));

    if (tracker) {
      if (ruby_current_node && ruby_current_node->nd_file &&
          *ruby_current_node->nd_file) {
        tracker->source = ruby_current_node->nd_file;
        tracker->line = nd_line(ruby_current_node);
      } else if (ruby_sourcefile) {
        tracker->source = ruby_sourcefile;
        tracker->line = ruby_sourceline;
      } else {
        tracker->source = NULL;
        tracker->line = 0;
      }

      tracker->obj = ret;
      tracker->len = 1;

      /* TODO a way for the user to disallow time tracking */
      if (gettimeofday(&tracker->time[0], NULL) == -1) {
        perror("gettimeofday failed. Continuing anyway, error");
      }

      rb_gc_disable();
      st_insert(objs, (st_data_t)ret, (st_data_t)tracker);
      rb_gc_enable();
    } else {
      fprintf(stderr, "Warning, unable to allocate a tracker. "
              "You are running dangerously low on RAM!\n");
    }
  }

  return ret;
}

static void
freelist_tramp(unsigned long rval)
{
  struct obj_track *tracker = NULL;

  if (rb_add_freelist) {
    rb_add_freelist(rval);
  }

  if (track_objs && objs) {
    st_delete(objs, (st_data_t *) &rval, (st_data_t *) &tracker);
    if (tracker) {
      free(tracker);
    }
  }
}

static int
objs_free(st_data_t key, st_data_t record, st_data_t arg)
{
  struct obj_track *tracker = (struct obj_track *)record;
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
  int bytes_printed = 0;

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

  bytes_printed = asprintf(&source_key, "%s:%d:%s", tracker->source ? tracker->source : "__null__", tracker->line, type);
  assert(bytes_printed != -1);
  st_lookup(table, (st_data_t)source_key, (st_data_t *)&count);
  if (st_insert(table, (st_data_t)source_key, ++count)) {
    free(source_key);
  }

  return ST_CONTINUE;
}

struct results {
  char **entries;
  size_t num_entries;
};

static int
objs_to_array(st_data_t key, st_data_t record, st_data_t arg)
{
  struct results *res = (struct results *)arg;
  unsigned long count = (unsigned long)record;
  char *source = (char *)key;
  int bytes_printed = 0;

  bytes_printed = asprintf(&(res->entries[res->num_entries++]), "%7li %s", count, source);
  assert(bytes_printed != -1);

  free(source);
  return ST_DELETE;
}

static VALUE
memprof_start(VALUE self)
{
  if (!memprof_started) {

    insert_tramp("rb_newobj", newobj_tramp);
    insert_tramp("add_freelist", freelist_tramp);
    memprof_started = 1;
  }

  if (track_objs == 1)
    return Qfalse;

  track_objs = 1;
  return Qtrue;
}

static VALUE
memprof_stop(VALUE self)
{
  /* TODO: remove trampolines and set memprof_started = 0 */

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
  size_t i;
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

  if (out)
    fclose(out);

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

struct memprof_malloc_stats {
  size_t malloc_bytes_requested;
  size_t calloc_bytes_requested;
  size_t realloc_bytes_requested;

  size_t malloc_bytes_actual;
  size_t calloc_bytes_actual;
  size_t realloc_bytes_actual;
  size_t free_bytes_actual;

  size_t malloc_calls;
  size_t calloc_calls;
  size_t realloc_calls;
  size_t free_calls;
};

static struct memprof_malloc_stats memprof_malloc_stats;
static void *orig_malloc, *orig_realloc, *orig_calloc, *orig_free;
static size_t (*malloc_usable_size)(void *ptr);

static void *
malloc_tramp(size_t size)
{
  void *ret = NULL;
  memprof_malloc_stats.malloc_bytes_requested += size;
  memprof_malloc_stats.malloc_calls++;
  ret = malloc(size);
  memprof_malloc_stats.malloc_bytes_actual += malloc_usable_size(ret);
  return ret;
}

static void *
calloc_tramp(size_t nmemb, size_t size)
{
  void *ret = NULL;
  memprof_malloc_stats.calloc_bytes_requested += (nmemb * size);
  memprof_malloc_stats.calloc_calls++;
  ret = calloc(nmemb, size);
  memprof_malloc_stats.calloc_bytes_actual += malloc_usable_size(ret);
  return ret;
}

static void *
realloc_tramp(void *ptr, size_t size)
{
  /* TODO need to check malloc_usable_size of before/after i guess? */
  void *ret = NULL;
  memprof_malloc_stats.realloc_bytes_requested += size;
  memprof_malloc_stats.realloc_calls++;
  ret = realloc(ptr, size);
  memprof_malloc_stats.realloc_bytes_actual += malloc_usable_size(ptr);
  return ret;
}

static void
free_tramp(void *ptr)
{
  /* TODO use malloc_usable_size to track bytes freed? */
  memprof_malloc_stats.free_bytes_actual += malloc_usable_size(ptr);
  memprof_malloc_stats.free_calls++;
  free(ptr);
}

static void
memprof_start_track_bytes()
{
  struct tramp_st2_entry tmp;

  if (!malloc_usable_size) {
    if ((malloc_usable_size =
          bin_find_symbol("MallocExtension_GetAllocatedSize", NULL, 1)) == NULL) {
      malloc_usable_size = bin_find_symbol("malloc_usable_size", NULL, 1);
      dbg_printf("tcmalloc was not found...\n");
    }
    assert(malloc_usable_size != NULL);
    dbg_printf("malloc_usable_size: %p\n", malloc_usable_size);
  }

  tmp.addr = malloc_tramp;
  bin_update_image("malloc", &tmp, &orig_malloc);
  assert(orig_malloc != NULL);
  dbg_printf("orig_malloc: %p\n", orig_malloc);

  tmp.addr = realloc_tramp;
  bin_update_image("realloc", &tmp, &orig_realloc);
  dbg_printf("orig_realloc: %p\n", orig_realloc);

  tmp.addr = calloc_tramp;
  bin_update_image("calloc", &tmp, &orig_calloc);
  dbg_printf("orig_calloc: %p\n", orig_calloc);

  tmp.addr = free_tramp;
  bin_update_image("free", &tmp, &orig_free);
  assert(orig_free != NULL);
  dbg_printf("orig_free: %p\n", orig_free);
}

static void
memprof_stop_track_bytes()
{
  struct tramp_st2_entry tmp;

  tmp.addr = orig_malloc;
  bin_update_image("malloc", &tmp, NULL);

  tmp.addr = orig_realloc;
  bin_update_image("realloc", &tmp, NULL);

  tmp.addr = orig_calloc;
  bin_update_image("calloc", &tmp, NULL);

  tmp.addr = orig_free;
  bin_update_image("free", &tmp, NULL);
}

static void
memprof_reset_track_bytes()
{
  memset(&memprof_malloc_stats, 0, sizeof(memprof_malloc_stats));
}

static VALUE
memprof_track_bytes(int argc, VALUE *argv, VALUE self)
{
  if (!rb_block_given_p())
    rb_raise(rb_eArgError, "block required");

  memprof_start_track_bytes();
  rb_yield(Qnil);
  fprintf(stderr, "================ Requested ====================\n");
  fprintf(stderr, "Malloced: %zd, Realloced: %zd, Calloced: %zd\n",
      memprof_malloc_stats.malloc_bytes_requested, memprof_malloc_stats.realloc_bytes_requested,
      memprof_malloc_stats.calloc_bytes_requested);
  fprintf(stderr, "================ Actual ====================\n");
  fprintf(stderr, "Malloced: %zd, Realloced: %zd, Calloced: %zd, Freed: %zd\n",
      memprof_malloc_stats.malloc_bytes_actual, memprof_malloc_stats.realloc_bytes_actual,
      memprof_malloc_stats.calloc_bytes_actual, memprof_malloc_stats.free_bytes_actual);
  fprintf(stderr, "================ Call count ====================\n");
  fprintf(stderr, "Calls to malloc: %zd, realloc: %zd, calloc: %zd, free: %zd\n",
      memprof_malloc_stats.malloc_calls,
      memprof_malloc_stats.realloc_calls,
      memprof_malloc_stats.calloc_calls,
      memprof_malloc_stats.free_calls);

  memprof_reset_track_bytes();
  memprof_stop_track_bytes();
  return Qnil;
}

#include <yajl/yajl_gen.h>
#include <stdarg.h>
#include "env.h"
#include "rubyio.h"
#include "re.h"

#ifndef RARRAY_PTR
#define RARRAY_PTR(ary) RARRAY(ary)->ptr
#endif

#ifndef RARRAY_LEN
#define RARRAY_LEN(ary) RARRAY(ary)->len
#endif

#ifndef RSTRING_PTR
#define RSTRING_PTR(str) RSTRING(str)->ptr
#endif

#ifndef RSTRING_LEN
#define RSTRING_LEN(str) RSTRING(str)->len
#endif

/* HAX: copied from internal yajl_gen.c (PATCH yajl before building instead)
 */

typedef enum {
    yajl_gen_start,
    yajl_gen_map_start,
    yajl_gen_map_key,
    yajl_gen_map_val,
    yajl_gen_array_start,
    yajl_gen_in_array,
    yajl_gen_complete,
    yajl_gen_error
} yajl_gen_state;

struct yajl_gen_t
{
    unsigned int depth;
    unsigned int pretty;
    const char * indentString;
    yajl_gen_state state[YAJL_MAX_DEPTH];
    yajl_print_t print;
    void * ctx; /* yajl_buf */
    /* memory allocation routines */
    yajl_alloc_funcs alloc;
};

static void
yajl_gen_reset(yajl_gen gen)
{
  yajl_gen_clear(gen);
  assert (gen->state[gen->depth] == yajl_gen_complete);
  gen->state[gen->depth] = yajl_gen_start;
  gen->print(gen->ctx, "\n", 1);
}

/* END HAX
 */

static yajl_gen_status
yajl_gen_cstr(yajl_gen gen, const char * str)
{
  if (!str || str[0] == 0)
    return yajl_gen_null(gen);
  else
    return yajl_gen_string(gen, (unsigned char *)str, strlen(str));
}

static yajl_gen_status
yajl_gen_format(yajl_gen gen, char *format, ...)
{
  va_list args;
  char *str;
  int bytes_printed = 0;

  yajl_gen_status ret;

  va_start(args, format);
  bytes_printed = vasprintf(&str, format, args);
  assert(bytes_printed != -1);
  va_end(args);

  ret = yajl_gen_string(gen, (unsigned char *)str, strlen(str));
  free(str);
  return ret;
}

static yajl_gen_status
yajl_gen_id(yajl_gen gen, ID id)
{
  if (id) {
    if (id < 100)
      return yajl_gen_format(gen, ":%c", id);
    else
      return yajl_gen_format(gen, ":%s", rb_id2name(id));
  } else
    return yajl_gen_null(gen);
}

static yajl_gen_status
yajl_gen_pointer(yajl_gen gen, void* ptr)
{
  return yajl_gen_format(gen, "0x%x", ptr);
}

static yajl_gen_status
yajl_gen_value(yajl_gen gen, VALUE obj)
{
  if (FIXNUM_P(obj))
    return yajl_gen_integer(gen, NUM2LONG(obj));
  else if (NIL_P(obj) || obj == Qundef)
    return yajl_gen_null(gen);
  else if (obj == Qtrue)
    return yajl_gen_bool(gen, 1);
  else if (obj == Qfalse)
    return yajl_gen_bool(gen, 0);
  else if (SYMBOL_P(obj))
    return yajl_gen_id(gen, SYM2ID(obj));
  else
    return yajl_gen_pointer(gen, (void*)obj);
}

static int
each_hash_entry(st_data_t key, st_data_t record, st_data_t arg)
{
  yajl_gen gen = (yajl_gen)arg;
  VALUE k = (VALUE)key;
  VALUE v = (VALUE)record;

  yajl_gen_array_open(gen);
  yajl_gen_value(gen, k);
  yajl_gen_value(gen, v);
  yajl_gen_array_close(gen);

  return ST_CONTINUE;
}

static int
each_ivar(st_data_t key, st_data_t record, st_data_t arg)
{
  yajl_gen gen = (yajl_gen)arg;
  ID id = (ID)key;
  VALUE val = (VALUE)record;
  const char *name = rb_id2name(id);

  yajl_gen_cstr(gen, name ? name : "(none)");
  yajl_gen_value(gen, val);

  return ST_CONTINUE;
}

static char *
nd_type_str(VALUE obj)
{
  switch(nd_type(obj)) {
    #define ND(type) case NODE_##type: return #type;
    ND(METHOD);     ND(FBODY);      ND(CFUNC);    ND(SCOPE);
    ND(BLOCK);      ND(IF);         ND(CASE);     ND(WHEN);
    ND(OPT_N);      ND(WHILE);      ND(UNTIL);    ND(ITER);
    ND(FOR);        ND(BREAK);      ND(NEXT);     ND(REDO);
    ND(RETRY);      ND(BEGIN);      ND(RESCUE);   ND(RESBODY);
    ND(ENSURE);     ND(AND);        ND(OR);       ND(NOT);
    ND(MASGN);      ND(LASGN);      ND(DASGN);    ND(DASGN_CURR);
    ND(GASGN);      ND(IASGN);      ND(CDECL);    ND(CVASGN);
    ND(CVDECL);     ND(OP_ASGN1);   ND(OP_ASGN2); ND(OP_ASGN_AND);
    ND(OP_ASGN_OR); ND(CALL);       ND(FCALL);    ND(VCALL);
    ND(SUPER);      ND(ZSUPER);     ND(ARRAY);    ND(ZARRAY);
    ND(HASH);       ND(RETURN);     ND(YIELD);    ND(LVAR);
    ND(DVAR);       ND(GVAR);       ND(IVAR);     ND(CONST);
    ND(CVAR);       ND(NTH_REF);    ND(BACK_REF); ND(MATCH);
    ND(MATCH2);     ND(MATCH3);     ND(LIT);      ND(STR);
    ND(DSTR);       ND(XSTR);       ND(DXSTR);    ND(EVSTR);
    ND(DREGX);      ND(DREGX_ONCE); ND(ARGS);     ND(ARGSCAT);
    ND(ARGSPUSH);   ND(SPLAT);      ND(TO_ARY);   ND(SVALUE);
    ND(BLOCK_ARG);  ND(BLOCK_PASS); ND(DEFN);     ND(DEFS);
    ND(ALIAS);      ND(VALIAS);     ND(UNDEF);    ND(CLASS);
    ND(MODULE);     ND(SCLASS);     ND(COLON2);   ND(COLON3)
    ND(CREF);       ND(DOT2);       ND(DOT3);     ND(FLIP2);
    ND(FLIP3);      ND(ATTRSET);    ND(SELF);     ND(NIL);
    ND(TRUE);       ND(FALSE);      ND(DEFINED);  ND(NEWLINE);
    ND(POSTEXE);    ND(ALLOCA);     ND(DMETHOD);  ND(BMETHOD);
    ND(MEMO);       ND(IFUNC);      ND(DSYM);     ND(ATTRASGN);
    ND(LAST);
    default:
      return "unknown";
  }
}

static inline void
obj_dump_class(yajl_gen gen, VALUE obj)
{
  if (RBASIC(obj)->klass) {
    yajl_gen_cstr(gen, "class");
    yajl_gen_value(gen, RBASIC(obj)->klass);

    VALUE name = rb_classname(RBASIC(obj)->klass);
    if (RTEST(name)) {
      yajl_gen_cstr(gen, "class_name");
      yajl_gen_cstr(gen, RSTRING_PTR(name));
    }
  }
}

/* TODO
 *  look for FL_EXIVAR flag and print ivars
 *  print more detail about Proc/struct BLOCK in T_DATA if freefunc == blk_free
 *  add Memprof.dump_all for full heap dump
 *  print details on different types of nodes (nd_next, nd_lit, nd_nth, etc)
 */

static void
obj_dump(VALUE obj, yajl_gen gen)
{
  int type;
  yajl_gen_map_open(gen);

  yajl_gen_cstr(gen, "_id");
  yajl_gen_value(gen, obj);

  struct obj_track *tracker = NULL;
  if (st_lookup(objs, (st_data_t)obj, (st_data_t *)&tracker) && BUILTIN_TYPE(obj) != T_NODE) {
    yajl_gen_cstr(gen, "file");
    yajl_gen_cstr(gen, tracker->source);
    yajl_gen_cstr(gen, "line");
    yajl_gen_integer(gen, tracker->line);
    yajl_gen_cstr(gen, "time");
    yajl_gen_integer(gen, (tracker->time[0].tv_sec * 1000000) + tracker->time[0].tv_usec);
  }

  yajl_gen_cstr(gen, "type");
  switch (type=BUILTIN_TYPE(obj)) {
    case T_DATA:
      yajl_gen_cstr(gen, "data");
      obj_dump_class(gen, obj);

      if (DATA_PTR(obj)) {
        yajl_gen_cstr(gen, "data");
        yajl_gen_pointer(gen, DATA_PTR(obj));
      }

      if (RDATA(obj)->dfree == (RUBY_DATA_FUNC)rb_blk_free) {
        void *val;
        VALUE ptr;

        val = *(void**)(DATA_PTR(obj) + memprof_config.offset_BLOCK_body);
        if (val) {
          yajl_gen_cstr(gen, "nd_body");
          yajl_gen_pointer(gen, val);
        }

        val = *(void**)(DATA_PTR(obj) + memprof_config.offset_BLOCK_var);
        if (val) {
          yajl_gen_cstr(gen, "nd_var");
          yajl_gen_pointer(gen, val);
        }

        val = *(void**)(DATA_PTR(obj) + memprof_config.offset_BLOCK_cref);
        if (val) {
          yajl_gen_cstr(gen, "nd_cref");
          yajl_gen_pointer(gen, val);
        }

        val = *(void**)(DATA_PTR(obj) + memprof_config.offset_BLOCK_dyna_vars);
        if (val) {
          yajl_gen_cstr(gen, "vars");
          yajl_gen_pointer(gen, val);
        }

        val = *(void**)(DATA_PTR(obj) + memprof_config.offset_BLOCK_scope);
        if (val) {
          yajl_gen_cstr(gen, "scope");
          yajl_gen_pointer(gen, val);
        }

        ptr = *(VALUE*)(DATA_PTR(obj) + memprof_config.offset_BLOCK_self);
        yajl_gen_cstr(gen, "self");
        yajl_gen_value(gen, ptr);

        ptr = *(VALUE*)(DATA_PTR(obj) + memprof_config.offset_BLOCK_klass);
        yajl_gen_cstr(gen, "klass");
        yajl_gen_value(gen, ptr);

        ptr = *(VALUE*)(DATA_PTR(obj) + memprof_config.offset_BLOCK_orig_thread);
        yajl_gen_cstr(gen, "thread");
        yajl_gen_value(gen, ptr);

        ptr = *(VALUE*)(DATA_PTR(obj) + memprof_config.offset_BLOCK_wrapper);
        if (RTEST(ptr)) {
          yajl_gen_cstr(gen, "wrapper");
          yajl_gen_value(gen, ptr);
        }

        ptr = *(VALUE*)(DATA_PTR(obj) + memprof_config.offset_BLOCK_block_obj);
        if (RTEST(ptr)) {
          yajl_gen_cstr(gen, "block");
          yajl_gen_value(gen, ptr);
        }

        /* TODO: is .prev actually useful? refers to non-heap allocated struct BLOCKs,
         * but we don't print out any information about those
         */
        /*
        yajl_gen_cstr(gen, "prev");
        yajl_gen_array_open(gen);
        val = *(void**)(DATA_PTR(obj) + memprof_config.offset_BLOCK_prev);
        while (val) {
          yajl_gen_pointer(gen, val);
          prev = val;
          val = *(void**)(ptr + memprof_config.offset_BLOCK_prev);
          if (prev == val)
            break;
        }
        yajl_gen_array_close(gen);
        */

      } else if (RDATA(obj)->dmark == (RUBY_DATA_FUNC)rb_bm_mark) {
        VALUE ptr;
        ID id, mid;

        ptr = *(VALUE*)(DATA_PTR(obj) + memprof_config.offset_METHOD_klass);
        if (RTEST(ptr)) {
          yajl_gen_cstr(gen, "klass");
          yajl_gen_value(gen, ptr);
        }

        ptr = *(VALUE*)(DATA_PTR(obj) + memprof_config.offset_METHOD_rklass);
        if (RTEST(ptr)) {
          yajl_gen_cstr(gen, "rklass");
          yajl_gen_value(gen, ptr);
        }

        ptr = *(VALUE*)(DATA_PTR(obj) + memprof_config.offset_METHOD_recv);
        if (RTEST(ptr)) {
          yajl_gen_cstr(gen, "recv");
          yajl_gen_value(gen, ptr);
        }

        ptr = *(VALUE*)(DATA_PTR(obj) + memprof_config.offset_METHOD_body);
        if (RTEST(ptr)) {
          yajl_gen_cstr(gen, "node");
          yajl_gen_value(gen, ptr);
        }

        mid = *(ID*)(DATA_PTR(obj) + memprof_config.offset_METHOD_id);
        if (mid) {
          yajl_gen_cstr(gen, "mid");
          yajl_gen_id(gen, mid);
        }

        id = *(ID*)(DATA_PTR(obj) + memprof_config.offset_METHOD_oid);
        if (id && id != mid) {
          yajl_gen_cstr(gen, "oid");
          yajl_gen_id(gen, id);
        }

      } else if (RDATA(obj)->dmark == (RUBY_DATA_FUNC)rb_thread_mark) {
        rb_thread_t th = (rb_thread_t)DATA_PTR(obj);

        if (th == rb_curr_thread) {
          yajl_gen_cstr(gen, "current");
          yajl_gen_bool(gen, 1);
        } else {
          if (th->dyna_vars) {
            yajl_gen_cstr(gen, "varmap");
            yajl_gen_pointer(gen, th->dyna_vars);
          }

          yajl_gen_cstr(gen, "node");
          yajl_gen_pointer(gen, th->node);

          yajl_gen_cstr(gen, "cref");
          yajl_gen_pointer(gen, th->cref);

          char *status;
          switch (th->status) {
            case THREAD_TO_KILL:
              status = "to_kill";
              break;
            case THREAD_RUNNABLE:
              status = "runnable";
              break;
            case THREAD_STOPPED:
              status = "stopped";
              break;
            case THREAD_KILLED:
              status = "killed";
              break;
            default:
              status = "unknown";
          }

          yajl_gen_cstr(gen, "status");
          yajl_gen_cstr(gen, status);

          #define WAIT_FD		(1<<0)
          #define WAIT_SELECT	(1<<1)
          #define WAIT_TIME	(1<<2)
          #define WAIT_JOIN	(1<<3)
          #define WAIT_PID	(1<<4)

          yajl_gen_cstr(gen, "wait_for");
          yajl_gen_array_open(gen);
          if (th->wait_for & WAIT_FD)
            yajl_gen_cstr(gen, "fd");
          if (th->wait_for & WAIT_SELECT)
            yajl_gen_cstr(gen, "select");
          if (th->wait_for & WAIT_TIME)
            yajl_gen_cstr(gen, "time");
          if (th->wait_for & WAIT_JOIN)
            yajl_gen_cstr(gen, "join");
          if (th->wait_for & WAIT_PID)
            yajl_gen_cstr(gen, "pid");
          yajl_gen_array_close(gen);

          if (th->wait_for & WAIT_FD) {
            yajl_gen_cstr(gen, "fd");
            yajl_gen_integer(gen, th->fd);
          }

          #define DELAY_INFTY 1E30

          if (th->wait_for & WAIT_TIME) {
            yajl_gen_cstr(gen, "delay");
            if (th->delay == DELAY_INFTY)
              yajl_gen_cstr(gen, "infinity");
            else
              yajl_gen_double(gen, th->delay - rb_timeofday());
          }

          if (th->wait_for & WAIT_JOIN) {
            yajl_gen_cstr(gen, "join");
            yajl_gen_value(gen, th->join->thread);
          }
        }

        yajl_gen_cstr(gen, "priority");
        yajl_gen_integer(gen, th->priority);

        if (th == rb_main_thread) {
          yajl_gen_cstr(gen, "main");
          yajl_gen_bool(gen, 1);
        }

        if (th->next && th->next != rb_main_thread) {
          yajl_gen_cstr(gen, "next");
          yajl_gen_value(gen, th->next->thread);
        }
        if (th->prev && th->prev != th && (th->prev == rb_main_thread || th->prev != th->next)) {
          yajl_gen_cstr(gen, "prev");
          yajl_gen_value(gen, th->prev->thread);
        }

        if (th->locals) {
          yajl_gen_cstr(gen, "variables");
          yajl_gen_map_open(gen);
          st_foreach(th->locals, each_ivar, (st_data_t)gen);
          yajl_gen_map_close(gen);
        }

      }
      break;

    case T_STRUCT:
      yajl_gen_cstr(gen, "struct");
      obj_dump_class(gen, obj);
      break;

    case T_FILE:
      yajl_gen_cstr(gen, "file");
      obj_dump_class(gen, obj);

      OpenFile *file = RFILE(obj)->fptr;

      if (file->f) {
        yajl_gen_cstr(gen, "fileno");
        yajl_gen_integer(gen, fileno(file->f));
      }

      if (file->f2) {
        yajl_gen_cstr(gen, "fileno2");
        yajl_gen_integer(gen, fileno(file->f2));
      }

      if (file->pid) {
        yajl_gen_cstr(gen, "pid");
        yajl_gen_integer(gen, file->pid);
      }

      if (file->path) {
        yajl_gen_cstr(gen, "path");
        yajl_gen_cstr(gen, file->path);
      }

      if (file->mode) {
        yajl_gen_cstr(gen, "mode");
        yajl_gen_array_open(gen);
        if (file->mode & FMODE_READABLE)
          yajl_gen_cstr(gen, "readable");
        if (file->mode & FMODE_WRITABLE)
          yajl_gen_cstr(gen, "writable");
        if (file->mode & FMODE_READWRITE)
          yajl_gen_cstr(gen, "readwrite");
        if (file->mode & FMODE_APPEND)
          yajl_gen_cstr(gen, "append");
        if (file->mode & FMODE_CREATE)
          yajl_gen_cstr(gen, "create");
        if (file->mode & FMODE_BINMODE)
          yajl_gen_cstr(gen, "binmode");
        if (file->mode & FMODE_SYNC)
          yajl_gen_cstr(gen, "sync");
        if (file->mode & FMODE_WBUF)
          yajl_gen_cstr(gen, "wbuf");
        if (file->mode & FMODE_RBUF)
          yajl_gen_cstr(gen, "rbuf");
        if (file->mode & FMODE_WSPLIT)
          yajl_gen_cstr(gen, "wsplit");
        if (file->mode & FMODE_WSPLIT_INITIALIZED)
          yajl_gen_cstr(gen, "wsplit_initialized");
        yajl_gen_array_close(gen);
      }

      break;

    case T_FLOAT:
      yajl_gen_cstr(gen, "float");
      obj_dump_class(gen, obj);

      yajl_gen_cstr(gen, "data");
      yajl_gen_double(gen, RFLOAT(obj)->value);
      break;

    case T_BIGNUM:
      yajl_gen_cstr(gen, "bignum");
      obj_dump_class(gen, obj);

      yajl_gen_cstr(gen, "negative");
      yajl_gen_bool(gen, RBIGNUM(obj)->sign == 0);

      yajl_gen_cstr(gen, "length");
      yajl_gen_integer(gen, RBIGNUM(obj)->len);

      yajl_gen_cstr(gen, "data");
      yajl_gen_string(gen, RBIGNUM(obj)->digits, RBIGNUM(obj)->len);
      break;

    case T_MATCH:
      yajl_gen_cstr(gen, "match");
      obj_dump_class(gen, obj);

      yajl_gen_cstr(gen, "data");
      yajl_gen_value(gen, RMATCH(obj)->str);
      break;

    case T_REGEXP:
      yajl_gen_cstr(gen, "regexp");
      obj_dump_class(gen, obj);

      yajl_gen_cstr(gen, "length");
      yajl_gen_integer(gen, RREGEXP(obj)->len);

      yajl_gen_cstr(gen, "data");
      yajl_gen_cstr(gen, RREGEXP(obj)->str);
      break;

    case T_SCOPE:
      yajl_gen_cstr(gen, "scope");
      obj_dump_class(gen, obj);

      struct SCOPE *scope = (struct SCOPE *)obj;
      if (scope->local_tbl) {
        int i = 0;
        int n = scope->local_tbl[0];
        VALUE *list = &scope->local_vars[-1];
        VALUE cur = *list++;

        if (RTEST(cur)) {
          yajl_gen_cstr(gen, "node");
          yajl_gen_value(gen, cur);
        }

        if (n) {
          yajl_gen_cstr(gen, "variables");
          yajl_gen_map_open(gen);
          while (n--) {
            cur = *list++;
            i++;

            if (!rb_is_local_id(scope->local_tbl[i]))
              continue;

            yajl_gen_id(gen, scope->local_tbl[i]);
            yajl_gen_value(gen, cur);
          }
          yajl_gen_map_close(gen);
        }
      }
      break;

    case T_NODE:
      yajl_gen_cstr(gen, "node");

      yajl_gen_cstr(gen, "node_type");
      yajl_gen_cstr(gen, nd_type_str(obj));

      yajl_gen_cstr(gen, "file");
      yajl_gen_cstr(gen, RNODE(obj)->nd_file);

      yajl_gen_cstr(gen, "line");
      yajl_gen_integer(gen, nd_line(obj));

      yajl_gen_cstr(gen, "node_code");
      yajl_gen_integer(gen, nd_type(obj));

      #define PRINT_ID(sub) yajl_gen_id(gen, RNODE(obj)->sub.id)
      #define PRINT_VAL(sub) yajl_gen_value(gen, RNODE(obj)->sub.value)

      int nd_type = nd_type(obj);
      yajl_gen_cstr(gen, "n1");
      switch(nd_type) {
        case NODE_LVAR:
        case NODE_DVAR:
        case NODE_IVAR:
        case NODE_CVAR:
        case NODE_GVAR:
        case NODE_CONST:
        case NODE_ATTRSET:
        case NODE_LASGN:
        case NODE_IASGN:
        case NODE_DASGN:
        case NODE_CVASGN:
        case NODE_CVDECL:
        case NODE_GASGN:
        case NODE_DASGN_CURR:
        case NODE_BLOCK_ARG:
        case NODE_CDECL:
        case NODE_VALIAS:
          PRINT_ID(u1);
          break;

        case NODE_OP_ASGN2:
          if (RNODE(obj)->u3.id > 1000000)
            PRINT_VAL(u1);
          else
            PRINT_ID(u1);
          break;

        case NODE_SCOPE: {
          ID *tbl = RNODE(obj)->nd_tbl;
          yajl_gen_array_open(gen);
          if (tbl) {
            int size = tbl[0];
            int i = 3;

            for (; i < size+1; i++) {
              yajl_gen_id(gen, tbl[i]);
            }
          }
          yajl_gen_array_close(gen);
          break;
        }

        case NODE_IFUNC:
        case NODE_CFUNC: {
          const char *name = bin_find_symbol_name((void*)RNODE(obj)->u1.value);
          yajl_gen_format(gen, "0x%x: %s", RNODE(obj)->u1.value, name ? name : "???");
          break;
        }

        default:
          PRINT_VAL(u1);
      }

      yajl_gen_cstr(gen, "n2");
      switch(nd_type) {
        case NODE_CALL:
        case NODE_FBODY:
        case NODE_DEFN:
        case NODE_ATTRASGN:
        case NODE_FCALL:
        case NODE_VCALL:
        case NODE_COLON2:
        case NODE_COLON3:
        case NODE_BACK_REF:
        case NODE_DEFS:
        case NODE_VALIAS:
          PRINT_ID(u2);
          break;

        case NODE_OP_ASGN1:
          if (RNODE(obj)->nd_mid == 0)
            yajl_gen_cstr(gen, ":||");
          else if (RNODE(obj)->nd_mid == 1)
            yajl_gen_cstr(gen, ":&&");
          else
            PRINT_ID(u2);
          break;

        case NODE_OP_ASGN2:
          if (RNODE(obj)->u3.id > 1000000) {
            PRINT_VAL(u2);
          } else {
            if (RNODE(obj)->nd_mid == 0)
              yajl_gen_cstr(gen, ":||");
            else if (RNODE(obj)->nd_mid == 1)
              yajl_gen_cstr(gen, ":&&");
            else
              PRINT_ID(u2);
          }
          break;

        case NODE_DREGX:
        case NODE_DREGX_ONCE:
        case NODE_NTH_REF:
        case NODE_IFUNC:
        case NODE_CFUNC:
        case NODE_NEWLINE:
          yajl_gen_integer(gen, RNODE(obj)->u2.argc);
          break;

        case NODE_BLOCK:
        case NODE_ARRAY:
          if (RNODE(obj)->u2.node == RNODE(obj))
            yajl_gen_null(gen);
          else
            PRINT_VAL(u2);
          break;

        default:
          PRINT_VAL(u2);
      }

      yajl_gen_cstr(gen, "n3");
      switch(nd_type) {
        case NODE_ARGS:
          yajl_gen_integer(gen, RNODE(obj)->u3.cnt);
          break;

        case NODE_OP_ASGN2:
          if (RNODE(obj)->u3.id > 1000000)
            PRINT_VAL(u3);
          else
            PRINT_ID(u3);
          break;

        default:
          PRINT_VAL(u3);
      }
      break;

    case T_STRING:
      yajl_gen_cstr(gen, "string");
      obj_dump_class(gen, obj);

      yajl_gen_cstr(gen, "length");
      yajl_gen_integer(gen, RSTRING_LEN(obj));

      if (FL_TEST(obj, ELTS_SHARED|FL_USER3)) {
        yajl_gen_cstr(gen, "shared");
        yajl_gen_value(gen, RSTRING(obj)->aux.shared);

        yajl_gen_cstr(gen, "flags");
        yajl_gen_array_open(gen);
        if (FL_TEST(obj, ELTS_SHARED))
          yajl_gen_cstr(gen, "elts_shared");
        if (FL_TEST(obj, FL_USER3))
          yajl_gen_cstr(gen, "str_assoc");
        yajl_gen_array_close(gen);
      } else {
        yajl_gen_cstr(gen, "data");
        yajl_gen_string(gen, (unsigned char *)RSTRING_PTR(obj), RSTRING_LEN(obj));
      }
      break;

    case T_VARMAP:
      yajl_gen_cstr(gen, "varmap");
      obj_dump_class(gen, obj);

      struct RVarmap *vars = (struct RVarmap *)obj;

      if (vars->next) {
        yajl_gen_cstr(gen, "next");
        yajl_gen_value(gen, (VALUE)vars->next);
      }

      if (vars->id) {
        yajl_gen_cstr(gen, "data");
        yajl_gen_map_open(gen);
        yajl_gen_id(gen, vars->id);
        yajl_gen_value(gen, vars->val);
        yajl_gen_map_close(gen);
      }
      break;

    case T_CLASS:
    case T_MODULE:
    case T_ICLASS:
      yajl_gen_cstr(gen, type==T_CLASS ? "class" : type==T_MODULE ? "module" : "iclass");
      obj_dump_class(gen, obj);

      yajl_gen_cstr(gen, "name");
      VALUE name = rb_classname(obj);
      if (RTEST(name))
        yajl_gen_cstr(gen, RSTRING_PTR(name));
      else
        yajl_gen_cstr(gen, 0);

      yajl_gen_cstr(gen, "super");
      yajl_gen_value(gen, RCLASS(obj)->super);

      if (RTEST(RCLASS(obj)->super)) {
        yajl_gen_cstr(gen, "super_name");
        VALUE super_name = rb_classname(RCLASS(obj)->super);
        if (RTEST(super_name))
          yajl_gen_cstr(gen, RSTRING_PTR(super_name));
        else
          yajl_gen_cstr(gen, 0);
      }

      if (FL_TEST(obj, FL_SINGLETON)) {
        yajl_gen_cstr(gen, "singleton");
        yajl_gen_bool(gen, 1);
      }

      if (RCLASS(obj)->iv_tbl && RCLASS(obj)->iv_tbl->num_entries) {
        yajl_gen_cstr(gen, "ivars");
        yajl_gen_map_open(gen);
        st_foreach(RCLASS(obj)->iv_tbl, each_ivar, (st_data_t)gen);
        yajl_gen_map_close(gen);
      }

      if (RCLASS(obj)->m_tbl && RCLASS(obj)->m_tbl->num_entries) {
        yajl_gen_cstr(gen, "methods");
        yajl_gen_map_open(gen);
        st_foreach(RCLASS(obj)->m_tbl, each_ivar, (st_data_t)gen);
        yajl_gen_map_close(gen);
      }
      break;

    case T_OBJECT:
      yajl_gen_cstr(gen, "object");
      obj_dump_class(gen, obj);

      struct RClass *klass = RCLASS(obj);

      if (klass->iv_tbl && klass->iv_tbl->num_entries) {
        yajl_gen_cstr(gen, "ivars");
        yajl_gen_map_open(gen);
        st_foreach(klass->iv_tbl, each_ivar, (st_data_t)gen);
        yajl_gen_map_close(gen);
      }
      break;

    case T_ARRAY:
      yajl_gen_cstr(gen, "array");
      obj_dump_class(gen, obj);

      struct RArray *ary = RARRAY(obj);

      yajl_gen_cstr(gen, "length");
      yajl_gen_integer(gen, ary->len);

      if (FL_TEST(obj, ELTS_SHARED)) {
        yajl_gen_cstr(gen, "shared");
        yajl_gen_value(gen, ary->aux.shared);
      } else if (ary->len) {
        yajl_gen_cstr(gen, "data");
        yajl_gen_array_open(gen);
        int i;
        for(i=0; i < ary->len; i++)
          yajl_gen_value(gen, ary->ptr[i]);
        yajl_gen_array_close(gen);
      }
      break;

    case T_HASH:
      yajl_gen_cstr(gen, "hash");
      obj_dump_class(gen, obj);

      struct RHash *hash = RHASH(obj);

      yajl_gen_cstr(gen, "length");
      if (hash->tbl)
        yajl_gen_integer(gen, hash->tbl->num_entries);
      else
        yajl_gen_integer(gen, 0);

      yajl_gen_cstr(gen, "default");
      yajl_gen_value(gen, hash->ifnone);

      if (hash->tbl && hash->tbl->num_entries) {
        yajl_gen_cstr(gen, "data");
        //yajl_gen_map_open(gen);
        yajl_gen_array_open(gen);
        st_foreach(hash->tbl, each_hash_entry, (st_data_t)gen);
        yajl_gen_array_close(gen);
        //yajl_gen_map_close(gen);
      }
      break;

    default:
      yajl_gen_cstr(gen, "unknown");
      obj_dump_class(gen, obj);
  }

  yajl_gen_cstr(gen, "code");
  yajl_gen_integer(gen, BUILTIN_TYPE(obj));

  yajl_gen_map_close(gen);
}

static int
objs_each_dump(st_data_t key, st_data_t record, st_data_t arg)
{
  obj_dump((VALUE)key, (yajl_gen)arg);
  return ST_CONTINUE;
}

extern st_table *rb_global_tbl;

static int
globals_each_dump(st_data_t key, st_data_t record, st_data_t arg)
{
  yajl_gen_id((yajl_gen)arg, (ID)key);
  yajl_gen_value((yajl_gen)arg, rb_gvar_get((void*)record));
  return ST_CONTINUE;
}

static void
memprof_dump_globals(yajl_gen gen)
{
  yajl_gen_map_open(gen);

  yajl_gen_cstr(gen, "_id");
  yajl_gen_cstr(gen, "globals");

  yajl_gen_cstr(gen, "type");
  yajl_gen_cstr(gen, "globals");

  yajl_gen_cstr(gen, "variables");

  yajl_gen_map_open(gen);
  st_foreach(rb_global_tbl, globals_each_dump, (st_data_t)gen);
  yajl_gen_map_close(gen);

  yajl_gen_map_close(gen);
  yajl_gen_reset(gen);
}

static void
memprof_dump_stack_frame(yajl_gen gen, struct FRAME *frame)
{
  yajl_gen_map_open(gen);

  yajl_gen_cstr(gen, "_id");
  yajl_gen_pointer(gen, frame);

  yajl_gen_cstr(gen, "type");
  yajl_gen_cstr(gen, "frame");

  yajl_gen_cstr(gen, "self");
  yajl_gen_value(gen, frame->self);

  if (frame->last_class) {
    yajl_gen_cstr(gen, "last_class");
    yajl_gen_value(gen, frame->last_class);
  }

  if (frame->orig_func) {
    yajl_gen_cstr(gen, "orig_func");
    yajl_gen_id(gen, frame->orig_func);
  }

  if (frame->last_func && frame->last_func != frame->orig_func) {
    yajl_gen_cstr(gen, "last_func");
    yajl_gen_id(gen, frame->last_func);
  }

  if (frame->node) {
    yajl_gen_cstr(gen, "node");
    yajl_gen_pointer(gen, (void*)frame->node);
  }

  if (frame->prev) {
    yajl_gen_cstr(gen, "prev");
    yajl_gen_pointer(gen, (void*)frame->prev);
  }

  if (frame->tmp) {
    yajl_gen_cstr(gen, "tmp");
    yajl_gen_pointer(gen, (void*)frame->tmp);
  }

  yajl_gen_map_close(gen);
  yajl_gen_reset(gen);

  if (frame->prev) {
    memprof_dump_stack_frame(gen, frame->prev);
  }
}

static void
memprof_dump_stack(yajl_gen gen)
{
  memprof_dump_stack_frame(gen, ruby_frame);
}

static void
memprof_dump_lsof(yajl_gen gen)
{
  VALUE cmd = rb_str_new2("lsof -np ");
  VALUE pid = rb_funcall(rb_mProcess, rb_intern("pid"), 0);
  rb_str_append(cmd, rb_funcall(pid, rb_intern("to_s"), 0));

  VALUE lsof = rb_funcall(rb_cObject, '`', 1, cmd);
  if (RTEST(lsof)) {
    VALUE newline = rb_str_new2("\n");
    VALUE lines = rb_funcall(lsof, rb_intern("split"), 1, newline);
    int i;
    for (i=1; i < RARRAY_LEN(lines); i++) {
      VALUE parts = rb_funcall(RARRAY_PTR(lines)[i], rb_intern("split"), 2, Qnil, INT2FIX(9));

      yajl_gen_map_open(gen);

      yajl_gen_cstr(gen, "_id");
      yajl_gen_format(gen, "lsof:%d", i);

      yajl_gen_cstr(gen, "type");
      yajl_gen_cstr(gen, "lsof");

      yajl_gen_cstr(gen, "fd");
      yajl_gen_cstr(gen, RSTRING_PTR(RARRAY_PTR(parts)[3]));

      yajl_gen_cstr(gen, "fd_type");
      yajl_gen_cstr(gen, RSTRING_PTR(RARRAY_PTR(parts)[4]));

      yajl_gen_cstr(gen, "fd_name");
      yajl_gen_cstr(gen, RSTRING_PTR(RARRAY_PTR(parts)[RARRAY_LEN(parts)-1]));

      yajl_gen_map_close(gen);
      yajl_gen_reset(gen);
    }
  }
}

static void
json_print(void *ctx, const char * str, unsigned int len)
{
  FILE *out = (FILE *)ctx;
  size_t written = 0;
  while(1) {
    written += fwrite(str + written, sizeof(char), len - written, out ? out : stdout);
    if (written == len) break;
  }
}

static VALUE
memprof_dump(int argc, VALUE *argv, VALUE self)
{
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

  yajl_gen_config conf = { .beautify = 1, .indentString = "  " };
  yajl_gen gen = yajl_gen_alloc2((yajl_print_t)&json_print, &conf, NULL, (void*)out);

  track_objs = 0;

  yajl_gen_array_open(gen);
  st_foreach(objs, objs_each_dump, (st_data_t)gen);
  yajl_gen_array_close(gen);
  yajl_gen_free(gen);

  if (out)
    fclose(out);

  track_objs = 1;

  return Qnil;
}

static VALUE
memprof_dump_all(int argc, VALUE *argv, VALUE self)
{
  if (memprof_config.heaps == NULL ||
      memprof_config.heaps_used == NULL ||
      memprof_config.sizeof_RVALUE == 0 ||
      memprof_config.sizeof_heaps_slot == 0 ||
      memprof_config.offset_heaps_slot_slot == SIZE_MAX ||
      memprof_config.offset_heaps_slot_limit == SIZE_MAX)
    rb_raise(eUnsupported, "not enough config data to dump heap");

  char *heaps = *(char**)memprof_config.heaps;
  int heaps_used = *(int*)memprof_config.heaps_used;

  char *p, *pend;
  int i, limit;
  VALUE str;
  char *filename = NULL;
  char *in_progress_filename = NULL;
  FILE *out = NULL;

  rb_scan_args(argc, argv, "01", &str);

  if (RTEST(str)) {
    filename = StringValueCStr(str);
    size_t filename_len = strlen(filename);
    in_progress_filename = alloca(filename_len + 13);
    memcpy(in_progress_filename, filename, filename_len);
    memcpy(in_progress_filename + filename_len, ".IN_PROGRESS\0", 13);

    out = fopen(in_progress_filename, "w");
    if (!out)
      rb_raise(rb_eArgError, "unable to open output file");
  }

  yajl_gen_config conf = { .beautify = 0, .indentString = "  " };
  yajl_gen gen = yajl_gen_alloc2((yajl_print_t)&json_print, &conf, NULL, (void*)out);

  track_objs = 0;

  memprof_dump_globals(gen);
  memprof_dump_stack(gen);

  for (i=0; i < heaps_used; i++) {
    p = *(char**)(heaps + (i * memprof_config.sizeof_heaps_slot) + memprof_config.offset_heaps_slot_slot);
    limit = *(int*)(heaps + (i * memprof_config.sizeof_heaps_slot) + memprof_config.offset_heaps_slot_limit);
    pend = p + (memprof_config.sizeof_RVALUE * limit);

    while (p < pend) {
      if (RBASIC(p)->flags) {
        obj_dump((VALUE)p, gen);
        yajl_gen_reset(gen);
      }

      p += memprof_config.sizeof_RVALUE;
    }
  }

  memprof_dump_lsof(gen);

  yajl_gen_clear(gen);
  yajl_gen_free(gen);

  if (out) {
    fclose(out);
    rename(in_progress_filename, filename);
  }

  track_objs = 1;

  return Qnil;
}

static void
init_memprof_config_base() {
  memset(&memprof_config, 0, sizeof(memprof_config));
  memprof_config.offset_heaps_slot_limit = SIZE_MAX;
  memprof_config.offset_heaps_slot_slot = SIZE_MAX;
  memprof_config.pagesize = getpagesize();
  assert(memprof_config.pagesize);
}

static void
init_memprof_config_extended() {
  /* If we don't have add_freelist, find the functions it gets inlined into */
  memprof_config.add_freelist               = bin_find_symbol("add_freelist", NULL, 0);

  /*
   * Sometimes gc_sweep gets inlined in garbage_collect
   * (e.g., on REE it gets inlined into garbage_collect_0).
   */
  if (memprof_config.add_freelist == NULL) {
    memprof_config.gc_sweep                 = bin_find_symbol("gc_sweep",
                                                &memprof_config.gc_sweep_size, 0);
    if (memprof_config.gc_sweep == NULL)
      memprof_config.gc_sweep               = bin_find_symbol("garbage_collect_0",
                                                &memprof_config.gc_sweep_size, 0);
    if (memprof_config.gc_sweep == NULL)
      memprof_config.gc_sweep               = bin_find_symbol("garbage_collect",
                                                &memprof_config.gc_sweep_size, 0);

    memprof_config.finalize_list            = bin_find_symbol("finalize_list",
                                                &memprof_config.finalize_list_size, 0);
    memprof_config.rb_gc_force_recycle      = bin_find_symbol("rb_gc_force_recycle",
                                                &memprof_config.rb_gc_force_recycle_size, 0);
    memprof_config.freelist                 = bin_find_symbol("freelist", NULL, 0);
  }

  memprof_config.classname                  = bin_find_symbol("classname", NULL, 0);
  memprof_config.timeofday                  = bin_find_symbol("timeofday", NULL, 0);
  memprof_config.bm_mark                    = bin_find_symbol("bm_mark", NULL, 0);
  memprof_config.blk_free                   = bin_find_symbol("blk_free", NULL, 0);
  memprof_config.thread_mark                = bin_find_symbol("thread_mark", NULL, 0);
  memprof_config.rb_mark_table_add_filename = bin_find_symbol("rb_mark_table_add_filename", NULL, 0);

  /* Stuff for dumping the heap */
  memprof_config.heaps                      = bin_find_symbol("heaps", NULL, 0);
  memprof_config.heaps_used                 = bin_find_symbol("heaps_used", NULL, 0);
#ifdef sizeof__RVALUE
  memprof_config.sizeof_RVALUE              = sizeof__RVALUE;
#else
  memprof_config.sizeof_RVALUE              = bin_type_size("RVALUE");
#endif
#ifdef sizeof__heaps_slot
  memprof_config.sizeof_heaps_slot          = sizeof__heaps_slot;
#else
  memprof_config.sizeof_heaps_slot          = bin_type_size("heaps_slot");
#endif
#ifdef offset__heaps_slot__limit
  memprof_config.offset_heaps_slot_limit    = offset__heaps_slot__limit;
#else
  memprof_config.offset_heaps_slot_limit    = bin_type_member_offset("heaps_slot", "limit");
#endif
#ifdef offset__heaps_slot__slot
  memprof_config.offset_heaps_slot_slot     = offset__heaps_slot__slot;
#else
  memprof_config.offset_heaps_slot_slot     = bin_type_member_offset("heaps_slot", "slot");
#endif
#ifdef offset__BLOCK__body
  memprof_config.offset_BLOCK_body          = offset__BLOCK__body;
#else
  memprof_config.offset_BLOCK_body          = bin_type_member_offset("BLOCK", "body");
#endif
#ifdef offset__BLOCK__var
  memprof_config.offset_BLOCK_var           = offset__BLOCK__var;
#else
  memprof_config.offset_BLOCK_var           = bin_type_member_offset("BLOCK", "var");
#endif
#ifdef offset__BLOCK__cref
  memprof_config.offset_BLOCK_cref          = offset__BLOCK__cref;
#else
  memprof_config.offset_BLOCK_cref          = bin_type_member_offset("BLOCK", "cref");
#endif
#ifdef offset__BLOCK__prev
  memprof_config.offset_BLOCK_prev          = offset__BLOCK__prev;
#else
  memprof_config.offset_BLOCK_prev          = bin_type_member_offset("BLOCK", "prev");
#endif
#ifdef offset__BLOCK__self
  memprof_config.offset_BLOCK_self          = offset__BLOCK__self;
#else
  memprof_config.offset_BLOCK_self          = bin_type_member_offset("BLOCK", "self");
#endif
#ifdef offset__BLOCK__klass
  memprof_config.offset_BLOCK_klass         = offset__BLOCK__klass;
#else
  memprof_config.offset_BLOCK_klass         = bin_type_member_offset("BLOCK", "klass");
#endif
#ifdef offset__BLOCK__orig_thread
  memprof_config.offset_BLOCK_orig_thread   = offset__BLOCK__orig_thread;
#else
  memprof_config.offset_BLOCK_orig_thread   = bin_type_member_offset("BLOCK", "orig_thread");
#endif
#ifdef offset__BLOCK__wrapper
  memprof_config.offset_BLOCK_wrapper       = offset__BLOCK__wrapper;
#else
  memprof_config.offset_BLOCK_wrapper       = bin_type_member_offset("BLOCK", "wrapper");
#endif
#ifdef offset__BLOCK__block_obj
  memprof_config.offset_BLOCK_block_obj     = offset__BLOCK__block_obj;
#else
  memprof_config.offset_BLOCK_block_obj     = bin_type_member_offset("BLOCK", "block_obj");
#endif
#ifdef offset__BLOCK__scope
  memprof_config.offset_BLOCK_scope         = offset__BLOCK__scope;
#else
  memprof_config.offset_BLOCK_scope         = bin_type_member_offset("BLOCK", "scope");
#endif
#ifdef offset__BLOCK__dyna_vars
  memprof_config.offset_BLOCK_dyna_vars     = offset__BLOCK__dyna_vars;
#else
  memprof_config.offset_BLOCK_dyna_vars     = bin_type_member_offset("BLOCK", "dyna_vars");
#endif
#ifdef offset__METHOD__klass
  memprof_config.offset_METHOD_klass        = offset__METHOD__klass;
#else
  memprof_config.offset_METHOD_klass        = bin_type_member_offset("METHOD", "klass");
#endif
#ifdef offset__METHOD__rklass
  memprof_config.offset_METHOD_rklass       = offset__METHOD__rklass;
#else
  memprof_config.offset_METHOD_rklass       = bin_type_member_offset("METHOD", "rklass");
#endif
#ifdef offset__METHOD__recv
  memprof_config.offset_METHOD_recv         = offset__METHOD__recv;
#else
  memprof_config.offset_METHOD_recv         = bin_type_member_offset("METHOD", "recv");
#endif
#ifdef offset__METHOD__id
  memprof_config.offset_METHOD_id           = offset__METHOD__id;
#else
  memprof_config.offset_METHOD_id           = bin_type_member_offset("METHOD", "id");
#endif
#ifdef offset__METHOD__oid
  memprof_config.offset_METHOD_oid          = offset__METHOD__oid;
#else
  memprof_config.offset_METHOD_oid          = bin_type_member_offset("METHOD", "oid");
#endif
#ifdef offset__METHOD__body
  memprof_config.offset_METHOD_body         = offset__METHOD__body;
#else
  memprof_config.offset_METHOD_body         = bin_type_member_offset("METHOD", "body");
#endif

  int heap_errors_printed = 0;

  if (memprof_config.heaps == NULL)
    heap_errors_printed += fprintf(stderr,
      "Failed to locate heaps\n");
  if (memprof_config.heaps_used == NULL)
    heap_errors_printed += fprintf(stderr,
      "Failed to locate heaps_used\n");
  if (memprof_config.sizeof_RVALUE == 0)
    heap_errors_printed += fprintf(stderr,
      "Failed to determine sizeof(RVALUE)\n");
  if (memprof_config.sizeof_heaps_slot == 0)
    heap_errors_printed += fprintf(stderr,
      "Failed to determine sizeof(heaps_slot)\n");
  if (memprof_config.offset_heaps_slot_limit == SIZE_MAX)
    heap_errors_printed += fprintf(stderr,
      "Failed to determine offset of heaps_slot->limit\n");
  if (memprof_config.offset_heaps_slot_slot == SIZE_MAX)
    heap_errors_printed += fprintf(stderr,
      "Failed to determine offset of heaps_slot->slot\n");

  if (heap_errors_printed)
    fprintf(stderr, "You won't be able to dump your heap!\n");

  int errors_printed = 0;

  /* If we can't find add_freelist, we need to make sure we located the functions that it gets inlined into. */
  if (memprof_config.add_freelist == NULL) {
    if (memprof_config.gc_sweep == NULL) {
      errors_printed += fprintf(stderr,
        "Failed to locate add_freelist (it's probably inlined, but we couldn't find it there either!)\n");
      errors_printed += fprintf(stderr,
        "Failed to locate gc_sweep, garbage_collect_0, or garbage_collect\n");
    }
    if (memprof_config.gc_sweep_size == 0)
      errors_printed += fprintf(stderr,
        "Failed to determine the size of gc_sweep/garbage_collect_0/garbage_collect: %ld\n",
        memprof_config.gc_sweep_size);
    if (memprof_config.finalize_list == NULL)
      errors_printed += fprintf(stderr,
        "Failed to locate finalize_list\n");
    if (memprof_config.finalize_list_size == 0)
      errors_printed += fprintf(stderr,
        "Failed to determine the size of finalize_list: %ld\n",
        memprof_config.finalize_list_size);
    if (memprof_config.rb_gc_force_recycle == NULL)
      errors_printed += fprintf(stderr,
        "Failed to locate rb_gc_force_recycle\n");
    if (memprof_config.rb_gc_force_recycle_size == 0)
      errors_printed += fprintf(stderr,
        "Failed to determine the size of rb_gc_force_recycle: %ld\n",
        memprof_config.rb_gc_force_recycle_size);
    if (memprof_config.freelist == NULL)
      errors_printed += fprintf(stderr,
        "Failed to locate freelist\n");
  }

  if (memprof_config.classname == NULL)
    errors_printed += fprintf(stderr,
      "Failed to locate classname\n");

  if (errors_printed) {
    VALUE ruby_build_info = rb_eval_string("require 'rbconfig'; RUBY_DESCRIPTION + '\n' + RbConfig::CONFIG['CFLAGS'];");
    /* who knows what could happen */
    if (TYPE(ruby_build_info) == T_STRING)
      fprintf(stderr, "%s\n", StringValuePtr(ruby_build_info));
    errx(EX_SOFTWARE, "Memprof does not have enough data to run. Please email this output to bugs@memprof.com");
  }
}

void
Init_memprof()
{
  VALUE memprof = rb_define_module("Memprof");
  eUnsupported = rb_define_class_under(memprof, "Unsupported", rb_eStandardError);
  rb_define_singleton_method(memprof, "start", memprof_start, 0);
  rb_define_singleton_method(memprof, "stop", memprof_stop, 0);
  rb_define_singleton_method(memprof, "stats", memprof_stats, -1);
  rb_define_singleton_method(memprof, "stats!", memprof_stats_bang, -1);
  rb_define_singleton_method(memprof, "track", memprof_track, -1);
  rb_define_singleton_method(memprof, "dump", memprof_dump, -1);
  rb_define_singleton_method(memprof, "dump_all", memprof_dump_all, -1);
  rb_define_singleton_method(memprof, "track_bytes", memprof_track_bytes, -1);

  objs = st_init_numtable();
  init_memprof_config_base();
  bin_init();
  init_memprof_config_extended();
  create_tramp_table();

  gc_hook = Data_Wrap_Struct(rb_cObject, sourcefile_marker, NULL, NULL);
  rb_global_variable(&gc_hook);

  rb_classname = memprof_config.classname;
  rb_add_freelist = memprof_config.add_freelist;
  rb_timeofday = memprof_config.timeofday;
  rb_bm_mark = memprof_config.bm_mark;
  rb_blk_free = memprof_config.blk_free;
  rb_thread_mark = memprof_config.thread_mark;
  ptr_to_rb_mark_table_add_filename = memprof_config.rb_mark_table_add_filename;

  assert(rb_classname);

  return;
}
