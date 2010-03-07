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

#define FREELIST_INLINES 3

size_t pagesize;

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

void
create_tramp_table()
{
  int i;
  void *region, *ent, *inline_ent;
  size_t tramp_sz = 0, inline_tramp_sz = 0;

  ent = arch_get_st2_tramp(&tramp_sz);
  inline_ent = arch_get_inline_st2_tramp(&inline_tramp_sz);
  assert(ent && inline_ent);

  region = bin_allocate_page();
  if (region == MAP_FAILED)
    errx(EX_SOFTWARE, "Failed to allocate memory for stage 1 trampolines.");

  tramp_table = region;
  inline_tramp_table = region + pagesize / 2;

  for (i = 0; i < (pagesize / 2) / tramp_sz; i++) {
    memcpy(tramp_table + i, ent, tramp_sz);
  }

  for (i = 0; i < (pagesize / 2) / inline_tramp_sz; i++) {
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

  freelist_inliners[0] = bin_find_symbol("gc_sweep", &sizes[0]);
  /*
   * Sometimes gc_sweep gets inlined in garbage_collect
   * (e.g., on REE it gets inlined into garbage_collect_0).
   */
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
