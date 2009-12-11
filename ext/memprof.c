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
memprof_tabulate(st_data_t key, st_data_t record, st_data_t arg)
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
        type = rb_obj_classname(tracker->obj);
      } else {
        type = "__unknown__";
      }
  }

  asprintf(&source_key, "%s:%d:%s", tracker->source, tracker->line, type);
  st_lookup(table, (st_data_t)source_key, (st_data_t *)&count);
  if (st_insert(table, (st_data_t)source_key, ++count)) {
    free(source_key);
  }

  free(tracker->source);
  return ST_DELETE;
}

struct results {
  char **entries;
  unsigned long num_entries;
};

static int
memprof_do_dump(st_data_t key, st_data_t record, st_data_t arg)
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
memprof_dump(VALUE self)
{
  st_table *tmp_table;
  struct results res;
  int i;

  track_objs = 0;

  tmp_table = st_init_strtable();
  st_foreach(objs, memprof_tabulate, (st_data_t)tmp_table);

  res.num_entries = 0;
  res.entries = malloc(sizeof(char*) * tmp_table->num_entries);

  st_foreach(tmp_table, memprof_do_dump, (st_data_t)&res);
  st_free_table(tmp_table);

  qsort(res.entries, res.num_entries, sizeof(char*), &memprof_strcmp);
  for (i=0; i < res.num_entries; i++) {
    printf("%s\n", res.entries[i]);
    free(res.entries[i]);
  }
  free(res.entries);

  track_objs = 1;
  return Qnil;
}

static void
create_tramp_table()
{
  int i, j = 0;

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

  if ((tramp_table = bin_allocate_page()) == MAP_FAILED) {
    fprintf(stderr, "Failed to allocate memory for stage 1 trampoline.\n");
    return;
  }

  if ((inline_tramp_table = bin_allocate_page()) == MAP_FAILED) {
    fprintf(stderr, "Faied to allocate memory for the stage 1 inline trampoline.\n");
    return;
  }

  for (j = 0; j < pagesize/sizeof(struct tramp_tbl_entry); j ++ ) {
    memcpy(tramp_table + j, &ent, sizeof(struct tramp_tbl_entry));
  }

  for (j = 0; j < pagesize/sizeof(struct inline_tramp_tbl_entry); j++) {
    memcpy(inline_tramp_table + j, &inline_ent, sizeof(struct inline_tramp_tbl_entry));
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
  rb_define_singleton_method(memprof, "start", memprof_start, 0);
  rb_define_singleton_method(memprof, "stop", memprof_stop, 0);
  rb_define_singleton_method(memprof, "dump", memprof_dump, 0);

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
