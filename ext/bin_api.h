#if !defined(BIN_API__)
#define BIN_API__
#include <stddef.h>
#include <stdint.h>

/* generic file format stuff */
extern void *text_segment;
extern unsigned long text_segment_len;
extern size_t pagesize;

/*
 *  trampoline specific stuff
 */
extern struct tramp_tbl_entry *tramp_table;
extern size_t tramp_size;

/*
 *  inline trampoline specific stuff
 */
extern size_t inline_tramp_size;
extern struct inline_tramp_tbl_entry *inline_tramp_table;

/* trampoline types */
struct tramp_inline {
  unsigned char jmp[1];
  uint32_t displacement;
  unsigned char pad[2];
} __attribute__((__packed__));

struct tramp_tbl_entry {
  unsigned char rbx_save[1];
  unsigned char mov[2];
  void *addr;
  unsigned char callq[2];
  unsigned char rbx_restore[1];
  unsigned char ret[1];
} __attribute__((__packed__));

struct inline_tramp_tbl_entry {
  unsigned char rex[1];
  unsigned char mov[1];
  unsigned char src_reg[1];
  uint32_t mov_displacement;

  struct {
    unsigned char push_rdi[1];
    unsigned char mov_rdi[3];
    uint32_t rdi_source_displacement;
    unsigned char push_rbx[1];
    unsigned char push_rbp[1];
    unsigned char save_rsp[3];
    unsigned char align_rsp[4];
    unsigned char mov[2];
    void *addr;
    unsigned char callq[2];
    unsigned char leave[1];
    unsigned char rbx_restore[1];
    unsigned char rdi_restore[1];
  } __attribute__((__packed__)) frame;

  unsigned char jmp[1];
  uint32_t jmp_displacement;
} __attribute__((__packed__));

void
update_callqs(int entry, void *trampee_addr);

/*
 *  EXPORTED API.
 */
void
bin_init();

void *
bin_find_symbol(char *sym, size_t *size);

void
bin_update_image(int entry, void *trampee_addr);

void *
bin_allocate_page();

#endif
