cmake_minimum_required(VERSION 3.8)

project(VisIVOVisualAnalytics LANGUAGES CXX C VERSION 1.6.0)

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
find_package(Qt5 QUIET REQUIRED COMPONENTS ${qt_components})

foreach (_qt_comp IN LISTS qt_components)
    list(APPEND qt_modules "Qt5::${_qt_comp}")
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
        src/aboutform.cpp
        src/astroutils.cpp
        src/authwrapper.cpp
        src/base64.cpp
        src/caesarwindow.cpp
        src/color.cpp
        src/downloadmanager.cpp
        src/ds9region/DS9Region.cpp
        src/ds9region/DS9RegionParser.cpp
        src/extendedglyph3d.cpp
        src/libwcs/cel.c
        src/libwcs/dateutil.c
        src/libwcs/distort.c
        src/libwcs/dsspos.c
        src/libwcs/fileutil.c
        src/libwcs/fitsfile.c
        src/libwcs/fitswcs.c
        src/libwcs/hget.c
        src/libwcs/hput.c
        src/libwcs/iget.c
        src/libwcs/imhfile.c
        src/libwcs/imio.c
        src/libwcs/lin.c
        src/libwcs/platepos.c
        src/libwcs/proj.c
        src/libwcs/sph.c
        src/libwcs/tnxpos.c
        src/libwcs/wcs.c
        src/libwcs/wcscon.c
        src/libwcs/wcsinit.c
        src/libwcs/wcslib.c
        src/libwcs/wcstrig.c
        src/libwcs/worldpos.c
        src/loadingwidget.cpp
        src/qcustomplot.cpp
        src/luteditor.cpp
        src/main.cpp
        src/mainwindow.cpp
        src/mcutoutsummary.cpp
        src/observedobject.cpp
        src/operationqueue.cpp
        src/pqmeminspector.cpp
        src/pqwindowcube.cpp
        src/pqwindowimage.cpp
        src/pvdownloadmanager.cpp
        src/sed.cpp
        src/sednode.cpp
        src/sessionloader.cpp
        src/settingform.cpp
        src/subsetselectordialog.cpp
        src/treeitem.cpp
        src/treemodel.cpp
        src/usertablewindow.cpp
        src/vialactea.cpp
        src/vialacteainitialquery.cpp
        src/vialacteasource.cpp
        src/vialacteastringdictwidget.cpp
        src/visivoutilsdesktop.cpp
        src/vispoint.cpp
        src/vlkbtable.cpp
        src/vlvastackimage.cpp
        src/vlvaurlschemehandler.cpp
        src/vsobjectdesktop.cpp
        src/vstabledesktop.cpp
        src/vtkellipse.cpp
        src/vtkextracthistogram.cpp
        src/vtkfitsreader.cpp
        src/vtkfitstoolwidget_new.cpp
        src/vtkfitstoolwidgetobject.cpp
        src/vtklegendscaleactor.cpp
        src/xmlparser.cpp
        src/interactors/vtkinteractorstyleimagecustom.cpp
        )

set(headers
        src/aboutform.h
        src/astroutils.h
        src/authkeys.h
        src/authwrapper.h
        src/base64.h
        src/caesarwindow.h
        src/color.h
        src/downloadmanager.h
        src/ds9region/CodeUtils.h
        src/ds9region/Consts.h
        src/ds9region/DS9Region.h
        src/ds9region/DS9RegionParser.h
        src/errorMessage.h
        src/extendedglyph3d.h
        src/libwcs/fitsfile.h
        src/libwcs/wcs.h
        src/loadingwidget.h
        src/qcustomplot.h
        src/luteditor.h
        src/mainwindow.h
        src/mcutoutsummary.h
        src/observedobject.h
        src/operationqueue.h
        src/osxhelper.h
        src/pqmeminspector.h
        src/pqwindowcube.h
        src/pqwindowimage.h
        src/pvdownloadmanager.h
        src/sed.h
        src/sednode.h
        src/sessionloader.h
        src/settingform.h
        src/singleton.h
        src/subsetselectordialog.h
        src/treeitem.h
        src/treemodel.h
        src/usertablewindow.h
        src/vialactea.h
        src/vialacteainitialquery.h
        src/vialacteasource.h
        src/vialacteastringdictwidget.h
        src/visivoutilsdesktop.h
        src/vispoint.h
        src/vlkbtable.h
        src/vlvastackimage.h
        src/vlvaurlschemehandler.h
        src/vsobjectdesktop.h
        src/vstabledesktop.h
        src/vtkellipse.h
        src/vtkextracthistogram.h
        src/vtkfitsreader.h
        src/vtkfitstoolwidget_new.h
        src/vtkfitstoolwidgetobject.h
        src/vtklegendscaleactor.h
        src/xmlparser.h
        src/interactors/vtkinteractorstyleimagecustom.h
        )

set(ui_files
        ui/aboutform.ui
        ui/caesarwindow.ui
        ui/loadingwidget.ui
        ui/mainwindow.ui
        ui/mcutoutsummary.ui
        ui/operationqueue.ui
        ui/pqmeminspector.ui
        ui/pqwindowcube.ui
        ui/pqwindowimage.ui
        ui/sessionloader.ui
        ui/settingform.ui
        ui/subsetselectordialog.ui
        ui/usertablewindow.ui
        ui/vialactea.ui
        ui/vialacteainitialquery.ui
        ui/vialacteastringdictwidget.ui
        ui/vtkfitstoolwidget_new.ui
        )

set(ICON_RESOURCE icons/logo_256_beta.icns)
set(MACOSX_BUNDLE_ICON_FILE logo_256_beta.icns)
set(app_icon_macos "${CMAKE_SOURCE_DIR}/icons/logo_256_beta.icns")
set_source_files_properties(${app_icon_macos} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources"
)

set(resources visivo.qrc ${ICON_RESOURCE})

add_executable(VisIVOVisualAnalytics ${ui_files} ${resources} ${sources} ${headers}
    src/pvdownloadmanager.h src/pvdownloadmanager.cpp)

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
