#ifndef klox_trace_h
#define klox_trace_h

#include <libgen.h>
#include <stdio.h>
#include <string.h>


#if KLOX_TRACE_ENABLE

extern __thread bool on_main_thread;
extern __thread bool can_print;

#define KLOX_TRACE_(FMT, ARGS...) do { if (can_print) printf(FMT, ##ARGS); } while (0)

#define KLOX_TRACE_PREFIXED(FL, LINE, FUN, FMT, ARGS...) \
  do { \
    const char *__f = (FL); \
    const char *__basename = strrchr(__f, '/'); \
    __basename = (__basename ? __basename+1 : __f); \
    KLOX_TRACE_("TRACE %c %s:%d:%s() " FMT, (on_main_thread? 'M' : 'G'), __basename, LINE, FUN, ##ARGS); \
  } while(0)

#define KLOX_TRACE(FMT, ARGS...) \
    KLOX_TRACE_PREFIXED(__FILE__, __LINE__, __FUNCTION__, FMT, ##ARGS)

#define KLOX_TRACE_ONLY(X) X

#else

#define KLOX_TRACE_(FMT, ARGS...) do {} while(0)
#define KLOX_TRACE_PREFIXED(FL, LINE, FUN, FMT, ARGS...) do {} while(0)
#define KLOX_TRACE(FMT, ARGS...) do {} while(0)
#define KLOX_TRACE_ONLY(X)

#endif


#endif //klox_trace_h
