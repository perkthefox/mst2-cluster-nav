/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * SAL's connectivity log copied into the mod's log, info level and up: what Android Auto's
 * receiver, AOA and SAL's phone code report during a session. The unit prints only errors, and
 * only to a console no car owner can read; the mod's log reaches GEM "Save diagnostics", and
 * while the mod runs it is kept on the unit's storage across a reset (MCN_PLOG).
 *
 * tsd::connectivity::common::logging::ConnCommonLogger (libtsd.connectivity.common.so) has
 * three ends, each reached through the PLT, so the definitions here take every call: log(level,
 * string) and logCStringWithFuncPrefix(level, prefix, text), where every wrapper ends (info(),
 * error(string, char const*), logWithFuncPrefix() ...: 0x14518, 0x13fcc, 0x140e8, JUMP_SLOTs
 * 0x23df8 and 0x23ed0, ev_6aa546c8, ev_2ebcdee7), and log_fmt(level, prefix, format, ...), the
 * only one the receiver library imports (ev_cc1954e0). log_fmt asks Logger::isEnabledFor
 * (this + 4, &level) first and does nothing for a disabled level; otherwise it formats into 512
 * bytes and calls logCStringWithFuncPrefix through the PLT (0x14044, ev_9fb85c6d, ev_7f439138).
 * Here the text is formatted the same way; an enabled level goes on to the real log_fmt as "%s"
 * (what the unit prints is unchanged) and is copied by the logCStringWithFuncPrefix definition
 * below, a disabled one is copied here.
 * Levels: trace 1, debug 2, info 3, warn 4, error 5, fatal 6 (ev_0cf29a5f).
 *
 * The real functions are found with dlsym, not mcn_hook_enter: logging starts during SAL's
 * static construction, before the worker may be started, and a failed lookup only loses lines.
 * Nothing is formatted beyond what the unit itself formats, and nothing is copied, unless the mod
 * is on: mcn_active, cached for 2 s, asked here too, so that a running mod has the connection from
 * SAL's start (the first log line also starts SAL's crash guard, at once, not at the first hook).
 */
#include "sal.h"

#define CONN_LIB "libtsd.connectivity.common.so"
#define LOGGER "_ZN3tsd12connectivity6common7logging16ConnCommonLogger"
#define LEVEL "ENS_6common7logging8LogLevel8logLevelE"
#define LOG_STR LOGGER "3log" LEVEL "RKSs"
#define LOG_CSTR LOGGER "24logCStringWithFuncPrefix" LEVEL "RKSsPKc"
#define LOG_FMT LOGGER "7log_fmt" LEVEL "RKSsPKcz"
#define ENABLED "_ZNK3tsd6common7logging6Logger12isEnabledForERKNS1_8LogLevel8logLevelE"
#define COMMON_LIB "libtsd.common.so"
#define INFO 3

typedef void (*log_str_fn)(void *self, int level, const void *text);
typedef void (*log_cstr_fn)(void *self, int level, const void *prefix, const char *text);
typedef void (*log_fmt_fn)(void *self, int level, const void *prefix, const char *fmt, ...);
typedef int (*enabled_fn)(const void *logger, const int *level);

static void *real_in(const char *library, void **cache, const char *name)
{
    void *lib;

    if (!*cache) {
        lib = dlopen(library, RTLD_LAZY | RTLD_GLOBAL);
        *cache = lib ? dlsym(lib, name) : 0;
    }
    return *cache;
}

static void *real(void **cache, const char *name)
{
    return real_in(CONN_LIB, cache, name);
}

/* The characters of a libstdc++ (gcc 4.4, reference-counted) std::string: its only member. */
static const char *chars(const void *str)
{
    const char *p = str ? *(const char *const *)str : 0;

    return p ? p : "";
}

static void capture(int level, const char *prefix, const char *text)
{
    char head[64];

    if (level < INFO || level > 6 || !mcn_active()) {
        return;
    }
    snprintf(head, sizeof(head), "unit L%d %s%s", level, prefix, *prefix ? ": " : "");
    mcn_capture(head, text);
}

MCN_EXPORT void _ZN3tsd12connectivity6common7logging16ConnCommonLogger3logENS_6common7logging8LogLevel8logLevelERKSs(
    void *self, int level, const void *text)
{
    static void *fn;
    log_str_fn f = (log_str_fn)real(&fn, LOG_STR);

    if (f) {
        f(self, level, text);
    }
    capture(level, "", chars(text));
}

MCN_EXPORT void _ZN3tsd12connectivity6common7logging16ConnCommonLogger24logCStringWithFuncPrefixENS_6common7logging8LogLevel8logLevelERKSsPKc(
    void *self, int level, const void *prefix, const char *text)
{
    static void *fn;
    log_cstr_fn f = (log_cstr_fn)real(&fn, LOG_CSTR);

    if (f) {
        f(self, level, prefix, text);
    }
    capture(level, chars(prefix), text ? text : "");
}

MCN_EXPORT void _ZN3tsd12connectivity6common7logging16ConnCommonLogger7log_fmtENS_6common7logging8LogLevel8logLevelERKSsPKcz(
    void *self, int level, const void *prefix, const char *fmt, ...)
{
    static void *fn, *en;
    log_fmt_fn f = (log_fmt_fn)real(&fn, LOG_FMT);
    enabled_fn enabled = (enabled_fn)real_in(COMMON_LIB, &en, ENABLED);
    char text[0x200];
    __builtin_va_list ap;

    if (f && enabled && enabled((const uint8_t *)self + 4, &level)) {
        __builtin_va_start(ap, fmt);
        vsnprintf(text, sizeof(text), fmt, ap);
        __builtin_va_end(ap);
        f(self, level, prefix, "%s", text);
        return;
    }
    /* A level the unit does not log is formatted only for a running mod: stock never formats
     * it, and its arguments were never exercised there. */
    if (level < INFO || !fmt || !mcn_active()) {
        return;
    }
    __builtin_va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    __builtin_va_end(ap);
    capture(level, chars(prefix), text);
}
