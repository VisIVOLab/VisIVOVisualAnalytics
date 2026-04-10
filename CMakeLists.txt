cmake_minimum_required(VERSION 3.22)

project(VisIVOVisualAnalytics
        VERSION 0.0.1
        LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

message(STATUS "Configuring ${PROJECT_NAME} v${PROJECT_VERSION}")

set(qt_components
        Concurrent
        Widgets
        Svg
        Network
        NetworkAuth
        PrintSupport
        WebEngineWidgets
)
foreach (module IN LISTS qt_components)
    list(APPEND qt_modules "Qt::${module}")
endforeach ()

find_package(Qt6 6.10.0 CONFIG REQUIRED COMPONENTS ${qt_components})
message(STATUS "Using Qt v${Qt6_VERSION}")

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

find_package(cfitsio CONFIG REQUIRED)
message(STATUS "Using cfitsio v${cfitsio_VERSION}")

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
        src/app/DiagnosticsManager.cpp
        src/app/DiagnosticsManager.h
        src/gui/RemoteFileBrowserDialog.cpp
        src/gui/RemoteFileBrowserDialog.h
        src/gui/RemoteMomentWindow.cpp
        src/gui/RemoteMomentWindow.h
        src/gui/AboutDialog.cpp
        src/gui/AboutDialog.h
        src/gui/CatalogueTableModel.cpp
        src/gui/CatalogueTableModel.h
        src/gui/DataHubWidget.cpp
        src/gui/DataHubWidget.h
        src/gui/theme/VisivoTheme.cpp
        src/gui/theme/VisivoTheme.h
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
        src/gui/PvDiagramWidget.cpp
        src/gui/PvDiagramWidget.h
        src/gui/SettingsDialog.cpp
        src/gui/SettingsDialog.h
        src/gui/HiPSViewportWidget.cpp
        src/gui/HiPSViewportWidget.h
        src/gui/HiPSWindow.cpp
        src/gui/HiPSWindow.h
        src/gui/MomentMapController.cpp
        src/gui/MomentMapController.h
        src/gui/NoiseController.cpp
        src/gui/NoiseController.h
        src/gui/NoiseRegionDialog.cpp
        src/gui/NoiseRegionDialog.h
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
)

add_subdirectory(src/libwcs)

add_subdirectory(src/qcustomplot)

add_subdirectory(src/core)

add_subdirectory(src/vtk)

target_compile_definitions(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:QT_NO_DEBUG_OUTPUT>
)

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

install(TARGETS ${PROJECT_NAME}
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

qt_generate_deploy_app_script(
        TARGET ${PROJECT_NAME}
        OUTPUT_SCRIPT deploy_script
)

install(SCRIPT ${deploy_script})

# ── Unit tests (opt-in via -DBUILD_TESTING=ON) ────────────────────────────────
option(BUILD_TESTING "Build the VisIVO unit-test suite" OFF)
if (BUILD_TESTING)
    add_subdirectory(tests)
endif ()
