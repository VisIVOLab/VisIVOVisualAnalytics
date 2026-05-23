message(STATUS "Using libwcs v3.9.7")

add_library(wcs STATIC
        actread.c
        binread.c
        catutil.c
        cel.c
        ctgread.c
        daoread.c
        dateutil.c
        distort.c
        dsspos.c
        fileutil.c
        findstar.c
        fitsfile.c
        fitsfile.h
        fitshead.h
        fitswcs.c
        fitswcs.h
        gsc2read.c
        gscread.c
        hget.c
        hput.c
        iget.c
        imgetwcs.c
        imhfile.c
        imio.c
        imio.h
        imrotate.c
        imsetwcs.c
        imutil.c
        lin.c
        lwcs.h
        matchstar.c
        platefit.c
        platepos.c
        poly.c
        proj.c
        sdssread.c
        skybotread.c
        sortstar.c
        sph.c
        tabread.c
        tmcread.c
        tnxpos.c
        ty2read.c
        uacread.c
        ubcread.c
        ucacread.c
        ujcread.c
        wcs.c
        wcs.h
        wcscat.h
        wcscon.c
        wcsinit.c
        wcslib.c
        wcslib.h
        wcstrig.c
        webread.c
        worldpos.c
        zpxpos.c
)

set_target_properties(wcs PROPERTIES
        C_STANDARD 90
        AUTOMOC OFF
        AUTOUIC OFF
)

target_compile_definitions(wcs PRIVATE _FILE_OFFSET_BITS=64)

target_include_directories(wcs INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

# libwcs is POSIX-only C; on MSVC there's no <unistd.h>. Drop a shim that
# remaps the small set of POSIX I/O calls the sources use to their MSVC
# (<io.h> / <process.h>) underscore-prefixed equivalents. Only added to this
# target's include path so we don't pollute the rest of the project.
if (MSVC)
    target_include_directories(wcs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/win32")
    # Suppress noisy POSIX-deprecation warnings raised by the legacy C code.
    target_compile_definitions(wcs PRIVATE
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_DEPRECATE
            _WINSOCK_DEPRECATED_NO_WARNINGS
            # POSIX function names → MSVC underscore-prefixed equivalents.
            # Defined at the target level (not just inside win32/unistd.h) so
            # that sources which don't pull in <unistd.h> — notably wcscon.c —
            # still get the mapping. S_ISDIR/S_ISREG stay in the shim because
            # they need _S_IFMT from <sys/stat.h>.
            strcasecmp=_stricmp
            strncasecmp=_strnicmp
            ftruncate=_chsize
    )
    # webread.c uses BSD socket APIs that Windows exposes through Winsock2.
    target_link_libraries(wcs PRIVATE ws2_32)
endif ()
