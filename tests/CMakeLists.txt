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

find_package(Qt6 REQUIRED COMPONENTS Test Core Network Concurrent)

# ── Shared support: BackendClient + DiagnosticsManager ──────────────────────
# Compiled once and reused by every test target that needs backend parsing.
# Does NOT depend on Qt::Widgets, VTK, or any UI component.
add_library(visivo_test_support STATIC
    ${VISIVO_ROOT}/src/app/BackendClient.cpp
    ${VISIVO_ROOT}/src/app/BackendClient_Tasks.cpp
    ${VISIVO_ROOT}/src/app/BackendClient_VrPair.cpp
    ${VISIVO_ROOT}/src/thirdparty/qrcodegen/qrcodegen.cpp
    ${VISIVO_ROOT}/src/app/BackendUrl.cpp
    ${VISIVO_ROOT}/src/app/LaunchOptions.cpp
    ${VISIVO_ROOT}/src/app/RecentDatasetsManager.cpp
    ${VISIVO_ROOT}/src/app/ClusterProfile.cpp
    ${VISIVO_ROOT}/src/app/ClusterConnection.cpp
    ${VISIVO_ROOT}/src/app/DiagnosticsManager.cpp
    ${VISIVO_ROOT}/src/app/ProductRecipe.cpp
    ${VISIVO_ROOT}/src/app/LassoReplay.cpp
    ${VISIVO_ROOT}/src/app/SharedSelection.cpp
    ${VISIVO_ROOT}/src/app/SofiaSuggest.cpp
    ${VISIVO_ROOT}/src/app/FinderTaskRun.cpp
    ${VISIVO_ROOT}/src/app/UpdateCheck.cpp
    ${VISIVO_ROOT}/src/vlkb/SourceTable.cpp
    ${VISIVO_ROOT}/src/AnalysisParamStore.cpp
    ${VISIVO_ROOT}/src/Settings.cpp
    ${VISIVO_ROOT}/src/skava/BackendRouting.cpp
    ${VISIVO_ROOT}/src/skava/SkavaClient.cpp
    ${VISIVO_ROOT}/src/skava/VoTable.cpp
    ${VISIVO_ROOT}/src/skava/NodeSoda.cpp
    ${VISIVO_ROOT}/src/skava/BrokerClient.cpp
    ${VISIVO_ROOT}/src/vtk/ImageLod.cpp
    ${VISIVO_ROOT}/src/gui/PvPathGeometry.cpp
    ${VISIVO_ROOT}/src/gui/OrbitPath.cpp
    ${VISIVO_ROOT}/src/gui/ParticleFrame.cpp
    ${VISIVO_ROOT}/src/gui/CopilotStatus.cpp
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
    TestRemoteLevels.h
    test_remote_levels.cpp
    TestRemoteRenderQueue.h
    test_remote_render_queue.cpp
    TestRenderSessionCalls.h
    test_render_session_calls.cpp
    ${VISIVO_ROOT}/src/gui/RenderSessionCalls.cpp
    TestRegionFullRes.h
    test_region_full_res.cpp
    TestDatasetName.h
    test_dataset_name.cpp
    TestVbtByteOrder.h
    test_vbt_byte_order.cpp
    TestCubeStatRows.h
    test_cube_stat_rows.cpp
    TestRecentReopen.h
    test_recent_reopen.cpp
    TestThresholdShare.h
    test_threshold_share.cpp
    TestColourBarTicks.h
    test_colour_bar_ticks.cpp
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
    TestOrbitPath.h
    test_orbit_path.cpp
    TestParticleFrame.h
    test_particle_frame.cpp
    TestProductRecipe.h
    test_product_recipe.cpp
    TestLassoReplay.h
    test_lasso_replay.cpp
    TestSofiaSuggest.h
    test_sofia_suggest.cpp
    TestUpdateCheck.h
    test_update_check.cpp
    TestSourceTable.h
    test_source_table.cpp
    TestBackendTaskFollow.h
    TestCosmologyTools.h
    test_cosmology_tools.cpp
    test_backend_task_follow.cpp
    TestSamp.h
    test_samp.cpp
    ${VISIVO_ROOT}/src/app/SampCallbackServer.cpp
    TestCopilotStatus.h
    test_copilot_status.cpp
    TestCopilotChoice.h
    test_copilot_choice.cpp
    TestProbeSpectrumFetch.h
    test_probe_spectrum_fetch.cpp
    TestWcsAxisTicks.h
    test_wcs_axis_ticks.cpp
    TestOpenJobMerge.h
    test_open_job_merge.cpp
    TestTaskQuitDrain.h
    test_task_quit_drain.cpp
    TestVelocityLodGeometry.h
    test_velocity_lod_geometry.cpp
    TestSystemQuitRedirect.h
    test_system_quit_redirect.cpp
    TestSupportedFormats.h
    test_supported_formats.cpp
    TestFilePreview.h
    test_file_preview.cpp
    TestMissingRoute.h
    test_missing_route.cpp
    ${VISIVO_ROOT}/src/gui/BackendLocality.cpp
    TestBusyFeedback.h
    test_busy_feedback.cpp
    TestVrLink.h
    test_vr_link.cpp
    TestVrPair.h
    test_vr_pair.cpp
    TestSharedSelection.h
    test_shared_selection.cpp
    TestSharedSelectionLink.h
    test_shared_selection_link.cpp
    ${VISIVO_ROOT}/src/app/SharedSelectionLink.cpp
    TestVrHeadsetHub.h
    test_vr_headset_hub.cpp
    ${VISIVO_ROOT}/src/app/HeadsetHubLink.cpp
    TestBackendProcessTree.h
    test_backend_process_tree.cpp
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
    Qt::Concurrent   # the quit drain's test of a worker waiting on a queued job
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

# A process of its own: QCoreApplication::exit() inside it would end the event
# loops of every later test in VisIVOTests (TaskQuitDrain, issue #133).
add_executable(VisIVOQuitExitTest test_quit_exit_main.cpp)
target_include_directories(VisIVOQuitExitTest PRIVATE ${VISIVO_ROOT}/src ${PROJECT_BINARY_DIR})
target_link_libraries(VisIVOQuitExitTest PRIVATE visivo_test_support Qt::Concurrent)
add_test(NAME VisIVOQuitExit COMMAND VisIVOQuitExitTest)
set_tests_properties(VisIVOQuitExit PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
add_test(NAME VisIVOQuitExitGuiFollower COMMAND VisIVOQuitExitTest --gui-follower)
set_tests_properties(VisIVOQuitExitGuiFollower PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
add_test(NAME VisIVOQuitExitRender COMMAND VisIVOQuitExitTest --render)
set_tests_properties(VisIVOQuitExitRender PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
add_test(NAME VisIVOQuitExitRenderAboutToQuit COMMAND VisIVOQuitExitTest --render-about-to-quit)
set_tests_properties(VisIVOQuitExitRenderAboutToQuit PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
add_test(NAME VisIVOQuitExitRenderWhileQuitting COMMAND VisIVOQuitExitTest --render-quitting)
set_tests_properties(VisIVOQuitExitRenderWhileQuitting PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
add_test(NAME VisIVOQuitExitRenderSessions COMMAND VisIVOQuitExitTest --render-sessions)
set_tests_properties(VisIVOQuitExitRenderSessions PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
add_test(NAME VisIVOQuitExitRenderSessionsAboutToQuit
         COMMAND VisIVOQuitExitTest --render-sessions-about-to-quit)
set_tests_properties(VisIVOQuitExitRenderSessionsAboutToQuit PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
