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

#include "bin_api.h"

size_t pagesize;
void *text_segment = NULL;
unsigned long text_segment_len = 0;

/*
   trampoline specific stuff
 */
struct tramp_tbl_entry *tramp_table = NULL;
size_t tramp_size = 0;

/*
   inline trampoline specific stuff
 */
size_t inline_tramp_size = 0;
struct inline_tramp_tbl_entry *inline_tramp_table = NULL;

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

static void
error_tramp()
{
  printf("WARNING: NO TRAMPOLINE SET.\n");
  return;
}

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

#include <yajl/yajl_gen.h>
#include <stdarg.h>
#include "env.h"
#include "re.h"

yajl_gen_status
yajl_gen_cstr(yajl_gen gen, const unsigned char * str)
{
  if (!str || str[0] == 0)
    return yajl_gen_null(gen);
  else
    return yajl_gen_string(gen, str, strlen(str));
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

  ret = yajl_gen_string(gen, str, strlen(str));
  free(str);
  return ret;
}

yajl_gen_status
yajl_gen_value(yajl_gen gen, VALUE obj)
{
  if (FIXNUM_P(obj))
    return yajl_gen_integer(gen, FIX2INT(obj));
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

  yajl_gen_value(gen, k);
  yajl_gen_value(gen, v);

  return ST_CONTINUE;
}

static int
each_ivar(st_data_t key, st_data_t record, st_data_t arg)
{
  yajl_gen gen = (yajl_gen)arg;
  ID id = (ID)key;
  VALUE val = (VALUE)record;

  yajl_gen_cstr(gen, rb_id2name(id));
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

  yajl_gen_cstr(gen, "address");
  yajl_gen_value(gen, obj);

  struct obj_track *tracker = NULL;
  if (st_lookup(objs, (st_data_t)obj, (st_data_t *)&tracker)) {
    yajl_gen_cstr(gen, "source");
    yajl_gen_format(gen, "%s:%d", tracker->source, tracker->line);
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
        yajl_gen_string(gen, RSTRING(obj)->ptr, RSTRING(obj)->len);
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
        yajl_gen_map_open(gen);
        st_foreach(hash->tbl, each_hash_entry, (st_data_t)gen);
        yajl_gen_map_close(gen);
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

yajl_print_t
json_print(void *ctx, const char * str, unsigned int len)
{
  fwrite(str, sizeof(char), len, stdout);
}

static VALUE
memprof_dump(int argc, VALUE *argv, VALUE self)
{
  yajl_gen_config conf = { .beautify = 1, .indentString = "  " };
  yajl_gen gen = yajl_gen_alloc2((yajl_print_t)&json_print, &conf, NULL, NULL);

  track_objs = 0;

  yajl_gen_array_open(gen);
  st_foreach(objs, objs_each_dump, (st_data_t)gen);
  yajl_gen_array_close(gen);
  yajl_gen_free(gen);

  track_objs = 1;

  return Qnil;
}

static VALUE
memprof_dump_all(int argc, VALUE *argv, VALUE self)
{
  int sizeof_RVALUE = bin_type_size("RVALUE");
  void *heaps = *(void**)bin_find_symbol("heaps",0);
  int heaps_used = *(int*)bin_find_symbol("heaps_used",0);
  int sizeof_heaps_slot = bin_type_size("heaps_slot");
  int offset_limit = bin_type_member_offset("heaps_slot", "limit");
  int offset_slot = bin_type_member_offset("heaps_slot", "slot");
  void *p, *pend;
  int i, limit;

  if (sizeof_RVALUE < 0 || sizeof_heaps_slot < 0)
    rb_raise(eUnsupported, "could not find internal heap");

  yajl_gen_config conf = { .beautify = 1, .indentString = "  " };
  yajl_gen gen = yajl_gen_alloc2((yajl_print_t)&json_print, &conf, NULL, NULL);

  track_objs = 0;

  yajl_gen_array_open(gen);

  for (i=0; i < heaps_used; i++) {
    p = *(void**)(heaps + (i * sizeof_heaps_slot) + offset_slot);
    limit = *(int*)(heaps + (i * sizeof_heaps_slot) + offset_limit);
    pend = p + (sizeof_RVALUE * limit);

    while (p < pend) {
      if (RBASIC(p)->flags)
        obj_dump((VALUE)p, gen);

      p += sizeof_RVALUE;
    }
  }

  yajl_gen_array_close(gen);
  yajl_gen_free(gen);

  track_objs = 1;

  return Qnil;
}

static void
create_tramp_table()
{
  int i = 0;
  void *region = NULL;

  struct tramp_tbl_entry ent = {
    .rbx_save      = {'\x53'},                // push rbx
    .mov           = {'\x48', '\xbb'},        // mov addr into rbx
    .addr          = error_tramp,             // ^^^
    .callq         = {'\xff', '\xd3'},        // callq rbx
    .rbx_restore   = {'\x5b'},                // pop rbx
    .ret           = {'\xc3'},                // ret
  };

  struct inline_tramp_tbl_entry inline_ent = {
    .rex     = {'\x48'},
    .mov     = {'\x89'},
    .src_reg = {'\x05'},
    .mov_displacement = 0,

    .frame = {
      .push_rdi = {'\x57'},
      .mov_rdi = {'\x48', '\x8b', '\x3d'},
      .rdi_source_displacement = 0,
      .push_rbx = {'\x53'},
      .push_rbp = {'\x55'},
      .save_rsp = {'\x48', '\x89', '\xe5'},
      .align_rsp = {'\x48', '\x83', '\xe4', '\xf0'},
      .mov = {'\x48', '\xbb'},
      .addr = error_tramp,
      .callq = {'\xff', '\xd3'},
      .leave = {'\xc9'},
      .rbx_restore = {'\x5b'},
      .rdi_restore = {'\x5f'},
    },

    .jmp  = {'\xe9'},
    .jmp_displacement = 0,
  };

  if ((region = bin_allocate_page()) == MAP_FAILED) {
    fprintf(stderr, "Failed to allocate memory for stage 1 trampolines.\n");
    return;
  }

  tramp_table = region;
  inline_tramp_table = region + pagesize/2;

  for (i = 0; i < (pagesize/2)/sizeof(struct tramp_tbl_entry); i++) {
    memcpy(tramp_table + i, &ent, sizeof(struct tramp_tbl_entry));
  }

  for (i = 0; i < (pagesize/2)/sizeof(struct inline_tramp_tbl_entry); i++) {
    memcpy(inline_tramp_table + i, &inline_ent, sizeof(struct inline_tramp_tbl_entry));
  }
}

void
update_callqs(int entry, void *trampee_addr)
{
  char *byte = text_segment;
  size_t count = 0;
  int fn_addr = 0;
  void *aligned_addr = NULL;

  for(; count < text_segment_len; count++) {
    if (*byte == '\xe8') {
      fn_addr = *(int *)(byte+1);
      if (((void *)trampee_addr - (void *)(byte+5)) == fn_addr) {
        aligned_addr = (void*)(((long)byte+1)&~(0xffff));
        mprotect(aligned_addr, (((void *)byte+1) - aligned_addr) + 10, PROT_READ|PROT_WRITE|PROT_EXEC);
        *(int  *)(byte+1) = (uint32_t)((void *)(tramp_table + entry) - (void *)(byte + 5));
        mprotect(aligned_addr, (((void *)byte+1) - aligned_addr) + 10, PROT_READ|PROT_EXEC);
      }
    }
    byte++;
  }
}


static void
hook_freelist(int entry)
{
  long sizes[] = { 0, 0, 0 };
  void *sym1 = bin_find_symbol("gc_sweep", &sizes[0]);

  if (sym1 == NULL) {
    /* this is MRI ... */
    sym1 = bin_find_symbol("garbage_collect", &sizes[0]);
  }

  void *sym2 = bin_find_symbol("finalize_list", &sizes[1]);
  void *sym3 = bin_find_symbol("rb_gc_force_recycle", &sizes[2]);
  void *freelist_callers[] = { sym1, sym2, sym3 };
  int max = 3;
  size_t i = 0;
  char *byte = freelist_callers[0];
  void *freelist = bin_find_symbol("freelist", NULL);
  uint32_t mov_target =  0;
  void *aligned_addr = NULL;
  size_t count = 0;

  /* This is the stage 1 trampoline for hooking the inlined add_freelist
   * function .
   *
   * NOTE: The original instruction mov %reg, freelist is 7 bytes wide,
   * whereas jmpq $displacement is only 5 bytes wide. We *must* pad out
   * the next two bytes. This will be important to remember below.
   */
  struct tramp_inline tramp = {
    .jmp           = {'\xe9'},
    .displacement  = 0,
    .pad           = {'\x90', '\x90'},
  };

  struct inline_tramp_tbl_entry *inl_tramp_st2 = NULL;

  for (;i < max;) {
    /* make sure it is a mov instruction */
    if (byte[1] == '\x89') {

      /* Read the REX byte to make sure it is a mov that we care about */
      if ((byte[0] == '\x48') ||
          (byte[0] == '\x4c')) {

        /* Grab the target of the mov. REMEMBER: in this case the target is 
         * a 32bit displacment that gets added to RIP (where RIP is the adress of
         * the next instruction).
         */
        mov_target = *(uint32_t *)(byte + 3);

        /* Sanity check. Ensure that the displacement from freelist to the next
         * instruction matches the mov_target. If so, we know this mov is
         * updating freelist.
         */
        if ((freelist - (void *)(byte+7)) == mov_target) {
          /* Before the stage 1 trampoline gets written, we need to generate
           * the code for the stage 2 trampoline. Let's copy over the REX byte
           * and the byte which mentions the source register into the stage 2
           * trampoline.
           */
          inl_tramp_st2 = inline_tramp_table + entry;
          inl_tramp_st2->rex[0] = byte[0];
          inl_tramp_st2->src_reg[0] = byte[2];

          /* Setup the stage 1 trampoline. Calculate the displacement to
           * the stage 2 trampoline from the next instruction.
           *
           * REMEMBER!!!! The next instruction will be NOP after our stage 1
           * trampoline is written. This is 5 bytes into the structure, even
           * though the original instruction we overwrote was 7 bytes.
           */
          tramp.displacement = (uint32_t)((void *)(inl_tramp_st2) - (void *)(byte+5));

          /* Figure out what page the stage 1 tramp is gonna be written to, mark
           * it WRITE, write the trampoline in, and then remove WRITE permission.
           */
          aligned_addr = (void*)(((long)byte)&~(0xffff));
          mprotect(aligned_addr, (((void *)byte) - aligned_addr) + 10, PROT_READ|PROT_WRITE|PROT_EXEC);
          memcpy(byte, &tramp, sizeof(struct tramp_inline));
          mprotect(aligned_addr, (((void *)byte) - aligned_addr) + 10, PROT_READ|PROT_EXEC);

          /* Finish setting up the stage 2 trampoline. */

          /* calculate the displacement to freelist from the next instruction.
           *
           * This is used to replicate the original instruction we overwrote.
           */
          inl_tramp_st2->mov_displacement = freelist - (void *)&(inl_tramp_st2->frame);

          /* fill in the displacement to freelist from the next instruction.
           *
           * This is to arrange for the new value in freelist to be in %rdi, and as such
           * be the first argument to the C handler. As per the amd64 ABI.
           */
          inl_tramp_st2->frame.rdi_source_displacement = freelist - (void *)&(inl_tramp_st2->frame.push_rbx);

          /* jmp back to the instruction after stage 1 trampoline was inserted 
           *
           * This can be 5 or 7, it doesn't matter. If its 5, we'll hit our 2
           * NOPS. If its 7, we'll land directly on the next instruction.
           */
          inl_tramp_st2->jmp_displacement = (uint32_t)((void *)(byte + 7) -
                                                       (void *)(inline_tramp_table + entry + 1));

          /* write the address of our C level trampoline in to the structure */
          inl_tramp_st2->frame.addr = freelist_tramp;

          /* track the new entry and new trampoline size */
          entry++;
          inline_tramp_size++;
        }
      }
    }

    if (count >= sizes[i]) {
        count = 0;
        i ++;
        byte = freelist_callers[i];
    }
    count++;
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
      inline_tramp_table[inline_tramp_size].frame.addr = tramp;
      inline_tramp_size++;
      hook_freelist(inline_ent);
    } else {
      return;
    }
  } else {
    tramp_table[tramp_size].addr = tramp;
    tramp_size++;
    bin_update_image(entry, trampee_addr);
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

#if defined(HAVE_MACH)
  insert_tramp("_rb_newobj", newobj_tramp);
#elif defined(HAVE_ELF)
  insert_tramp("rb_newobj", newobj_tramp);
  insert_tramp("add_freelist", freelist_tramp);
#endif

  rb_classname = bin_find_symbol("classname", 0);

  if (getenv("MEMPROF"))
    track_objs = 1;

  return;
}
