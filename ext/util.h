#if !defined(__util_h__)
#define __util_h__

#if defined(_MEMPROF_DEBUG)
#define dbg_printf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dbg_printf(...)
#endif

#define ASSERT_ON_COMPILE(pred) \
      switch(0){case 0:case pred:;}

#endif
