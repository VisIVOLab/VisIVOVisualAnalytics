cmake_minimum_required(VERSION 3.22)

# ── Oldest macOS this build will run on ──────────────────────────────────────
#
# MUST be set before project(): CMake reads it while probing the compiler, and
# setting it afterwards leaves the already-probed flags alone.
#
# Left unset, CMake stamps the binary with the version of the machine that
# COMPILED it, and macOS refuses to launch it on anything older — "you can't
# open this application with this version of macOS", with no hint that the
# cause is the builder's OS rather than the user's. A build made on a machine
# that had just been upgraded therefore stopped working for every colleague who
# had not upgraded, which is exactly what happened.
#
# 13.0 is Qt 6.11's own floor (its frameworks are stamped minos 13.0), so
# asking for less would not buy anything: the app would still refuse to load Qt.
# NOT DEFINED is not enough: an existing build directory carries the variable as
# an EMPTY cache entry, which is defined. FORCE is safe under this condition,
# which is precisely "nobody asked for a target"; -DCMAKE_OSX_DEPLOYMENT_TARGET
# on the command line makes it false and is left alone.
if (APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING
        "Oldest macOS supported by this build (Qt 6.11's floor)" FORCE)
endif ()


project(VisIVOVisualAnalytics
        VERSION 1.0.7
        LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

message(STATUS "Configuring ${PROJECT_NAME} v${PROJECT_VERSION}")

set(qt_components
        Concurrent
        Widgets
        Xml
        Svg
        Network
        NetworkAuth
        PrintSupport
        WebEngineWidgets
        WebSockets
        Xml
)
foreach (module IN LISTS qt_components)
    list(APPEND qt_modules "Qt::${module}")
endforeach ()

find_package(Qt6 6.10.0 CONFIG REQUIRED COMPONENTS ${qt_components})
message(STATUS "Using Qt v${Qt6_VERSION}")

# VR opt-in flag is declared up here (instead of next to its use site below) so
# we can resolve `OpenXR::openxr_loader` BEFORE find_package(VTK). VTK ≥ 9.6
# built with RenderingOpenXR encodes that target in its link interface, and
# CMake fails the include if no `find_package(OpenXR)` has registered it yet.
option(VISIVO_ENABLE_VR
    "Enable VR (OpenXR) rendering for the cube viewer (requires VTK with RenderingOpenXR module)"
    OFF)
if (VISIVO_ENABLE_VR)
    # QUIET: if VTK happens to not have OpenXR after all we still want a clean
    # configure — the later block downgrades to a non-VR build with a WARNING.
    find_package(OpenXR CONFIG QUIET)
    if (OpenXR_FOUND)
        message(STATUS "Found OpenXR loader for VR build")
    endif ()
endif ()

find_package(VTK 9.5.0 CONFIG REQUIRED COMPONENTS
        CommonColor
        FiltersFlowPaths
        FiltersModeling
        FiltersStatistics
        GUISupportQt
        ImagingGeneral
        InteractionStyle
        InteractionWidgets
        RenderingAnnotation
        RenderingImage
        RenderingVolumeOpenGL2
)
message(STATUS "Using VTK v${VTK_VERSION}")

# CFITSIO is shipped under two different CMake package names in the wild:
#   * "cfitsio"            — upstream / Homebrew / conda / source build
#                            (exports target CFITSIO::cfitsio)
#   * "unofficial-cfitsio" — vcpkg's port (since cfitsio's own build doesn't
#                            install a CMake config file). Across vcpkg
#                            versions the actual target name has been either
#                            `unofficial::cfitsio::cfitsio` (old) or a bare
#                            `cfitsio` (current), so we try both.
# The rest of the project links against CFITSIO::cfitsio — we synthesise that
# as an INTERFACE alias whenever the lookup falls back to the vcpkg port.
find_package(cfitsio CONFIG QUIET)
if (cfitsio_FOUND AND TARGET CFITSIO::cfitsio)
    message(STATUS "Using cfitsio v${cfitsio_VERSION} (upstream CMake config)")
else ()
    find_package(unofficial-cfitsio CONFIG REQUIRED)
    if (TARGET unofficial::cfitsio::cfitsio)
        set(_cfitsio_vcpkg_target unofficial::cfitsio::cfitsio)
    elseif (TARGET cfitsio)
        set(_cfitsio_vcpkg_target cfitsio)
    else ()
        message(FATAL_ERROR
            "find_package(unofficial-cfitsio) succeeded but neither "
            "unofficial::cfitsio::cfitsio nor a bare 'cfitsio' target was "
            "defined — vcpkg port layout may have changed.")
    endif ()
    message(STATUS "Using cfitsio (vcpkg port, target: ${_cfitsio_vcpkg_target})")
    if (NOT TARGET CFITSIO::cfitsio)
        add_library(CFITSIO::cfitsio INTERFACE IMPORTED)
        target_link_libraries(CFITSIO::cfitsio INTERFACE ${_cfitsio_vcpkg_target})
    endif ()
endif ()

# An SDK's own /usr/include must never be handed to the compiler as an include
# directory: the C <math.h> found that way defines isfinite/isnan as MACROS
# after <cmath> has undefined them, and every std::isfinite in the codebase
# stops compiling ("expected unqualified-id") in headers nobody has touched.
#
# This used to be a one-target, one-path workaround (cfitsio, Command Line
# Tools). An Xcode upgrade showed both halves were too narrow: the path moved to
# /Applications/Xcode.app/.../SDKs/MacOSX.sdk/usr/include, and the target that
# carried it was Qt6::PrintSupport (it wants the SDK's CUPS headers, and pins
# the SDK it was built against). Strip it wherever it appears: the sysroot
# already provides those headers through the proper mechanism.
function(visivo_strip_sdk_usr_include target)
    if (NOT TARGET ${target})
        return()
    endif ()
    get_target_property(_inc ${target} INTERFACE_INCLUDE_DIRECTORIES)
    if (NOT _inc)
        return()
    endif ()
    set(_kept "${_inc}")
    list(FILTER _kept EXCLUDE REGEX "/SDKs/[^/]*\\.sdk/usr/include$")
    if (NOT "${_kept}" STREQUAL "${_inc}")
        set_target_properties(${target} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_kept}")
        message(STATUS "Stripped an SDK /usr/include from ${target}")
    endif ()
endfunction()

visivo_strip_sdk_usr_include(CFITSIO::cfitsio)
# Cups::Cups is where it hides on macOS: Qt6::PrintSupport links it, and its
# headers live in the SDK's /usr/include, so the whole directory came along.
visivo_strip_sdk_usr_include(Cups::Cups)
foreach (_qt_comp IN LISTS qt_components)
    visivo_strip_sdk_usr_include(Qt6::${_qt_comp})
endforeach ()

# ── Phase 3 SSR transport: H.265 / WebRTC via libdatachannel ────────────────
#
# When VISIVO_ENABLE_WEBRTC is ON (default), build the
# ``RemoteRenderWidget`` with the WebRTC transport in addition to the
# legacy JPEG-over-WebSocket path. Requires:
#
#   * libdatachannel  — WebRTC peer connection + DataChannel (C++17,
#                       MIT, ~150 KB). On Linux: ``apt install
#                       libdatachannel-dev``. On macOS: ``brew install
#                       libdatachannel``. On Windows: vcpkg port
#                       ``datachannel``.
#   * FFmpeg dev libs — libavcodec + libavutil + libswscale for the
#                       H.264 decode path. Same provenance pattern as
#                       libdatachannel above (system / brew / vcpkg).
#
# Both find_package paths are gated by the option so a developer who
# only needs the JPEG transport (e.g. a Windows lab without FFmpeg
# headers) can disable the feature at configure time without touching
# the source.
option(VISIVO_ENABLE_WEBRTC
    "Enable H.265 WebRTC transport in RemoteRenderWidget (requires libdatachannel + FFmpeg)"
    ON)
if (VISIVO_ENABLE_WEBRTC)
    # Apple Silicon Homebrew installs to /opt/homebrew, which CMake
    # does NOT search by default. Add brew's prefix to the search path
    # so find_package sees the formula's CMake configs (libdatachannel,
    # plus its OpenSSL/SRTP/usrsctp deps the config transitively links).
    # Mirror the same treatment for legacy Intel /usr/local. Linux/CI
    # and vcpkg are unaffected — they already register their prefixes.
    if (APPLE)
        foreach (_brew_prefix /opt/homebrew /usr/local)
            if (EXISTS "${_brew_prefix}/lib/cmake")
                list(APPEND CMAKE_PREFIX_PATH "${_brew_prefix}")
            endif ()
        endforeach ()
        message(STATUS "CMAKE_PREFIX_PATH augmented for brew lookup: "
                "${CMAKE_PREFIX_PATH}")
    endif ()

    # libdatachannel is NOT in Homebrew core and only some vcpkg
    # ports ship it. To keep developer setup zero-friction across
    # macOS/Windows/Linux we ALWAYS try find_package first (catches
    # the rare case it's pre-installed) and fall back to FetchContent
    # — CMake clones the repo and builds it as part of our configure
    # step. First configure is ~5-10 min slower; subsequent ones use
    # the FetchContent cache.
    # OpenSSL hint — libdatachannel's DTLS-SRTP needs real OpenSSL.
    # macOS system OpenSSL is actually LibreSSL with a reduced API
    # which fails libdatachannel's compile. Point find_package(OpenSSL)
    # at brew's openssl@3 install if present.
    if (APPLE AND NOT OPENSSL_ROOT_DIR)
        foreach (_ssl_root
                "/opt/homebrew/opt/openssl@3"
                "/usr/local/opt/openssl@3"
        )
            if (EXISTS "${_ssl_root}/include/openssl/ssl.h")
                set(OPENSSL_ROOT_DIR "${_ssl_root}")
                message(STATUS "OpenSSL hint: OPENSSL_ROOT_DIR=${_ssl_root}")
                break ()
            endif ()
        endforeach ()
        if (NOT OPENSSL_ROOT_DIR)
            message(WARNING
                "VISIVO_ENABLE_WEBRTC=ON on macOS but no OpenSSL found "
                "at /opt/homebrew/opt/openssl@3 or /usr/local/opt/openssl@3. "
                "Install with: brew install openssl@3. "
                "WebRTC build will likely fail until that's resolved.")
        endif ()
    endif ()

    find_package(LibDataChannel CONFIG QUIET)
    if (TARGET LibDataChannel::LibDataChannel)
        set(_visivo_webrtc_lib LibDataChannel::LibDataChannel)
        message(STATUS "Using libdatachannel from CMAKE_PREFIX_PATH "
                "(target: LibDataChannel::LibDataChannel)")
    else ()
        message(STATUS "libdatachannel not installed system-wide — "
                "fetching from upstream via FetchContent")
        include(FetchContent)
        # Trim libdatachannel's own build: we only need the C++ core
        # for our peer connection / track / data channel use; their
        # tests, examples, GnuTLS adapter, etc. add ~3 min and are
        # useless for a library consumer.
        set(NO_TESTS    ON CACHE BOOL "")
        set(NO_EXAMPLES ON CACHE BOOL "")
        set(NO_MEDIA    OFF CACHE BOOL "")    # we DO need media (RTP) for the H.264 track
        set(NO_WEBSOCKET ON CACHE BOOL "")    # we already have QWebSocket for the JPEG path
        set(USE_GNUTLS  OFF CACHE BOOL "")    # OpenSSL is universal across our platforms
        set(USE_NICE    OFF CACHE BOOL "")    # libnice is a Linux-only ICE library; not needed
        # Force ALL deps to come from libdatachannel's bundled
        # submodules (plog, libjuice, libsrtp, usrsctp). On the
        # initial attempt PREFER_SYSTEM_LIB=ON made libdatachannel
        # call find_package(plog) which fails on macOS — plog isn't
        # in Homebrew. The submodules are already in the libdatachannel
        # repo we just cloned, so bundling them is the path of least
        # resistance.
        set(USE_SYSTEM_PLOG     OFF CACHE BOOL "" FORCE)
        set(USE_SYSTEM_JUICE    OFF CACHE BOOL "" FORCE)
        set(USE_SYSTEM_SRTP     OFF CACHE BOOL "" FORCE)
        set(USE_SYSTEM_USRSCTP  OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(libdatachannel
            GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
            GIT_TAG        v0.21.2     # last release tested 2025-12, stable WebRTC support
            # NB: NO GIT_SHALLOW here. Shallow clones don't pull
            # submodules cleanly across CMake/git versions; the
            # full clone is ~80 MB extra but guarantees deps/* are
            # populated.
            GIT_SUBMODULES_RECURSE TRUE
        )
        # libdatachannel v0.21.2 bundles plog, whose CMakeLists still declares
        # cmake_minimum_required(VERSION 3.2). CMake 4 REMOVED compatibility with
        # anything below 3.5 and errors out, which is what broke the macOS CI
        # runners the day they moved to CMake 4 while Linux and Windows were
        # still on 3.x. Scoped to this dependency and restored immediately after,
        # rather than passed as -DCMAKE_POLICY_VERSION_MINIMUM on the whole
        # configure: our own code should keep being held to current policies.
        set(_visivo_saved_policy_min "${CMAKE_POLICY_VERSION_MINIMUM}")
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
        FetchContent_MakeAvailable(libdatachannel)
        set(CMAKE_POLICY_VERSION_MINIMUM "${_visivo_saved_policy_min}")
        unset(_visivo_saved_policy_min)
        if (TARGET LibDataChannel::LibDataChannel)
            set(_visivo_webrtc_lib LibDataChannel::LibDataChannel)
        elseif (TARGET datachannel)
            set(_visivo_webrtc_lib datachannel)
        endif ()
        if (_visivo_webrtc_lib)
            message(STATUS "Using libdatachannel from FetchContent "
                    "(target: ${_visivo_webrtc_lib})")
        else ()
            message(WARNING "FetchContent of libdatachannel ran but no "
                    "expected target was created — check upstream layout.")
        endif ()
    endif ()

    # FFmpeg via pkg-config — the most portable path across Linux/macOS
    # and vcpkg+Windows (vcpkg ships ``.pc`` files in its install root).
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(VISIVO_FFMPEG IMPORTED_TARGET
                libavcodec libavutil libswscale)
    endif ()

    if (_visivo_webrtc_lib AND VISIVO_FFMPEG_FOUND)
        message(STATUS "Using FFmpeg via pkg-config: "
                "libavcodec ${VISIVO_FFMPEG_libavcodec_VERSION}, "
                "libswscale ${VISIVO_FFMPEG_libswscale_VERSION}")
        set(VISIVO_HAS_WEBRTC ON)
    else ()
        message(WARNING "VISIVO_ENABLE_WEBRTC=ON but dependencies missing: "
                "LibDataChannel=${_visivo_webrtc_lib}, "
                "FFmpeg=${VISIVO_FFMPEG_FOUND}. "
                "Building WITHOUT the WebRTC transport.")
        set(VISIVO_HAS_WEBRTC OFF)
    endif ()
else ()
    set(VISIVO_HAS_WEBRTC OFF)
    message(STATUS "VISIVO_ENABLE_WEBRTC=OFF — WebRTC transport not built.")
endif ()

qt_standard_project_setup()
set(CMAKE_AUTOUIC OFF)

configure_file(src/Version.h.in Version.h @ONLY)

# VLKB sign-in (OIDC authorization-code flow against the IA2 single sign-on,
# Settings ▸ VLKB). The endpoints and scope are public and are the defaults
# below, as in the legacy ViaLactea client. The client credentials are never
# committed: pass them at configure time, as the release workflow does from the
# repository's secrets. Without a client id the build has no VLKB sign-in and
# the Settings dialog says so.
set(VISIVO_VLKB_AUTH_URL "https://sso.ia2.inaf.it/rap-ia2/auth/oauth2/authorize"
    CACHE STRING "VLKB OIDC authorization endpoint")
set(VISIVO_VLKB_TOKEN_URL "https://sso.ia2.inaf.it/rap-ia2/auth/oauth2/token"
    CACHE STRING "VLKB OIDC token endpoint")
set(VISIVO_VLKB_SCOPE "openid read:gms read:rap" CACHE STRING "VLKB OIDC scopes, space-separated")
set(VISIVO_VLKB_CLIENT_ID "" CACHE STRING "VLKB OIDC client identifier")
set(VISIVO_VLKB_CLIENT_SECRET "" CACHE STRING "VLKB OIDC client secret")
set(VISIVO_VLKB_TAP_USER "" CACHE STRING "VLKB TAP user (unused by the client today)")
set(VISIVO_VLKB_TAP_SECRET "" CACHE STRING "VLKB TAP secret (unused by the client today)")
mark_as_advanced(VISIVO_VLKB_CLIENT_SECRET VISIVO_VLKB_TAP_SECRET)
# The values land inside C++ string literals: escape backslashes and quotes, and
# refuse control characters rather than let them silently change a credential.
# The *_C copies are what the template substitutes; the cache keeps the raw value.
foreach(_vlkb_var AUTH_URL TOKEN_URL SCOPE CLIENT_ID CLIENT_SECRET TAP_USER TAP_SECRET)
    set(_vlkb_value "${VISIVO_VLKB_${_vlkb_var}}")
    if(_vlkb_value MATCHES "[\t\r\n]")
        message(FATAL_ERROR "VISIVO_VLKB_${_vlkb_var} contains a tab or a line break")
    endif()
    string(REPLACE "\\" "\\\\" _vlkb_value "${_vlkb_value}")
    string(REPLACE "\"" "\\\"" _vlkb_value "${_vlkb_value}")
    set(VISIVO_VLKB_${_vlkb_var}_C "${_vlkb_value}")
endforeach()
configure_file(src/auth/AuthKeys.h.in AuthKeys.h @ONLY)
if(VISIVO_VLKB_AUTH_URL STREQUAL "" OR VISIVO_VLKB_TOKEN_URL STREQUAL ""
        OR VISIVO_VLKB_CLIENT_ID STREQUAL "")
    message(STATUS "VLKB sign-in: not configured (set VISIVO_VLKB_CLIENT_ID and _CLIENT_SECRET)")
else()
    message(STATUS "VLKB sign-in: configured")
endif()

set(PROJECT_SOURCES
        src/LayerListModel.cpp
        src/LayerListModel.h
        src/main.cpp
        src/AnalysisParamStore.cpp
        src/AnalysisParamStore.h
        src/Settings.cpp
        src/Settings.h
        src/WebViewProcess.cpp
        src/WebViewProcess.h
        src/auth/AuthWrapper.cpp
        src/auth/AuthWrapper.h
        src/auth/HttpServerReplyHandler.cpp
        src/auth/HttpServerReplyHandler.h
        src/auth/OIDCAuthorizationCodeFlow.cpp
        src/auth/OIDCAuthorizationCodeFlow.h
        src/auth/SecureStore.cpp
        src/auth/SecureStore.h
        src/auth/VisIVOUrlSchemeHandler.cpp
        src/auth/VisIVOUrlSchemeHandler.h
        src/app/BackendLauncher.cpp
        src/app/BackendLauncher.h
        src/app/BackendUrl.cpp
        src/app/BackendUrl.h
        src/app/LaunchOptions.cpp
        src/app/LaunchOptions.h
        src/app/DiagnosticsManager.cpp
        src/app/DiagnosticsManager.h
        src/app/ThemeManager.cpp
        src/app/ThemeManager.h
        src/app/RecentDatasetsManager.cpp
        src/app/RecentDatasetsManager.h
        src/app/UpdateCheck.cpp
        src/app/UpdateCheck.h
        src/app/SampClient.cpp
        src/app/SampClient.h
        src/app/SampMessageHandler.cpp
        src/app/SampMessageHandler.h
        src/gui/RemoteFileBrowserDialog.cpp
        src/gui/RemoteFileBrowserDialog.h
        src/gui/StartupDialog.cpp
        src/gui/StartupDialog.h
        src/gui/RemoteMomentWindow.cpp
        src/gui/RemoteMomentWindow.h
        src/gui/AboutDialog.cpp
        src/gui/AboutDialog.h
        src/gui/CommandPalette.cpp
        src/gui/CommandPalette.h
        src/gui/WorkspaceChrome.cpp
        src/gui/WorkspaceChrome.h
        src/gui/SessionDataTree.cpp
        src/gui/SessionDataTree.h
        src/gui/InspectorPanel.cpp
        src/gui/InspectorPanel.h
        src/gui/AssistantPanel.cpp
        src/gui/AssistantPanel.h
        src/gui/CopilotActionRouter.cpp
        src/gui/CopilotActionRouter.h
        src/gui/DockRail.cpp
        src/gui/DockRail.h
        src/app/ProductRegistry.cpp
        src/app/ProductRecipe.cpp
        src/app/LassoReplay.cpp
        src/app/HostMemory.cpp
        src/app/ProductRegistry.h
        src/app/ProductRecipe.h
        src/app/LassoReplay.h
        src/app/ThumbnailCache.cpp
        src/app/ThumbnailCache.h
        src/gui/CatalogueTableModel.cpp
        src/gui/CatalogueTableModel.h
        src/gui/DataHubWidget.cpp
        src/gui/DataHubWidget_Recent.cpp
        src/gui/DataHubWidget_Exports.cpp
        src/gui/DataHubWidget_Status.cpp
        src/gui/DataHubWidget.h
        src/gui/CardClickFilter.h
        src/gui/ColormapIcon.cpp
        src/gui/ColormapIcon.h
        src/gui/vtkWindowVectorField.cpp
        src/gui/vtkWindowVectorField_Catalogue.cpp
        src/gui/vtkWindowVectorField.h
        src/gui/VLKBInventoryTree.cpp
        src/gui/VLKBInventoryTree.h
        src/vlkb/VlkbQueryService.cpp
        src/vlkb/SourceTable.cpp
        src/vlkb/SourceTable.h
        src/vlkb/VlkbQueryService.h
        src/skava/SkavaClient.cpp
        src/skava/VoTable.cpp
        src/skava/NodeSoda.cpp
        src/skava/BrokerClient.cpp
        src/skava/SkavaClient.h
        src/skava/VoTable.h
        src/skava/NodeSoda.h
        src/skava/BrokerClient.h
        src/skava/BackendRouting.cpp
        src/skava/BackendRouting.h
        src/gui/SkavaSearchPanel.cpp
        src/gui/SkavaSearchPanel.h
        src/gui/RemoteRenderWidget.cpp
        src/gui/RemoteRenderWidget.h
        src/gui/theme/VisivoTheme.cpp
        src/gui/theme/VisivoTheme.h
        src/gui/theme/SegmentedToggle.cpp
        src/gui/theme/ScrollWheelGuard.cpp
        src/gui/theme/SegmentedToggle.h
        src/gui/theme/ScrollWheelGuard.h
        src/gui/ChannelOverlay.cpp
        src/gui/ChannelOverlay.h
        src/gui/CubeVRController.cpp
        src/gui/CubeVRController.h
        src/gui/Catalogue3DTableModel.cpp
        src/gui/Catalogue3DTableModel.h
        src/gui/DiagnosticsWindow.cpp
        src/gui/DiagnosticsWindow.h
        src/gui/ImageLayerController.cpp
        src/gui/ImageLayerController.h
        src/gui/LUTCustomizerDialog.cpp
        src/gui/LUTCustomizerDialog.h
        src/gui/MainWindow.cpp
        src/gui/MainWindow_Menus.cpp
        src/gui/MainWindow_Open.cpp
        src/gui/MainWindow_SkavaOps.cpp
        src/gui/MainWindow_Science.cpp
        src/gui/MainWindow_Samp.cpp
        src/gui/MainWindow_Sky.cpp
        src/gui/MainWindow.h
        src/gui/ProfileWidget.cpp
        src/gui/ProfileWidget.h
        src/gui/ChannelMapsDialog.cpp
        src/gui/ChannelMapsDialog.h
        src/gui/ChannelMapsWindow.cpp
        src/gui/ChannelMapsWindow.h
        src/gui/PvDiagramWidget.cpp
        src/gui/PvDiagramWidget.h
        src/gui/SettingsDialog.cpp
        src/gui/SettingsDialog.h
        src/gui/SourceTableDialog.cpp
        src/gui/SourceTableDialog.h
        src/gui/UpdateNotifier.cpp
        src/gui/UpdateNotifier.h
        src/gui/BackendsSettingsWidget.cpp
        src/gui/BackendsSettingsWidget.h
        src/gui/HiPSViewportWidget.cpp
        src/gui/HiPSViewportWidget.h
        src/gui/HiPSCatalogueDialog.cpp
        src/gui/HiPSSurveyBrowserDialog.cpp
        src/gui/HiPSWindow.cpp
        src/gui/HiPSWindow.h
        src/gui/CubeUiAssembler.cpp
        src/gui/CubeUiAssembler.h
        src/gui/MomentMapController.cpp
        src/gui/MomentMapController.h
        src/gui/ScienceMapWindow.cpp
        src/gui/ScienceMapWindow.h
        src/gui/LinewidthDialog.cpp
        src/gui/LinewidthDialog.h
        src/gui/BaselineDialog.cpp
        src/gui/BaselineDialog.h
        src/gui/StackDialog.cpp
        src/gui/StackDialog.h
        src/gui/SourceFindDialog.cpp
        src/gui/SourceFindDialog.h
        src/gui/SkavaSourceFindDialog.cpp
        src/gui/SkavaSourceFindDialog.h
        src/gui/PvController.cpp
        src/gui/PvController.h
        src/gui/PvTypes.h
        src/gui/NoiseController.cpp
        src/gui/NoiseController.h
        src/gui/NoiseRegionDialog.cpp
        src/gui/NoiseRegionDialog.h
        src/gui/ToolResultDialog.cpp
        src/gui/ToolResultDialog.h
        src/gui/SidebarPanel.cpp
        src/gui/SidebarPanel.h
        src/gui/vtkWindowCatalogue3D.cpp
        src/gui/vtkWindowCatalogue3D_Setup.cpp
        src/gui/vtkWindowCatalogue3D_Scene.cpp
        src/gui/vtkWindowCatalogue3D_Interaction.cpp
        src/gui/vtkWindowCatalogue3D_Data.cpp
        src/gui/vtkWindowCatalogue3D.h
        src/gui/vtkWindowCube.cpp
        src/gui/vtkWindowCube.h
        src/gui/vtkWindowCube_Catalogue.cpp
        src/gui/vtkWindowCube_RemoteRender.cpp
        src/gui/vtkWindowCube_Wcs.cpp
        src/gui/vtkWindowCube_RemoteSlice.cpp
        src/gui/vtkWindowCube_Probe.cpp
        src/gui/vtkWindowCube_Polarisation.cpp
        src/gui/vtkWindowCube_Region.cpp
        src/gui/vtkWindowCube_Setup.cpp
        src/gui/vtkWindowCube_Kinematic.cpp
        src/gui/vtkWindowCube_Lasso.cpp
        src/gui/vtkWindowCube_Movie.cpp
        src/gui/vtkWindowCube_Link.cpp
        src/gui/vtkWindowCube_RegionIO.cpp
        src/gui/Pane2DView.cpp
        src/gui/Pane2DView.h
        src/gui/CubeViewportGeometry.cpp
        src/gui/CubeViewportGeometry.h
        src/gui/CubeStatsUtil.cpp
        src/gui/CubeStatsUtil.h
        src/gui/CubeWcsUtil.cpp
        src/gui/CubeWcsUtil.h
        src/gui/CubeRegionGeom.cpp
        src/gui/PvPathGeometry.cpp
        src/gui/CubeRegionGeom.h
        src/gui/vtkWindowImage.cpp
        src/gui/vtkWindowImage.h
        src/gui/vtkWindowImage_Catalogue.cpp
        src/gui/ArepoExtractDialog.cpp
        src/gui/FilterFitsDialog.cpp
        src/gui/LayerCardDelegate.cpp
        src/gui/LayerCardDelegate.h
        src/gui/Hips2FitsDialog.cpp
        src/gui/VlkbQueryComposer.cpp
        src/gui/VlkbQueryComposer.h
        src/gui/Hips2FitsDialog.h
        src/gui/ConeSearchWindow.cpp
        src/gui/ConeSearchWindow.h
        src/gui/FitsHeaderEditorDialog.cpp
        src/gui/FitsHeaderEditorDialog.h
        src/gui/FitsHeaderCard.h
        src/gui/FilterFitsDialog.h
        src/gui/McutoutJobDialog.cpp
        src/gui/McutoutJobDialog.h
        src/gui/ArepoExtractDialog.h
        src/gui/SedWindow.cpp
        src/gui/SedWindow.h
        src/gui/SedBuilder.h
        src/gui/vtkWindowImage_Wcs.cpp
        src/gui/vtkWindowImage_Polarisation.cpp
        src/gui/vtkWindowImage_Probe.cpp
        src/gui/vtkWindowImage_Dynspec.cpp
        src/gui/vtkWindowImage_Measurement.cpp
        src/gui/vtkWindowImage_Contours.cpp
        src/gui/vtkWindowImage_Annotations.cpp
        src/gui/vtkWindowImage_Products.cpp
        src/gui/vtkWindowImage_Region.cpp
        src/gui/vtkWindowImage_Setup.cpp
        src/gui/vtkWindowVbt.cpp
        src/gui/vtkWindowVbt.h
        src/gui/vtkWindowVbtVolume.cpp
        src/gui/vtkWindowVbtVolume.h
)

set(PROJECT_UI
        ui/AboutDialog.ui
        ui/LUTCustomizerDialog.ui
        ui/MainWindow.ui
        ui/ProfileWidget.ui
        ui/SettingsDialog.ui
        ui/vtkWindowCatalogue3D.ui
        ui/vtkWindowCube.ui
        ui/vtkWindowImage.ui
)

set(PROJECT_ASSETS
        icons/COLORIZE.png
        icons/FILE_OPEN.png
        icons/PIC_BACK.png
        icons/PIC_BOTTOM.png
        icons/PIC_FRONT.png
        icons/PIC_LEFT.png
        icons/PIC_RIGHT.png
        icons/PIC_TOP.png
        icons/RECT_SELECT.png
        icons/RESET.png
        icons/VisIVO.icns
        icons/VisIVO.svg
        icons/VisIVO_32.png
        icons/VisIVO_48.png
        icons/VisIVO_128.png
        icons/VisIVO_256.png
        icons/VisIVO_512.png
)

if (APPLE)
    set(PROJECT_ICON icons/VisIVO.icns)
    set(MACOSX_BUNDLE_ICON_FILE VisIVO.icns)
    set_source_files_properties(${PROJECT_ICON} PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources"
    )
endif ()

qt_add_executable(${PROJECT_NAME}
        ${PROJECT_SOURCES}
        ${PROJECT_ICON}
)

qt_add_ui(${PROJECT_NAME}
        SOURCES ${PROJECT_UI}
)

qt_add_resources(${PROJECT_NAME} assets
        FILES ${PROJECT_ASSETS}
)

# Bundled reference list of common radio / mm spectral lines, compiled in so
# "Load Lines…" has something to offer offline (a compute node usually has no
# outbound internet). Accessible as :/spectral_lines/common_lines.csv.
qt_add_resources(${PROJECT_NAME} bundled_spectral_lines
        PREFIX "/spectral_lines"
        BASE "resources/spectral_lines"
        FILES
            resources/spectral_lines/common_lines.csv
)

# Compile bundled Lato fonts into the binary so they load on all platforms
# without requiring system font installation.
# Accessible at runtime as :/fonts/Lato-Regular.ttf etc.
qt_add_resources(${PROJECT_NAME} bundled_fonts
        PREFIX "/fonts"
        BASE "resources/fonts"
        FILES
            resources/fonts/Lato-Light.ttf
            resources/fonts/Lato-Regular.ttf
            resources/fonts/Lato-Bold.ttf
            resources/fonts/Lato-Black.ttf
)

qt_add_resources(${PROJECT_NAME} branding_assets
    PREFIX "/icons"
    BASE "resources/branding"
    FILES
        resources/branding/visivo_logo_white.svg
)

qt_add_resources(${PROJECT_NAME} rail_icons
    PREFIX "/icons"
    BASE "resources/icons"
    FILES
        resources/icons/rail_threshold.svg
        resources/icons/rail_slice.svg
        resources/icons/rail_contours.svg
        resources/icons/rail_stats.svg
        resources/icons/rail_layers.svg
        resources/icons/rail_catalogue.svg
        resources/icons/rail_3d.svg
        resources/icons/rail_viz.svg
        resources/icons/rail_info.svg
        resources/icons/rail_display.svg
        resources/icons/rail_filter.svg
        resources/icons/rail_metadata.svg
        resources/icons/rail_copilot.svg
        resources/icons/radio_dot.svg
        resources/icons/chevron_down.svg
        resources/icons/chevron_up.svg
        resources/icons/chevron_right.svg
        resources/icons/tree_chevron_down.svg
        resources/icons/tree_chevron_right.svg
        resources/icons/tree_chevron_down_light.svg
        resources/icons/tree_chevron_right_light.svg
        resources/icons/pane_1.svg
        resources/icons/pane_2.svg
        resources/icons/pane_2_rows.svg
        resources/icons/pane_4.svg
        resources/icons/check.svg
        resources/icons/rail_provenance.svg
)

add_subdirectory(src/libwcs)

add_subdirectory(src/qcustomplot)

add_subdirectory(src/core)

add_subdirectory(src/vtk)

target_compile_definitions(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:QT_NO_DEBUG_OUTPUT>
)

# ── Optional: VR (OpenXR) support for the cube viewer ────────────────────────
#
# The VISIVO_ENABLE_VR option is declared near the top of this file so that
# `find_package(OpenXR)` can run before `find_package(VTK)` (VTK ≥ 9.6 with
# RenderingOpenXR carries an unresolved transitive dep on OpenXR::openxr_loader
# in its link interface otherwise). Here we just consume the result.
#
# Off by default — keeps macOS / vanilla VTK builds working unchanged. To
# enable:
#   1. Rebuild VTK with -DVTK_MODULE_ENABLE_VTK_RenderingOpenXR=YES
#      (Windows: needs the Khronos OpenXR SDK; Linux: monado / SteamVR OpenXR)
#   2. Configure VisIVO with -DVISIVO_ENABLE_VR=ON
#
# When ON we additionally check that VTK actually exports the
# `VTK::RenderingOpenXR` target — if not we WARN and silently fall back to a
# VR-less build instead of failing, so a misconfigured user environment never
# blocks the desktop build.
if (VISIVO_ENABLE_VR)
    if (TARGET VTK::RenderingOpenXR)
        message(STATUS "VisIVO: VR support ENABLED (VTK::RenderingOpenXR found)")
        target_compile_definitions(${PROJECT_NAME} PRIVATE VISIVO_HAS_VR=1)
        target_link_libraries(${PROJECT_NAME} PRIVATE VTK::RenderingOpenXR)

        # vtkOpenXRRenderWindowInteractor reads a JSON action manifest at startup
        # (controller bindings — without it BeginSession is never called and the
        # HMD shows nothing). VTK ships these files at `<prefix>/share/xr_actions/`,
        # but the interactor defaults to looking in the *current working directory*.
        # Resolve the install path at configure time and pass it as a compile def
        # so the runtime knows where to point ActionManifestDirectory.
        #
        # VTK_DIR points to `<prefix>/lib/cmake/vtk-X.Y/`, so the prefix is three
        # levels up. Trailing slash is mandatory — VTK concatenates the directory
        # and filename without a separator.
        get_filename_component(_visivo_vtk_prefix "${VTK_DIR}/../../.." ABSOLUTE)
        set(_visivo_xr_actions_dir "${_visivo_vtk_prefix}/share/xr_actions/")
        if (EXISTS "${_visivo_xr_actions_dir}vtk_openxr_actions.json")
            message(STATUS "VisIVO: OpenXR action manifest at ${_visivo_xr_actions_dir}")
            target_compile_definitions(${PROJECT_NAME} PRIVATE
                VISIVO_VTK_OPENXR_ACTIONS_DIR="${_visivo_xr_actions_dir}")
        else ()
            message(WARNING
                "VisIVO: vtk_openxr_actions.json not found at ${_visivo_xr_actions_dir} — "
                "VR controller bindings will fail to load at runtime, leaving the headset "
                "blank. Reinstall VTK with the share/xr_actions/ directory or set the path "
                "manually.")
        endif ()
        unset(_visivo_vtk_prefix)
        unset(_visivo_xr_actions_dir)

        # Optional: 3D controller model rendering. VTK does NOT bundle the glTF
        # models — there's an upstream archive at
        # https://www.paraview.org/files/data/OpenXRControllerModels-0.1.tgz
        # (Valve Index + HTC Vive only). For Meta Quest / Oculus Touch the
        # Kitware blog post "Using VTK with the Meta Quest" describes
        # converting SteamVR's Quest models to glTF in ParaView.
        #
        # Whichever route, the user points VISIVO_OPENXR_CONTROLLER_MODELS_DIR
        # at the directory containing `openxr_controllermodels.json`. The JSON
        # maps each OpenXR interaction profile (e.g.
        # /interaction_profiles/oculus/touch_controller) to glTF asset paths,
        # which VTK then renders attached to the controller poses. Empty means
        # "no controller models" — the controllers still track and trigger
        # actions, they're just invisible. See README/docs for the layout.
        set(VISIVO_OPENXR_CONTROLLER_MODELS_DIR "" CACHE PATH
            "Directory containing openxr_controllermodels.json and glTF controller models. \
Leave empty to skip controller-model rendering.")
        if (VISIVO_OPENXR_CONTROLLER_MODELS_DIR)
            if (EXISTS "${VISIVO_OPENXR_CONTROLLER_MODELS_DIR}/openxr_controllermodels.json")
                message(STATUS "VisIVO: OpenXR controller models at ${VISIVO_OPENXR_CONTROLLER_MODELS_DIR}")
                target_compile_definitions(${PROJECT_NAME} PRIVATE
                    VISIVO_OPENXR_CONTROLLER_MODELS_DIR="${VISIVO_OPENXR_CONTROLLER_MODELS_DIR}")
            else ()
                message(WARNING
                    "VisIVO: VISIVO_OPENXR_CONTROLLER_MODELS_DIR is set to "
                    "'${VISIVO_OPENXR_CONTROLLER_MODELS_DIR}' but no openxr_controllermodels.json "
                    "was found there. Controllers will be invisible in VR.")
            endif ()
        endif ()
    else ()
        message(WARNING
            "VISIVO_ENABLE_VR=ON but VTK::RenderingOpenXR is not available in this "
            "VTK build. Building WITHOUT VR support. Rebuild VTK with "
            "-DVTK_MODULE_ENABLE_VTK_RenderingOpenXR=YES if you need it.")
    endif ()
endif ()

target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/auth"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gui"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/libwcs"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
        visivo_shared_core
        visivo_shared_vtk
        ${qt_modules}
        QCustomPlot
)

# The VLKB sign-in survives a restart through the platform's secure store
# (src/auth/SecureStore.cpp): the Keychain on macOS, the Credential Manager on
# Windows, libsecret on Linux when it is installed (libsecret-1-dev); without
# it the Linux build simply asks for the login on every start.
if(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE "-framework Security" "-framework CoreFoundation")
elseif(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE advapi32)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSECRET IMPORTED_TARGET libsecret-1)
    endif()
    if(LIBSECRET_FOUND)
        target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::LIBSECRET)
        target_compile_definitions(${PROJECT_NAME} PRIVATE VISIVO_HAVE_LIBSECRET=1)
        message(STATUS "VLKB sign-in storage: libsecret")
    else()
        message(STATUS "VLKB sign-in storage: none (install libsecret-1-dev to keep the sign-in)")
    endif()
endif()

if (VISIVO_HAS_WEBRTC)
    # Compile-time toggle picked up by RemoteRenderWidget to gate the
    # WebRTC code paths. Other VisIVO modules don't need to know about
    # the transport; everything is encapsulated in RemoteRenderWidget.
    target_compile_definitions(${PROJECT_NAME} PRIVATE VISIVO_HAS_WEBRTC=1)
    # The WebRTC client (libdatachannel + FFmpeg includes) only compiles
    # when those libs are present, so we add it to the target ONLY in
    # this branch. ``target_sources`` lets us add files post-creation.
    target_sources(${PROJECT_NAME} PRIVATE
            src/gui/RemoteRenderWebRTCClient.cpp
            src/gui/RemoteRenderWebRTCClient.h
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE
            ${_visivo_webrtc_lib}
            PkgConfig::VISIVO_FFMPEG
    )
endif ()

if (APPLE)
    target_link_options(${PROJECT_NAME} PRIVATE
            "LINKER:-no_warn_duplicate_libraries"
    )
endif ()

vtk_module_autoinit(
        TARGETS ${PROJECT_NAME}
        MODULES ${VTK_LIBRARIES}
)

set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER it.inaf.oact.VisIVO
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE TRUE
        # Our own Info.plist template: Qt's generated one has nowhere to declare
        # CFBundleDocumentTypes, and without that Finder cannot hand a
        # double-clicked FITS file to the app at all. The template is Qt's plus
        # the FITS association (see deploy/macos/Info.plist.in for why the
        # handler rank is Alternate).
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/deploy/macos/Info.plist.in"
)

# ── Standalone tool: SSR/RemoteRenderWidget test harness ────────────────────
# Talks to a backend over WebSocket, no full VisIVO UI involved. Built
# as a separate executable so it can be iterated quickly while the
# main app is also under development. Not bundled into the .dmg.
qt_add_executable(test_remote_render
        tools/test_remote_render.cpp
        src/gui/RemoteRenderWidget.cpp
        src/gui/RemoteRenderWidget.h
        # RemoteRenderWidget and the WebRTC client stamp the backend session
        # headers on the requests they build by hand, so this harness needs
        # BackendClient. Same minimal set the unit-test support library links
        # (tests/CMakeLists.txt) — keep the two in step.
        src/app/BackendClient.cpp
        src/app/DiagnosticsManager.cpp
        src/AnalysisParamStore.cpp
        src/Settings.cpp
        src/skava/BackendRouting.cpp
        src/skava/SkavaClient.cpp
        src/skava/VoTable.cpp
        src/skava/NodeSoda.cpp
        src/skava/BrokerClient.cpp
        src/vtk/ImageLod.cpp
)
target_include_directories(test_remote_render PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui
        ${CMAKE_CURRENT_SOURCE_DIR}/src/skava
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vtk
        ${PROJECT_BINARY_DIR}
)
target_link_libraries(test_remote_render PRIVATE
        Qt::Widgets
        Qt::Network
        Qt::WebSockets
)
if (VISIVO_HAS_WEBRTC)
    # Same gate as the main app — the test harness uses the WebRTC
    # path too, so it must see the libdatachannel + FFmpeg symbols
    # AND link the RemoteRenderWebRTCClient TU (its symbols are
    # referenced from RemoteRenderWidget.cpp under VISIVO_HAS_WEBRTC).
    target_compile_definitions(test_remote_render PRIVATE VISIVO_HAS_WEBRTC=1)
    target_sources(test_remote_render PRIVATE
            src/gui/RemoteRenderWebRTCClient.cpp
            src/gui/RemoteRenderWebRTCClient.h
    )
    target_link_libraries(test_remote_render PRIVATE
            ${_visivo_webrtc_lib}
            PkgConfig::VISIVO_FFMPEG
    )
endif ()
set_target_properties(test_remote_render PROPERTIES
        MACOSX_BUNDLE FALSE   # plain command-line binary
)

install(TARGETS ${PROJECT_NAME}
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ── Ship the backend with the application ───────────────────────────────────
#
# BackendLauncher looks for a directory containing app/main.py, and its FIRST
# candidate on macOS is <app>.app/Contents/Resources/backend. Nothing used to
# put it there, so an installed application could not start a backend at all:
# it only ever worked when a developer had already started uvicorn by hand and
# the launcher found the running instance.
#
# The .venv and the caches are excluded deliberately — a virtual environment is
# not relocatable, and this one is 1.3 GB. The launcher creates or finds an
# interpreter at run time instead.
if (APPLE)
    set(_visivo_backend_dest "${PROJECT_NAME}.app/Contents/Resources")
else()
    set(_visivo_backend_dest "${CMAKE_INSTALL_BINDIR}")
endif()

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/backend/"
        DESTINATION "${_visivo_backend_dest}/backend"
        USE_SOURCE_PERMISSIONS
        PATTERN ".venv"        EXCLUDE
        PATTERN "__pycache__"  EXCLUDE
        PATTERN ".pytest_cache" EXCLUDE
        PATTERN "tests"        EXCLUDE
        PATTERN "*.pyc"        EXCLUDE
)

# The same directory, next to the build output, so the launcher's dev-tree
# candidates resolve when running straight from the build directory. A symlink
# on Unix keeps it live; Windows copies.
if (WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_CURRENT_SOURCE_DIR}/backend/app"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/backend/app"
            # The first-run bootstrap installs from <backend>/requirements.txt.
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/backend/requirements.txt"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/backend/requirements.txt"
            COMMENT "Copying backend next to the executable")
else()
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                    "${CMAKE_CURRENT_SOURCE_DIR}/backend"
                    "${CMAKE_BINARY_DIR}/backend"
            COMMENT "Linking backend/ into the build tree for the launcher")
endif()

qt_generate_deploy_app_script(
        TARGET ${PROJECT_NAME}
        OUTPUT_SCRIPT deploy_script
)

install(SCRIPT ${deploy_script})

# ── macOS: bundle libdatachannel + FFmpeg into the .app ─────────────────────
#
# ``macdeployqt`` (invoked by qt_generate_deploy_app_script above)
# only bundles Qt frameworks; non-Qt dynamic libraries that the binary
# is linked against — libdatachannel, libavcodec, libavutil,
# libswscale, plus their transitives (OpenSSL, libsrtp, usrsctp) —
# need an explicit deploy step. Without it the .app links to the
# developer's Homebrew install at /opt/homebrew/lib/, which fails the
# moment another user double-clicks the .dmg without that exact brew
# layout.
#
# We use BundleUtilities — CMake's stock macro for bundling .dylibs +
# fixing install names — which walks the binary's load commands,
# copies every external dylib into ``Contents/Frameworks/``, and
# rewrites the load paths to ``@executable_path/../Frameworks/...``.
# Robust across Apple Silicon + Intel and across brew install
# locations.
if (APPLE AND VISIVO_HAS_WEBRTC)
    # Search dirs for fixup_bundle. We need three groups:
    #
    #   1. Qt's library prefix — without this, frameworks like
    #      ``@rpath/QtConcurrent.framework/...`` (Qt's own self-
    #      references) don't resolve and fixup_bundle aborts mid-run.
    #   2. libdatachannel's FetchContent BUILD dir — we don't install
    #      it system-wide, the .dylib lives in build/_deps/.
    #   3. Homebrew prefixes for FFmpeg and OpenSSL on both Apple
    #      Silicon (/opt/homebrew) and Intel (/usr/local) layouts.
    #
    # Order matters: search Qt + libdatachannel first so the right
    # build artifacts win over any older system-installed copies.
    get_target_property(_qt_core_loc Qt::Core LOCATION)
    get_filename_component(_qt_lib_dir "${_qt_core_loc}" DIRECTORY)
    # QtCore is a framework; LOCATION points inside the .framework, so
    # we strip back to the lib/ dir that holds Qt*.framework siblings.
    get_filename_component(_qt_lib_dir "${_qt_lib_dir}" DIRECTORY)
    get_filename_component(_qt_lib_dir "${_qt_lib_dir}" DIRECTORY)
    get_filename_component(_qt_lib_dir "${_qt_lib_dir}" DIRECTORY)

    # VTK lib dir — derived from one of the imported VTK targets the
    # same way. Some VTK 9.x packages put libs under ``lib`` directly,
    # others under ``lib64`` / ``Frameworks``; LOCATION is the most
    # portable handle.
    set(_vtk_lib_dir "")
    if (TARGET VTK::CommonCore)
        get_target_property(_vtk_loc VTK::CommonCore LOCATION)
        if (_vtk_loc)
            get_filename_component(_vtk_lib_dir "${_vtk_loc}" DIRECTORY)
        endif ()
    endif ()

    set(_visivo_macos_dirs
            "${_qt_lib_dir}"
            "${_vtk_lib_dir}"
            "${CMAKE_BINARY_DIR}/_deps/libdatachannel-build"
            "/opt/homebrew/lib"
            "/opt/homebrew/opt/ffmpeg/lib"
            "/opt/homebrew/opt/openssl@3/lib"
            "/usr/local/lib"
            "/usr/local/opt/ffmpeg/lib"
    )
    # $ENV{DESTDIR} because this runs on the installed tree directly rather
    # than through install(TARGETS), which prepends it for us. Without it a
    # `DESTDIR=/tmp/stage cmake --install` bundles the wrong directory.
    install(CODE "
        include(BundleUtilities)
        set(BU_CHMOD_BUNDLE_ITEMS ON)
        fixup_bundle(
            \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.app\"
            \"\"
            \"${_visivo_macos_dirs}\"
        )
    ")
    message(STATUS
        "Mac bundling: fixup_bundle search dirs = ${_visivo_macos_dirs}"
    )
endif ()

# ── macOS: repair the plugins, then re-sign the whole bundle ─────────────
#
# The installed .app does not start at all without this — two faults, both
# measured on a staging tree. The reasoning lives next to the code, in
# cmake/MacBundleRepair.cmake.in, which is a separate file because it is real
# logic (it probes each plugin and fails loudly) rather than the two-liner an
# install(CODE) string can carry legibly.
#
# No COMPONENT, deliberately: install(TARGETS) above, the backend, and Qt's
# deploy script are all in the default component, so putting the repair in
# `Runtime` would let `--component Runtime` try to sign a bundle that the same
# invocation never installed.
if (APPLE)
    set(VISIVO_CODESIGN_ID "-" CACHE STRING
            "Identity for the post-install codesign pass ('-' = ad hoc)")
    configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MacBundleRepair.cmake.in"
            "${CMAKE_CURRENT_BINARY_DIR}/MacBundleRepair.cmake"
            @ONLY)
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/MacBundleRepair.cmake")
endif ()

# ── Windows runtime DLL deployment ───────────────────────────────────────────
#
# On Windows the OS only looks for DLLs alongside the exe (or on PATH), so a
# freshly built VisIVO can't even start without the VTK / OpenXR / cfitsio /
# vcpkg DLLs sitting next to it. We solve this in two layers:
#
#   1. Post-build COPY:  every build automatically mirrors the exe's
#      transitive runtime-DLL set into the build output dir, so launching
#      ./VisIVOVisualAnalytics.exe straight after a build just works. Qt's
#      own DLLs / plugins / WebEngine assets are NOT in $<TARGET_RUNTIME_DLLS>
#      (Qt is a special citizen) — run windeployqt manually, or set
#      VISIVO_AUTO_WINDEPLOYQT=ON to chain it onto every build.
#
#   2. Install:          `cmake --install build` lays the same DLL set down
#      next to the installed exe, so a distributable bundle is one command.
#      qt_generate_deploy_app_script (above) handles Qt's part on Windows too.
if (WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}>
                    $<TARGET_FILE_DIR:${PROJECT_NAME}>
            COMMAND_EXPAND_LISTS
            VERBATIM)

    install(FILES $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}>
            DESTINATION ${CMAKE_INSTALL_BINDIR})

    option(VISIVO_AUTO_WINDEPLOYQT
            "Run windeployqt after every build to mirror Qt DLLs/plugins next to the exe"
            OFF)
    if (VISIVO_AUTO_WINDEPLOYQT)
        find_program(WINDEPLOYQT_EXE windeployqt
                HINTS "${Qt6_DIR}/../../../bin")
        if (WINDEPLOYQT_EXE)
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                    COMMAND "${WINDEPLOYQT_EXE}"
                            --no-compiler-runtime
                            --no-translations
                            $<TARGET_FILE:${PROJECT_NAME}>
                    VERBATIM)
        else ()
            message(WARNING "VISIVO_AUTO_WINDEPLOYQT=ON but windeployqt was not found "
                    "under ${Qt6_DIR}/../../../bin — falling back to manual deploy.")
        endif ()
    endif ()
endif ()

# ── Unit tests (opt-in via -DBUILD_TESTING=ON) ────────────────────────────────
option(BUILD_TESTING "Build the VisIVO unit-test suite" OFF)
if (BUILD_TESTING)
    add_subdirectory(tests)
endif ()
