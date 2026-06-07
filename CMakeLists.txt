cmake_minimum_required(VERSION 3.22)

project(VisIVOVisualAnalytics
        VERSION 0.0.2
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
        FiltersModeling
        FiltersStatistics
        GUISupportQt
        InteractionStyle
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

get_target_property(cfitsio_include_dirs CFITSIO::cfitsio INTERFACE_INCLUDE_DIRECTORIES)
if (cfitsio_include_dirs)
    list(FILTER cfitsio_include_dirs EXCLUDE REGEX
            "^/Library/Developer/CommandLineTools/SDKs/.*/usr/include$")
    set_target_properties(CFITSIO::cfitsio PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${cfitsio_include_dirs}"
    )
endif ()

qt_standard_project_setup()
set(CMAKE_AUTOUIC OFF)

configure_file(src/Version.h.in Version.h @ONLY)

set(PROJECT_SOURCES
        src/LayerListModel.cpp
        src/LayerListModel.h
        src/main.cpp
        src/Settings.cpp
        src/Settings.h
        src/WebViewProcess.cpp
        src/WebViewProcess.h
        src/auth/AuthKeys.h
        src/auth/AuthWrapper.cpp
        src/auth/AuthWrapper.h
        src/auth/HttpServerReplyHandler.cpp
        src/auth/HttpServerReplyHandler.h
        src/auth/OIDCAuthorizationCodeFlow.cpp
        src/auth/OIDCAuthorizationCodeFlow.h
        src/auth/VisIVOUrlSchemeHandler.cpp
        src/auth/VisIVOUrlSchemeHandler.h
        src/app/BackendLauncher.cpp
        src/app/BackendLauncher.h
        src/app/DiagnosticsManager.cpp
        src/app/DiagnosticsManager.h
        src/app/ThemeManager.cpp
        src/app/ThemeManager.h
        src/app/RecentDatasetsManager.cpp
        src/app/RecentDatasetsManager.h
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
        src/app/ThumbnailCache.cpp
        src/app/ThumbnailCache.h
        src/gui/CatalogueTableModel.cpp
        src/gui/CatalogueTableModel.h
        src/gui/DataHubWidget.cpp
        src/gui/DataHubWidget.h
        src/gui/VLKBInventoryTree.cpp
        src/gui/VLKBInventoryTree.h
        src/vlkb/VlkbQueryService.cpp
        src/vlkb/VlkbQueryService.h
        src/skava/SkavaClient.cpp
        src/skava/SkavaClient.h
        src/gui/SkavaSearchPanel.cpp
        src/gui/SkavaSearchPanel.h
        src/gui/RemoteRenderWidget.cpp
        src/gui/RemoteRenderWidget.h
        src/gui/theme/VisivoTheme.cpp
        src/gui/theme/VisivoTheme.h
        src/gui/theme/SegmentedToggle.cpp
        src/gui/theme/SegmentedToggle.h
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
        src/gui/BackendsSettingsWidget.cpp
        src/gui/BackendsSettingsWidget.h
        src/gui/HiPSViewportWidget.cpp
        src/gui/HiPSViewportWidget.h
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
        src/gui/vtkWindowCatalogue3D.h
        src/gui/vtkWindowCube.cpp
        src/gui/vtkWindowCube.h
        src/gui/vtkWindowImage.cpp
        src/gui/vtkWindowImage.h
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
        resources/icons/chevron_down.svg
        resources/icons/chevron_up.svg
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
)

# ── Standalone tool: SSR/RemoteRenderWidget test harness ────────────────────
# Talks to a backend over WebSocket, no full VisIVO UI involved. Built
# as a separate executable so it can be iterated quickly while the
# main app is also under development. Not bundled into the .dmg.
qt_add_executable(test_remote_render
        tools/test_remote_render.cpp
        src/gui/RemoteRenderWidget.cpp
        src/gui/RemoteRenderWidget.h
)
target_link_libraries(test_remote_render PRIVATE
        Qt::Widgets
        Qt::Network
        Qt::WebSockets
)
set_target_properties(test_remote_render PROPERTIES
        MACOSX_BUNDLE FALSE   # plain command-line binary
)

install(TARGETS ${PROJECT_NAME}
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

qt_generate_deploy_app_script(
        TARGET ${PROJECT_NAME}
        OUTPUT_SCRIPT deploy_script
)

install(SCRIPT ${deploy_script})

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
