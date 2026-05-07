/*
 * Windows MSVC stub for <sys/time.h>.
 *
 * Provides struct timeval and a gettimeofday() implementation built on
 * Windows' GetSystemTimeAsFileTime, which gives 100-ns precision since
 * 1601 — sufficient for libwcs's date-formatting helpers.
 */
#ifndef VISIVO_LIBWCS_WIN32_SYS_TIME_H
#define VISIVO_LIBWCS_WIN32_SYS_TIME_H

#include <winsock2.h>   /* defines struct timeval on Windows */
#include <windows.h>
#include <stdint.h>

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

/* Returns 0 always; populates tv with current wall-clock time. tz is filled
 * with zeros for compatibility — libwcs only inspects tv. */
static __inline int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    FILETIME ft;
    uint64_t t;

    if (tv) {
        GetSystemTimeAsFileTime(&ft);
        /* FILETIME = 100-ns ticks since 1601-01-01;
         * Unix epoch = 1970-01-01, offset = 11644473600 seconds. */
        t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        t -= 116444736000000000ULL;
        tv->tv_sec  = (long)(t / 10000000ULL);
        tv->tv_usec = (long)((t % 10000000ULL) / 10);
    }
    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime     = 0;
    }
    return 0;
}

#endif /* VISIVO_LIBWCS_WIN32_SYS_TIME_H */
