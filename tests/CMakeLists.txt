# tests/CMakeLists.txt
# Minimal unit-test suite for VisIVO Visual Analytics.
#
# Requirements:
#   Qt::Test, Qt::Core, Qt::Network
#   No VTK, no QtWidgets, no live backend.
#
# Two ways to build:
#   1. As part of the main project (opt-in):
#        cmake -B build -DBUILD_TESTING=ON
#        cmake --build build --target VisIVOTests
#        ctest --test-dir build -V
#   2. Standalone (CI — avoids configuring VTK / Qt-Widgets / libdatachannel):
#        cmake -S tests -B build-tests
#        cmake --build build-tests
#        ctest --test-dir build-tests -V

cmake_minimum_required(VERSION 3.22)

# ── Dual-mode: standalone (CI) vs subdirectory of the main build ────────────
# The suite deliberately links only Qt Core/Network/Test, so it can be
# configured on its own without the heavy VTK / Qt-Widgets / WebRTC stack the
# full application requires. When configured standalone this file becomes the
# top-level project; VISIVO_ROOT then points back at the repository root so the
# `${VISIVO_ROOT}/src/...` source paths keep resolving in both modes.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(VisIVOTests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    get_filename_component(VISIVO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
else()
    set(VISIVO_ROOT "${PROJECT_SOURCE_DIR}")
endif()

include(CTest)
enable_testing()

find_package(Qt6 REQUIRED COMPONENTS Test Core Network)

# ── Shared support: BackendClient + DiagnosticsManager ──────────────────────
# Compiled once and reused by every test target that needs backend parsing.
# Does NOT depend on Qt::Widgets, VTK, or any UI component.
add_library(visivo_test_support STATIC
    ${VISIVO_ROOT}/src/app/BackendClient.cpp
    ${VISIVO_ROOT}/src/app/BackendUrl.cpp
    ${VISIVO_ROOT}/src/app/DiagnosticsManager.cpp
    ${VISIVO_ROOT}/src/app/ProductRecipe.cpp
    ${VISIVO_ROOT}/src/app/LassoReplay.cpp
    ${VISIVO_ROOT}/src/app/UpdateCheck.cpp
    ${VISIVO_ROOT}/src/vlkb/SourceTable.cpp
    ${VISIVO_ROOT}/src/AnalysisParamStore.cpp
    ${VISIVO_ROOT}/src/Settings.cpp
    ${VISIVO_ROOT}/src/skava/BackendRouting.cpp
    ${VISIVO_ROOT}/src/skava/SkavaClient.cpp
    ${VISIVO_ROOT}/src/skava/VoTable.cpp
    ${VISIVO_ROOT}/src/vtk/ImageLod.cpp
    ${VISIVO_ROOT}/src/gui/PvPathGeometry.cpp
)
set_target_properties(visivo_test_support PROPERTIES AUTOMOC ON)
target_include_directories(visivo_test_support
    PUBLIC
        ${VISIVO_ROOT}/src
        ${VISIVO_ROOT}/src/app
        ${VISIVO_ROOT}/src/gui
        ${VISIVO_ROOT}/src/skava
        ${VISIVO_ROOT}/src/vtk
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
    TestBackendContract.h
    test_backend_contract.cpp
    TestRemoteSliceCache.h
    test_remote_slice_cache.cpp
    TestLinkedWindowRegistry.h
    test_linked_window_registry.cpp
    TestSpectralUnitConvert.h
    test_spectral_unit_convert.cpp
    TestCubeRegionGeom.h
    test_cube_region_geom.cpp
    TestCatalogueCsv.h
    test_catalogue_csv.cpp
    TestFitsHeaderString.h
    test_fits_header_string.cpp
    TestStokesCompanion.h
    test_stokes_companion.cpp
    TestProductOrigin.h
    test_product_origin.cpp
    TestToolResultRecord.h
    test_tool_result_record.cpp
    TestCubeToolGate.h
    test_cube_tool_gate.cpp
    TestSpectralAxisInfer.h
    test_spectral_axis_infer.cpp
    TestKinematicLevelField.h
    test_kinematic_level_field.cpp
    TestRayVoxelPick.h
    test_ray_voxel_pick.cpp
    TestSedBuilder.h
    test_sed_builder.cpp
    TestVlkbPosString.h
    test_vlkb_pos_string.cpp
    TestSkyOverlap.h
    test_sky_overlap.cpp
    TestCatalogueCosmology.h
    test_catalogue_cosmology.cpp
    TestFitsHeaderCard.h
    test_fits_header_card.cpp
    TestViewerModeState.h
    test_viewer_mode_state.cpp
    TestLayerCardGeometry.h
    test_layer_card_geometry.cpp
    TestRegionReadout.h
    test_region_readout.cpp
    TestBackendUrlLocality.h
    test_backend_url_locality.cpp
    TestSkavaClient.h
    test_skava_client.cpp
    TestPvPathGeometry.h
    test_pv_path_geometry.cpp
    TestProductRecipe.h
    test_product_recipe.cpp
    TestLassoReplay.h
    test_lasso_replay.cpp
    TestUpdateCheck.h
    test_update_check.cpp
    TestSourceTable.h
    test_source_table.cpp
)
set_target_properties(VisIVOTests PROPERTIES AUTOMOC ON)
target_include_directories(VisIVOTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}    # find TestBackendClient.h etc.
    ${VISIVO_ROOT}/src
    ${VISIVO_ROOT}/src/app
    ${VISIVO_ROOT}/src/gui
    ${PROJECT_BINARY_DIR}
)
target_link_libraries(VisIVOTests PRIVATE
    visivo_test_support
    Qt::Test
)
# Let the contract test locate the shared header manifest in both build layouts.
target_compile_definitions(VisIVOTests PRIVATE
    VISIVO_REPO_ROOT="${VISIVO_ROOT}"
    VISIVO_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

# ── CTest registration ───────────────────────────────────────────────────────
# Both test classes live in the same binary; run them all in a single CTest entry.
# Verbose output is opt-in: CTEST_OUTPUT_ON_FAILURE=1  or  ctest -V
add_test(NAME VisIVOUnitTests COMMAND VisIVOTests)
