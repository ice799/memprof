#include <ruby.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <err.h>
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
static st_table *objs = NULL;

struct memprof_config memprof_config;

struct obj_track {
  VALUE obj;
  char *source;
  int line;
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
    tracker = malloc(sizeof(*tracker));

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
  unsigned long num_entries;
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

#include <yajl/yajl_gen.h>
#include <stdarg.h>
#include "env.h"
#include "re.h"

yajl_gen_status
yajl_gen_cstr(yajl_gen gen, const char * str)
{
  if (!str || str[0] == 0)
    return yajl_gen_null(gen);
  else
    return yajl_gen_string(gen, (unsigned char *)str, strlen(str));
}

yajl_gen_status
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

yajl_gen_status
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
    return yajl_gen_format(gen, ":%s", rb_id2name(SYM2ID(obj)));
  else
    return yajl_gen_format(gen, "0x%x", obj);
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

char *
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

static VALUE (*rb_classname)(VALUE);

/* TODO
 *  look for FL_EXIVAR flag and print ivars
 *  print more detail about Proc/struct BLOCK in T_DATA if freefunc == blk_free
 *  add Memprof.dump_all for full heap dump
 *  print details on different types of nodes (nd_next, nd_lit, nd_nth, etc)
 */

void
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
  }

  yajl_gen_cstr(gen, "type");
  switch (type=BUILTIN_TYPE(obj)) {
    case T_DATA:
      yajl_gen_cstr(gen, "data");

      if (RBASIC(obj)->klass) {
        yajl_gen_cstr(gen, "class");
        yajl_gen_value(gen, RBASIC(obj)->klass);

        yajl_gen_cstr(gen, "class_name");
        VALUE name = rb_classname(RBASIC(obj)->klass);
        if (RTEST(name))
          yajl_gen_cstr(gen, RSTRING(name)->ptr);
        else
          yajl_gen_cstr(gen, 0);
      }
      break;

    case T_FILE:
      yajl_gen_cstr(gen, "file");
      break;

    case T_FLOAT:
      yajl_gen_cstr(gen, "float");

      yajl_gen_cstr(gen, "data");
      yajl_gen_double(gen, RFLOAT(obj)->value);
      break;

    case T_BIGNUM:
      yajl_gen_cstr(gen, "bignum");

      yajl_gen_cstr(gen, "negative");
      yajl_gen_bool(gen, RBIGNUM(obj)->sign == 0);

      yajl_gen_cstr(gen, "length");
      yajl_gen_integer(gen, RBIGNUM(obj)->len);

      yajl_gen_cstr(gen, "data");
      yajl_gen_string(gen, RBIGNUM(obj)->digits, RBIGNUM(obj)->len);
      break;

    case T_MATCH:
      yajl_gen_cstr(gen, "match");

      yajl_gen_cstr(gen, "data");
      yajl_gen_value(gen, RMATCH(obj)->str);
      break;

    case T_REGEXP:
      yajl_gen_cstr(gen, "regexp");

      yajl_gen_cstr(gen, "length");
      yajl_gen_integer(gen, RREGEXP(obj)->len);

      yajl_gen_cstr(gen, "data");
      yajl_gen_cstr(gen, RREGEXP(obj)->str);
      break;

    case T_SCOPE:
      yajl_gen_cstr(gen, "scope");

      struct SCOPE *scope = (struct SCOPE *)obj;
      if (scope->local_tbl) {
        int i = 1;
        int n = scope->local_tbl[0];
        VALUE *list = &scope->local_vars[-1];
        VALUE cur = *list++;

        yajl_gen_cstr(gen, "node");
        yajl_gen_value(gen, cur);

        if (n) {
          yajl_gen_cstr(gen, "variables");
          yajl_gen_map_open(gen);
          while (n--) {
            cur = *list++;
            yajl_gen_cstr(gen, scope->local_tbl[i] == 95 ? "_" : rb_id2name(scope->local_tbl[i]));
            yajl_gen_value(gen, cur);
            i++;
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

      switch (nd_type(obj)) {
        case NODE_SCOPE:
          break;
      }
      break;

    case T_STRING:
      yajl_gen_cstr(gen, "string");

      yajl_gen_cstr(gen, "length");
      yajl_gen_integer(gen, RSTRING(obj)->len);

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
        yajl_gen_string(gen, (unsigned char *)RSTRING(obj)->ptr, RSTRING(obj)->len);
      }
      break;

    case T_VARMAP:
      yajl_gen_cstr(gen, "varmap");

      struct RVarmap *vars = (struct RVarmap *)obj;

      if (vars->next) {
        yajl_gen_cstr(gen, "next");
        yajl_gen_value(gen, (VALUE)vars->next);
      }

      if (vars->id) {
        yajl_gen_cstr(gen, "data");
        yajl_gen_map_open(gen);
        yajl_gen_cstr(gen, rb_id2name(vars->id));
        yajl_gen_value(gen, vars->val);
        yajl_gen_map_close(gen);
      }
      break;

    case T_CLASS:
    case T_MODULE:
    case T_ICLASS:
      yajl_gen_cstr(gen, type==T_CLASS ? "class" : type==T_MODULE ? "module" : "iclass");

      yajl_gen_cstr(gen, "name");
      VALUE name = rb_classname(obj);
      if (RTEST(name))
        yajl_gen_cstr(gen, RSTRING(name)->ptr);
      else
        yajl_gen_cstr(gen, 0);

      yajl_gen_cstr(gen, "super");
      yajl_gen_value(gen, RCLASS(obj)->super);

      yajl_gen_cstr(gen, "super_name");
      VALUE super_name = rb_classname(RCLASS(obj)->super);
      if (RTEST(super_name))
        yajl_gen_cstr(gen, RSTRING(super_name)->ptr);
      else
        yajl_gen_cstr(gen, 0);

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

      if (type != T_ICLASS && RCLASS(obj)->m_tbl && RCLASS(obj)->m_tbl->num_entries) {
        yajl_gen_cstr(gen, "methods");
        yajl_gen_map_open(gen);
        st_foreach(RCLASS(obj)->m_tbl, each_ivar, (st_data_t)gen);
        yajl_gen_map_close(gen);
      }
      break;

    case T_OBJECT:
      yajl_gen_cstr(gen, "object");

      yajl_gen_cstr(gen, "class");
      yajl_gen_value(gen, RBASIC(obj)->klass);

      yajl_gen_cstr(gen, "class_name");
      yajl_gen_cstr(gen, rb_obj_classname(obj));

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

void
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
      memprof_config.offset_heaps_slot_slot == -1 ||
      memprof_config.offset_heaps_slot_limit == -1)
    rb_raise(eUnsupported, "not enough config data to dump heap");

  char *heaps = *(char**)memprof_config.heaps;
  int heaps_used = *(int*)memprof_config.heaps_used;

  char *p, *pend;
  int i, limit;
  VALUE str;
  FILE *out = NULL;

  rb_scan_args(argc, argv, "01", &str);

  if (RTEST(str)) {
    out = fopen(StringValueCStr(str), "w");
    if (!out)
      rb_raise(rb_eArgError, "unable to open output file");
  }

  yajl_gen_config conf = { .beautify = 0, .indentString = "  " };
  yajl_gen gen = yajl_gen_alloc2((yajl_print_t)&json_print, &conf, NULL, (void*)out);

  track_objs = 0;

  //yajl_gen_array_open(gen);

  for (i=0; i < heaps_used; i++) {
    p = *(char**)(heaps + (i * memprof_config.sizeof_heaps_slot) + memprof_config.offset_heaps_slot_slot);
    limit = *(int*)(heaps + (i * memprof_config.sizeof_heaps_slot) + memprof_config.offset_heaps_slot_limit);
    pend = p + (memprof_config.sizeof_RVALUE * limit);

    while (p < pend) {
      if (RBASIC(p)->flags) {
        obj_dump((VALUE)p, gen);
        // XXX ugh
        yajl_gen_clear(gen);
        yajl_gen_free(gen);
        gen = yajl_gen_alloc2((yajl_print_t)&json_print, &conf, NULL, (void*)out);
        while(fputc('\n', out ? out : stdout) == EOF);
      }

      p += memprof_config.sizeof_RVALUE;
    }
  }

  //yajl_gen_array_close(gen);
  yajl_gen_clear(gen);
  yajl_gen_free(gen);

  if (out)
    fclose(out);

  track_objs = 1;

  return Qnil;
}

void
init_memprof_config() {
  memset(&memprof_config, 0, sizeof(struct memprof_config));
  memprof_config.offset_heaps_slot_limit = -1;
  memprof_config.offset_heaps_slot_slot = -1;

  /* If we don't have add_freelist, find the functions it gets inlined into */
  memprof_config.add_freelist               = bin_find_symbol("add_freelist", NULL);

  /*
   * Sometimes gc_sweep gets inlined in garbage_collect
   * (e.g., on REE it gets inlined into garbage_collect_0).
   */
  if (memprof_config.add_freelist == NULL) {
    memprof_config.gc_sweep                 = bin_find_symbol("gc_sweep",
                                                &memprof_config.gc_sweep_size);
    if (memprof_config.gc_sweep == NULL)
      memprof_config.gc_sweep               = bin_find_symbol("garbage_collect_0",
                                                &memprof_config.gc_sweep_size);
    if (memprof_config.gc_sweep == NULL)
      memprof_config.gc_sweep               = bin_find_symbol("garbage_collect",
                                                &memprof_config.gc_sweep_size);

    memprof_config.finalize_list            = bin_find_symbol("finalize_list",
                                                &memprof_config.finalize_list_size);
    memprof_config.rb_gc_force_recycle      = bin_find_symbol("rb_gc_force_recycle",
                                                &memprof_config.rb_gc_force_recycle_size);
    memprof_config.freelist                 = bin_find_symbol("freelist", NULL);
  }

  memprof_config.classname                  = bin_find_symbol("classname", NULL);
  memprof_config.rb_mark_table_add_filename = bin_find_symbol("rb_mark_table_add_filename", NULL);

  /* Stuff for dumping the heap */
  memprof_config.heaps                      = bin_find_symbol("heaps", NULL);
  memprof_config.heaps_used                 = bin_find_symbol("heaps_used", NULL);
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
  if (memprof_config.offset_heaps_slot_limit == -1)
    heap_errors_printed += fprintf(stderr,
      "Failed to determine offset of heaps_slot->limit\n");
  if (memprof_config.offset_heaps_slot_slot == -1)
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
    errx(EX_SOFTWARE, "Memprof does not have enough data to run. Please email this output to Memprof dudes.");
  }
}

void
Init_memprof()
{
  init_memprof_config();

  VALUE memprof = rb_define_module("Memprof");
  eUnsupported = rb_define_class_under(memprof, "Unsupported", rb_eStandardError);
  rb_define_singleton_method(memprof, "start", memprof_start, 0);
  rb_define_singleton_method(memprof, "stop", memprof_stop, 0);
  rb_define_singleton_method(memprof, "stats", memprof_stats, -1);
  rb_define_singleton_method(memprof, "stats!", memprof_stats_bang, -1);
  rb_define_singleton_method(memprof, "track", memprof_track, -1);
  rb_define_singleton_method(memprof, "dump", memprof_dump, -1);
  rb_define_singleton_method(memprof, "dump_all", memprof_dump_all, -1);

  pagesize = getpagesize();
  objs = st_init_numtable();
  bin_init();
  create_tramp_table();

  gc_hook = Data_Wrap_Struct(rb_cObject, sourcefile_marker, NULL, NULL);
  rb_global_variable(&gc_hook);

  rb_classname = memprof_config.classname;
  rb_add_freelist = memprof_config.add_freelist;
  ptr_to_rb_mark_table_add_filename = memprof_config.rb_mark_table_add_filename;

  assert(rb_classname);

  insert_tramp("rb_newobj", newobj_tramp);
  insert_tramp("add_freelist", freelist_tramp);

  if (getenv("MEMPROF"))
    track_objs = 1;

  return;
}
