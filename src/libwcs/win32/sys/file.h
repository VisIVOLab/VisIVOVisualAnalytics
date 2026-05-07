/*
 * Windows MSVC stub for <sys/file.h>.
 *
 * libwcs's binread.c includes <sys/file.h> for file-locking constants and
 * flock(2). The actual symbols are never referenced by the library, so an
 * empty stub satisfies the include and lets the build proceed.
 */
#ifndef VISIVO_LIBWCS_WIN32_SYS_FILE_H
#define VISIVO_LIBWCS_WIN32_SYS_FILE_H

/* Intentionally empty — kept as a placeholder so #include <sys/file.h>
 * resolves on MSVC. If a future libwcs source genuinely calls flock(),
 * declare a no-op shim here. */

#endif /* VISIVO_LIBWCS_WIN32_SYS_FILE_H */
