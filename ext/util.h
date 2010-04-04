#if !defined(__util_h__)
#define __util_h__

#if defined(_MEMPROF_DEBUG)
#include <stdio.h>
#define dbg_printf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dbg_printf(...)
#endif

#define ASSERT_ON_COMPILE(pred) \
      switch(0){case 0:case pred:;}

struct memprof_config {
  void *gc_sweep;
  size_t gc_sweep_size;

  void *finalize_list;
  size_t finalize_list_size;

  void *rb_gc_force_recycle;
  size_t rb_gc_force_recycle_size;

  void *freelist;
  void *classname;
  void *add_freelist;
  void *timeofday;

  void *rb_mark_table_add_filename;

  void *bm_mark;
  void *blk_free;
  void *thread_mark;

  void *heaps;
  void *heaps_used;
  void *finalizer_table;

  size_t sizeof_RVALUE;
  size_t sizeof_heaps_slot;

  size_t offset_heaps_slot_limit;
  size_t offset_heaps_slot_slot;

  size_t offset_BLOCK_body;
  size_t offset_BLOCK_var;
  size_t offset_BLOCK_cref;
  size_t offset_BLOCK_prev;
  size_t offset_BLOCK_self;
  size_t offset_BLOCK_klass;
  size_t offset_BLOCK_wrapper;
  size_t offset_BLOCK_orig_thread;
  size_t offset_BLOCK_block_obj;
  size_t offset_BLOCK_scope;
  size_t offset_BLOCK_dyna_vars;

  size_t offset_METHOD_klass;
  size_t offset_METHOD_rklass;
  size_t offset_METHOD_recv;
  size_t offset_METHOD_id;
  size_t offset_METHOD_oid;
  size_t offset_METHOD_body;

  size_t pagesize;
};

/* This is the CRC function used by GNU. Stripped executables may contain a
 * section .gnu_debuglink which holds the name of an elf object with debug
 * information and a checksum.
 */
unsigned long
gnu_debuglink_crc32 (unsigned long crc, unsigned char *buf, size_t len);

#endif
