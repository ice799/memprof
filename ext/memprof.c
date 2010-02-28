#include <ruby.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
#include <assert.h>

#include <st.h>
#include <intern.h>
#include <node.h>

#include "arch.h"
#include "bin_api.h"


size_t pagesize;

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
static VALUE eUnsupported;
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

  if (track_objs && objs) {
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
      rb_gc_disable();
      st_insert(objs, (st_data_t)ret, (st_data_t)tracker);
      rb_gc_enable();
    } else {
      fprintf(stderr, "Warning, unable to allocate a tracker. You are running dangerously low on RAM!\n");
    }
  }

  return ret;
}

static void (*rb_add_freelist)(VALUE);

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
  
  asprintf(&(res->entries[res->num_entries++]), "%7li %s", count, source);

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
  yajl_gen_status ret;

  va_start(args, format);
  vasprintf(&str, format, args);
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
  char *heaps = *(char**)bin_find_symbol("heaps",0);
  int heaps_used = *(int*)bin_find_symbol("heaps_used",0);

#ifndef sizeof__RVALUE
  int sizeof__RVALUE = bin_type_size("RVALUE");
#endif
#ifndef sizeof__heaps_slot
  int sizeof__heaps_slot = bin_type_size("heaps_slot");
#endif
#ifndef offset__heaps_slot__limit
  int offset__heaps_slot__limit = bin_type_member_offset("heaps_slot", "limit");
#endif
#ifndef offset__heaps_slot__slot
  int offset__heaps_slot__slot = bin_type_member_offset("heaps_slot", "slot");
#endif

  char *p, *pend;
  int i, limit;

  if (sizeof__RVALUE < 0 || sizeof__heaps_slot < 0)
    rb_raise(eUnsupported, "could not find internal heap");

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
    p = *(char**)(heaps + (i * sizeof__heaps_slot) + offset__heaps_slot__slot);
    limit = *(int*)(heaps + (i * sizeof__heaps_slot) + offset__heaps_slot__limit);
    pend = p + (sizeof__RVALUE * limit);

    while (p < pend) {
      if (RBASIC(p)->flags) {
        obj_dump((VALUE)p, gen);
        // XXX ugh
        yajl_gen_clear(gen);
        yajl_gen_free(gen);
        gen = yajl_gen_alloc2((yajl_print_t)&json_print, &conf, NULL, (void*)out);
        while(fputc('\n', out ? out : stdout) == EOF);
      }

      p += sizeof__RVALUE;
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

static void
create_tramp_table()
{
  int i = 0;
  void *region = NULL;
   size_t tramp_sz = 0, inline_tramp_sz = 0;

  void *ent = arch_get_st2_tramp(&tramp_sz);
  void *inline_ent = arch_get_inline_st2_tramp(&inline_tramp_sz);

  if ((region = bin_allocate_page()) == MAP_FAILED)
    errx(EX_SOFTWARE, "Failed to allocate memory for stage 1 trampolines.");

  tramp_table = region;
  inline_tramp_table = region + pagesize/2;

  for (i = 0; i < (pagesize/2)/tramp_sz; i++) {
    memcpy(tramp_table + i, ent, tramp_sz);
  }

  for (i = 0; i < (pagesize/2)/inline_tramp_sz; i++) {
    memcpy(inline_tramp_table + i, inline_ent, inline_tramp_sz);
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
  int tramps_completed = 0;

  freelist_inliners[0] = bin_find_symbol("gc_sweep", &sizes[0]);
  /* sometimes gc_sweep gets inlined in garbage_collect */
  /* on REE, it gets inlined into garbage_collect_0 */
  if (!freelist_inliners[0])
    freelist_inliners[0] = bin_find_symbol("garbage_collect_0", &sizes[0]);
  if (!freelist_inliners[0])
    freelist_inliners[0] = bin_find_symbol("garbage_collect", &sizes[0]);
  if (!freelist_inliners[0]) {
    /* couldn't find anything containing gc_sweep. */
    errx(EX_SOFTWARE, "Couldn't find gc_sweep or garbage_collect!");
  }

  freelist_inliners[1] = bin_find_symbol("finalize_list", &sizes[1]);
  if (!freelist_inliners[1])
    errx(EX_SOFTWARE, "Couldn't find finalize_list!");

  freelist_inliners[2] = bin_find_symbol("rb_gc_force_recycle", &sizes[2]);
  if (!freelist_inliners[2])
    errx(EX_SOFTWARE, "Couldn't find rb_gc_force_recycle!");

  freelist = bin_find_symbol("freelist", NULL);
  if (!freelist)
    errx(EX_SOFTWARE, "Couldn't find freelist!");

  /* start the search for places to insert the inline tramp */

  byte = freelist_inliners[i];

  while (i < FREELIST_INLINES) {
    if (arch_insert_inline_st2_tramp(byte, freelist, freelist_tramp,
        &inline_tramp_table[entry]) == 0) {
      /* insert occurred, so increment internal counters for the tramp table */
      entry++;
      inline_tramp_size++;

      /* add_freelist() only gets inlined *ONCE* into any of the 3 functions that we're scanning, */
      /* so move on to the next 'inliner' when after we tramp the first instruction we find. */
      /* REE's gc_sweep has 2 calls, but this gets optimized into a single inlining and a jmp to it */
      /* older patchlevels of 1.8.7 don't have an add_freelist(), but the instruction should be the same */
      tramps_completed++;
      i++;
      byte = freelist_inliners[i];
      continue;
    }

    /* if we've looked at all the bytes in this function... */
    if (((void *)byte - freelist_inliners[i]) >= sizes[i]) {
      /* move on to the next function */
      i++;
      byte = freelist_inliners[i];
    }
    byte++;
  }

  if (tramps_completed != 3)
    errx(EX_SOFTWARE, "Inline add_freelist tramp insertion failed! Only inserted %d tramps.", tramps_completed);
}

static void
insert_tramp(char *trampee, void *tramp)
{
  void *trampee_addr = bin_find_symbol(trampee, NULL);
  int inline_ent = inline_tramp_size;

  if (trampee_addr == NULL) {
    if (strcmp("add_freelist", trampee) == 0) {
      /* XXX super hack */
      inline_tramp_size++;
      hook_freelist(inline_ent);
    } else {
      errx(EX_SOFTWARE, "Failed to locate required symbol %s", trampee);
    }
  } else {
    if (strcmp("add_freelist", trampee) == 0) {
      rb_add_freelist = trampee_addr;
    }

    tramp_table[tramp_size].addr = tramp;
    if (bin_update_image(trampee, &tramp_table[tramp_size]) != 0)
      errx(EX_SOFTWARE, "Failed to insert tramp for %s", trampee);
    tramp_size++;
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

  pagesize = getpagesize();
  objs = st_init_numtable();
  bin_init();
  create_tramp_table();
  rb_add_freelist = NULL;

  insert_tramp("rb_newobj", newobj_tramp);
  insert_tramp("add_freelist", freelist_tramp);

  rb_classname = bin_find_symbol("classname", 0);

  if (getenv("MEMPROF"))
    track_objs = 1;

  return;
}
