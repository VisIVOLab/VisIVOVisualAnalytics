/*
 * Windows MSVC shim for <unistd.h>.
 *
 * libwcs (WCSTools) is POSIX C; the original sources include <unistd.h>
 * unconditionally. MSVC has no such header, but the same low-level POSIX I/O
 * functions are provided in <io.h> and <process.h> with leading-underscore
 * names. This shim maps the names libwcs uses back to their MSVC equivalents
 * and defines a few missing access-mode constants. Only used when this file's
 * directory is on the include path (set conditionally for MSVC in the libwcs
 * CMakeLists).
 */
#ifndef VISIVO_LIBWCS_WIN32_UNISTD_H
#define VISIVO_LIBWCS_WIN32_UNISTD_H

#include <io.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>      /* MSVC's _stricmp / _strnicmp live here */
#include <sys/types.h>
#include <sys/stat.h>    /* _S_IFMT / _S_IFDIR for the S_ISDIR shim below */

/* access() mode flags (POSIX defines these in <unistd.h>; MSVC omits them). */
#ifndef R_OK
#  define R_OK 4
#endif
#ifndef W_OK
#  define W_OK 2
#endif
#ifndef F_OK
#  define F_OK 0
#endif
#ifndef X_OK
#  define X_OK 0   /* MSVC's _access has no execute mode; treat as F_OK. */
#endif

/* Standard file descriptors. */
#ifndef STDIN_FILENO
#  define STDIN_FILENO  0
#endif
#ifndef STDOUT_FILENO
#  define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#  define STDERR_FILENO 2
#endif

/* Map POSIX names to MSVC's underscore-prefixed equivalents. */
#define access     _access
#define close      _close
#define dup        _dup
#define dup2       _dup2
#define fileno     _fileno
#define isatty     _isatty
#define lseek      _lseek
#define read       _read
#define unlink     _unlink
#define write      _write
#define getpid     _getpid
#ifndef ftruncate
#  define ftruncate _chsize   /* _chsize(fd, long size) — compatible for libwcs */
#endif

/* POSIX <strings.h> case-insensitive compares. MSVC keeps them in <string.h>
 * with an underscore prefix. */
#ifndef strcasecmp
#  define strcasecmp  _stricmp
#endif
#ifndef strncasecmp
#  define strncasecmp _strnicmp
#endif

/* POSIX file-type test macro. MSVC's <sys/stat.h> defines _S_IFMT/_S_IFDIR
 * but not the S_IS* helpers; supply the standard expansion. */
#ifndef S_ISDIR
#  define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#  define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

/* MSVC declares ssize_t in BaseTsd.h as SSIZE_T; libwcs's tiny use of it
 * just needs a signed integer wide enough for a byte count. */
#ifndef _SSIZE_T_DEFINED
#  include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#  define _SSIZE_T_DEFINED
#endif

#endif /* VISIVO_LIBWCS_WIN32_UNISTD_H */
