cmake_minimum_required(VERSION 3.8)

project(VisIVOVisualAnalytics LANGUAGES CXX C VERSION 0.1.0)

# Set a default build type if none was specified
set(default_build_type "Release")
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
    set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE
            STRING "Choose the type of build." FORCE
            )
    # Set the possible values of build type for cmake-gui
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
            "Debug" "Release" "MinSizeRel" "RelWithDebInfo"
            )
endif ()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(ParaView REQUIRED)
find_package(CFITSIO REQUIRED)
find_package(Boost REQUIRED)

set(qt_components Widgets Network NetworkAuth WebEngineWidgets Concurrent Xml PrintSupport)
find_package(Qt6 QUIET REQUIRED COMPONENTS ${qt_components})

foreach (_qt_comp IN LISTS qt_components)
    list(APPEND qt_modules "Qt6::${_qt_comp}")
endforeach ()

if (NOT PARAVIEW_USE_QT)
    message(FATAL_ERROR
            "${CMAKE_PROJECT_NAME} requires PARAVIEW_USE_QT "
            "to be enabled. Please rebuild ParaView (or point to a different build of "
            "ParaView) with PARAVIEW_USE_QT set to TRUE")
endif ()

include(GNUInstallDirs)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS ui)

set(sources
        src/main.cpp
        src/startupwindow.cpp
        src/fitsheaderviewer.cpp
        src/astroutils.cpp
        src/fitsheadermodifierdialog.cpp
        src/recentfilesmanager.cpp
)

set(headers
        src/startupwindow.h
        src/fitsheaderviewer.h
        src/astroutils.h
        src/fitsheadermodifierdialog.h
        src/recentfilesmanager.h
)

set(ui_files
       ui/startupwindow.ui
       ui/fitsheaderviewer.ui
       ui/fitsheadermodifierdialog.ui
)

add_subdirectory(src/libwcs)


set(ICON_RESOURCE icons/logo_256_beta.icns)
set(MACOSX_BUNDLE_ICON_FILE logo_256_beta.icns)
set(app_icon_macos "${CMAKE_SOURCE_DIR}/icons/logo_256_beta.icns")
set_source_files_properties(${app_icon_macos} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources"
)

qt_add_resources(VisIVOVisualAnalytics assets
    FILES
    icons/settings.svg
    icons/folder_open.svg
    icons/cloud.svg
)

add_executable(VisIVOVisualAnalytics ${ui_files} ${resources} ${sources} ${headers} ${assets})

target_include_directories(VisIVOVisualAnalytics PRIVATE ${Boost_INCLUDE_DIRS})

target_link_libraries(VisIVOVisualAnalytics PRIVATE
        ${qt_modules}
        ${VTK_LIBRARIES}
        ParaView::pqApplicationComponents
        ParaView::RemotingViews
        CFITSIO::CFITSIO
)

set_target_properties(VisIVOVisualAnalytics PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER it.inaf.oact.visivo
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}
    MACOSX_BUNDLE ON
)
