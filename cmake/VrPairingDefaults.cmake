# cmake/VrPairingDefaults.cmake
# The headset pairing service and public loader a desktop build offers by
# default (issue #344): Settings ▸ Backends ▸ Headset Pairing shows them, and
# the backend the desktop starts is given them while the fields are empty. They
# are defined here and nowhere else, so a site with its own server builds with
#
#   cmake -DVISIVO_DEFAULT_VR_PAIR_URL=wss://vr.example.org/pair
#         -DVISIVO_DEFAULT_VR_PAGE_URL=https://vr.example.org/visivo/
#
# Empty values build a desktop with no default: pairing then needs the fields.
# The backend has no built-in host either way: one started by hand or on a
# cluster is configured by its own environment.
#
# Included by the top-level build and by the standalone test build; the header
# goes to the top of the build tree, which both put on the include path.
include_guard(GLOBAL)

set(VISIVO_DEFAULT_VR_PAIR_URL "wss://vr.visivolab.oact.inaf.it/pair"
    CACHE STRING "Headset pairing service the desktop offers by default (empty: none)")
set(VISIVO_DEFAULT_VR_PAGE_URL "https://vr.visivolab.oact.inaf.it/"
    CACHE STRING "Public headset loader the desktop offers by default, with its trailing / (empty: none)")

# The values land inside C++ string literals. An address never needs a quote,
# a backslash, a space or a control character, so refuse them instead of
# escaping them.
foreach(_vr_var VISIVO_DEFAULT_VR_PAIR_URL VISIVO_DEFAULT_VR_PAGE_URL)
    if("${${_vr_var}}" MATCHES "[\"\\\\ \t\r\n]")
        message(FATAL_ERROR "${_vr_var} contains a quote, a backslash or white space: "
                            "'${${_vr_var}}'")
    endif()
endforeach()

configure_file("${CMAKE_CURRENT_LIST_DIR}/../src/app/VrPairingDefaults.h.in"
               "${CMAKE_BINARY_DIR}/VrPairingDefaults.h" @ONLY)
