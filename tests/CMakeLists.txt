# tests/CMakeLists.txt
# Minimal unit-test suite for VisIVO Visual Analytics.
#
# Requirements:
#   Qt::Test, Qt::Core, Qt::Network
#   No VTK, no QtWidgets, no live backend.
#
# Build & run:
#   cmake -B build -DBUILD_TESTING=ON
#   cmake --build build --target VisIVOTests
#   ctest --test-dir build -V   (or)   build/tests/VisIVOTests -v2

cmake_minimum_required(VERSION 3.22)

include(CTest)
enable_testing()

find_package(Qt6 REQUIRED COMPONENTS Test Core Network)

# ── Shared support: BackendClient + DiagnosticsManager ──────────────────────
# Compiled once and reused by every test target that needs backend parsing.
# Does NOT depend on Qt::Widgets, VTK, or any UI component.
add_library(visivo_test_support STATIC
    ${PROJECT_SOURCE_DIR}/src/app/BackendClient.cpp
    ${PROJECT_SOURCE_DIR}/src/app/DiagnosticsManager.cpp
    ${PROJECT_SOURCE_DIR}/src/AnalysisParamStore.cpp
    ${PROJECT_SOURCE_DIR}/src/Settings.cpp
    ${PROJECT_SOURCE_DIR}/src/skava/BackendRouting.cpp
    ${PROJECT_SOURCE_DIR}/src/vtk/ImageLod.cpp
)
set_target_properties(visivo_test_support PROPERTIES AUTOMOC ON)
target_include_directories(visivo_test_support
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/app
        ${PROJECT_SOURCE_DIR}/src/gui
        ${PROJECT_SOURCE_DIR}/src/skava
        ${PROJECT_SOURCE_DIR}/src/vtk
        ${PROJECT_BINARY_DIR}      # for generated headers (Version.h etc.)
)
target_link_libraries(visivo_test_support
    PUBLIC
        Qt::Core
        Qt::Network
)

# ── Combined test executable ─────────────────────────────────────────────────
add_executable(VisIVOTests
    main.cpp
    TestBackendClient.h
    test_backendclient.cpp
    TestCatalogueParser.h
    test_catalogue_parser.cpp
    TestBackendRouting.h
    test_backend_routing.cpp
    TestImageLod.h
    test_image_lod.cpp
    TestAnalysisParamStore.h
    test_analysis_param_store.cpp
)
set_target_properties(VisIVOTests PROPERTIES AUTOMOC ON)
target_include_directories(VisIVOTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}    # find TestBackendClient.h etc.
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/app
    ${PROJECT_SOURCE_DIR}/src/gui
    ${PROJECT_BINARY_DIR}
)
target_link_libraries(VisIVOTests PRIVATE
    visivo_test_support
    Qt::Test
)

# ── CTest registration ───────────────────────────────────────────────────────
# Both test classes live in the same binary; run them all in a single CTest entry.
# Verbose output is opt-in: CTEST_OUTPUT_ON_FAILURE=1  or  ctest -V
add_test(NAME VisIVOUnitTests COMMAND VisIVOTests)
