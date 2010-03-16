#include <ruby.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sysexits.h>
#include <sys/mman.h>
#include <err.h>
#include <assert.h>

#include "arch.h"
#include "bin_api.h"
#include "util.h"

#define FREELIST_INLINES 3

/*
 * trampoline specific stuff
 */
static struct tramp_st2_entry *tramp_table;
static size_t tramp_size;

/*
 * inline trampoline specific stuff
 */
static size_t inline_tramp_size;
static struct inline_tramp_st2_entry *inline_tramp_table;

extern struct memprof_config memprof_config;

void
create_tramp_table()
{
  size_t i;
  void *region, *ent, *inline_ent;
  size_t tramp_sz = 0, inline_tramp_sz = 0;

  ent = arch_get_st2_tramp(&tramp_sz);
  inline_ent = arch_get_inline_st2_tramp(&inline_tramp_sz);
  assert(ent && inline_ent);

  region = bin_allocate_page();
  if (region == MAP_FAILED)
    errx(EX_SOFTWARE, "Failed to allocate memory for stage 1 trampolines.");

  tramp_table = region;
  inline_tramp_table = region + memprof_config.pagesize / 2;

  for (i = 0; i < (memprof_config.pagesize / 2) / tramp_sz; i++) {
    memcpy(tramp_table + i, ent, tramp_sz);
  }

  for (i = 0; i < (memprof_config.pagesize / 2) / inline_tramp_sz; i++) {
    memcpy(inline_tramp_table + i, inline_ent, inline_tramp_sz);
  }
}

static void
hook_freelist(int entry, void *tramp)
{
  size_t sizes[FREELIST_INLINES];
  void *freelist_inliners[FREELIST_INLINES];
  void *freelist = NULL;
  unsigned char *byte = NULL;
  int i = 0, tramps_completed = 0;

  assert(memprof_config.gc_sweep != NULL);
  assert(memprof_config.finalize_list != NULL);
  assert(memprof_config.rb_gc_force_recycle != NULL);
  assert(memprof_config.freelist != NULL);
  assert(memprof_config.gc_sweep_size > 0);
  assert(memprof_config.finalize_list_size > 0);
  assert(memprof_config.rb_gc_force_recycle_size > 0);

  freelist_inliners[0] = memprof_config.gc_sweep;
  freelist_inliners[1] = memprof_config.finalize_list;
  freelist_inliners[2] = memprof_config.rb_gc_force_recycle;
  sizes[0] = memprof_config.gc_sweep_size;
  sizes[1] = memprof_config.finalize_list_size;
  sizes[2] = memprof_config.rb_gc_force_recycle_size;

  freelist = memprof_config.freelist;

  /* start the search for places to insert the inline tramp */
  byte = freelist_inliners[i];

  while (i < FREELIST_INLINES) {
    if (arch_insert_inline_st2_tramp(byte, freelist, tramp,
        &inline_tramp_table[entry]) == 0) {
      /* insert occurred, so increment internal counters for the tramp table */
      entry++;
      inline_tramp_size++;

      /*
       * add_freelist() only gets inlined *ONCE* into any of the 3 functions
       * that we're scanning, so move on to the next 'inliner' after we
       * tramp the first instruction we find.  REE's gc_sweep has 2 calls,
       * but this gets optimized into a single inlining and a jmp to it.
       * older patchlevels of 1.8.7 don't have an add_freelist(), but the
       * instruction should be the same.
       */
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
    errx(EX_SOFTWARE, "Inline add_freelist tramp insertion failed! "
         "Only inserted %d tramps.", tramps_completed);
}

void
insert_tramp(const char *trampee, void *tramp)
{
  void *trampee_addr = bin_find_symbol(trampee, NULL);
  int inline_ent = inline_tramp_size;

  if (trampee_addr == NULL) {
    if (strcmp("add_freelist", trampee) == 0) {
      /* XXX super hack */
      inline_tramp_size++;
      hook_freelist(inline_ent, tramp /* freelist_tramp() */);
    } else {
      errx(EX_SOFTWARE, "Failed to locate required symbol %s", trampee);
    }
  } else {
    tramp_table[tramp_size].addr = tramp;
    if (bin_update_image(trampee, &tramp_table[tramp_size]) != 0)
      errx(EX_SOFTWARE, "Failed to insert tramp for %s", trampee);
    tramp_size++;
  }
}
