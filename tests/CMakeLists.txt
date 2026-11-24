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

# The headset pairing defaults header (VrPairingDefaults.h); a no-op when the
# main build already included it.
include(${VISIVO_ROOT}/cmake/VrPairingDefaults.cmake)

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
    ${VISIVO_ROOT}/src/app/BackendClient_CubeLod.cpp
    ${VISIVO_ROOT}/src/app/BackendClient_Products.cpp
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
    ${VISIVO_ROOT}/src/vtk/CubeLodPlan.cpp
    ${VISIVO_ROOT}/src/gui/PvPathGeometry.cpp
    ${VISIVO_ROOT}/src/gui/OrbitPath.cpp
    ${VISIVO_ROOT}/src/gui/ParticleFrame.cpp
    ${VISIVO_ROOT}/src/gui/CopilotStatus.cpp
    ${VISIVO_ROOT}/src/gui/CopilotActionRouter.cpp
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
    TestSliceDetailPlan.h
    test_slice_detail_plan.cpp
    TestLinkedWindowRegistry.h
    test_linked_window_registry.cpp
    TestLinkedCameraSync.h
    test_linked_camera_sync.cpp
    TestSnapshotTimelineSteps.h
    test_snapshot_timeline_steps.cpp
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
    TestSimulationVolume.h
    TestPlutoCartesianView.h
    test_simulation_volume.cpp
    test_pluto_cartesian_view.cpp
    TestCubeLodGeometry.h
    test_cube_lod_geometry.cpp
    TestCubeLodPlan.h
    test_cube_lod_plan.cpp
    TestCubeLodGate.h
    test_cube_lod_gate.cpp
    TestCubeLodAssembly.h
    test_cube_lod_assembly.cpp
    TestCubeLodAuto.h
    test_cube_lod_auto.cpp
    TestSpectralAxisInfer.h
    test_spectral_axis_infer.cpp
    TestKinematicLevelField.h
    test_kinematic_level_field.cpp
    TestRayVoxelPick.h
    test_ray_voxel_pick.cpp
    TestSedBuilder.h
    test_sed_builder.cpp
    TestSedPlotStyle.h
    test_sed_plot_style.cpp
    TestMolecularLineClient.h
    test_molecular_line_client.cpp
    TestLteCoRequest.h
    test_lte_co_request.cpp
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
    TestRemoteCameraPreset.h
    test_remote_camera_preset.cpp
    TestCatalogueSelection.h
    test_catalogue_selection.cpp
    TestRemoteRenderQueue.h
    test_remote_render_queue.cpp
    TestRenderSessionCalls.h
    test_render_session_calls.cpp
    ${VISIVO_ROOT}/src/gui/RenderSessionCalls.cpp
    TestRegionFullRes.h
    test_region_full_res.cpp
    TestContourSettings.h
    test_contour_settings.cpp
    TestLiveRegionSpectra.h
    test_live_region_spectra.cpp
    TestRegionModel.h
    test_region_model.cpp
    TestRegionFlux.h
    test_region_flux.cpp
    TestRegionMeasure.h
    test_region_measure.cpp
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
    TestIsoLevels.h
    test_iso_levels.cpp
    test_colour_bar_ticks.cpp
    TestSequentialColors.h
    test_sequential_colors.cpp
    TestImageLayerContext.h
    test_image_layer_context.cpp
    TestProductPaneDisplay.h
    test_product_pane_display.cpp
    TestLayerCardGeometry.h
    test_layer_card_geometry.cpp
    TestLayerBlend.h
    test_layer_blend.cpp
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
    TestRenderExportPlan.h
    test_render_export_plan.cpp
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
    TestParticleLightReport.h
    test_particle_light_report.cpp
    TestDiagnosticsTask.h
    test_diagnostics_task.cpp
    TestParticleProfiles.h
    test_particle_profiles.cpp
    TestParticleTableRule.h
    test_particle_table_rule.cpp
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
    TestRadioDisplay.h
    test_radio_display.cpp
    TestOpenJobMerge.h
    test_open_job_merge.cpp
    TestTaskQuitDrain.h
    test_task_quit_drain.cpp
    TestVelocityLodGeometry.h
    test_velocity_lod_geometry.cpp
    TestSolidSplats.h
    test_solid_splats.cpp
    TestRaytraceViewRules.h
    test_raytrace_view_rules.cpp
    TestSystemQuitRedirect.h
    test_system_quit_redirect.cpp
    TestSupportedFormats.h
    test_supported_formats.cpp
    TestFilePreview.h
    test_file_preview.cpp
    TestMissingRoute.h
    test_missing_route.cpp
    TestWorkspaceExportsView.h
    test_workspace_exports_view.cpp
    TestBackendWebrtcEnv.h
    test_backend_webrtc_env.cpp
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
    TestVrSpectator.h
    test_vr_spectator.cpp
    ${VISIVO_ROOT}/src/app/VrSpectatorFeed.cpp
    TestVrHeadsetApp.h
    test_vr_headset_app.cpp
    ${VISIVO_ROOT}/src/app/VrAppLauncher.cpp
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
    Qt::Gui          # QColor (the isosurface levels, IsoLevels.h); no window is made
    Qt::Concurrent   # the quit drain's test of a worker waiting on a queued job
)
# Let the contract test locate the shared header manifest in both build layouts.
target_compile_definitions(VisIVOTests PRIVATE
    VISIVO_REPO_ROOT="${VISIVO_ROOT}"
    VISIVO_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

# The sky matcher of AstroUtils (issue #522) needs libwcs and CFITSIO, which the
# standalone CI configuration does not have: its test is built with the main project only.
if (TARGET visivo_shared_core)
    target_sources(VisIVOTests PRIVATE TestAstroMatch.h test_astro_match.cpp)
    target_link_libraries(VisIVOTests PRIVATE visivo_shared_core)
    target_compile_definitions(VisIVOTests PRIVATE VISIVO_TEST_ASTRO_MATCH)
endif()

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

# ── Widget tests ─────────────────────────────────────────────────────────────
# A process of its own that links QtWidgets (VisIVOTests deliberately does
# not): a window's real form, built by uic with its VTK view stubbed, driven
# with QTest's mouse. Only the base Qt is needed, as for the suite above.
find_package(Qt6 COMPONENTS Widgets)
if (TARGET Qt6::Widgets)
    qt_wrap_ui(VISIVO_WIDGET_TEST_FORMS ${VISIVO_ROOT}/ui/vtkWindowCatalogue3D.ui)
    add_executable(VisIVOSizeSliderDragTest
        test_size_slider_drag.cpp
        ${VISIVO_WIDGET_TEST_FORMS}
    )
    set_target_properties(VisIVOSizeSliderDragTest PROPERTIES AUTOMOC ON)
    target_include_directories(VisIVOSizeSliderDragTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/widget_stubs
        ${VISIVO_ROOT}/src/gui
        ${CMAKE_CURRENT_BINARY_DIR}
    )
    target_link_libraries(VisIVOSizeSliderDragTest PRIVATE Qt::Widgets Qt::Test)
    add_test(NAME VisIVOSizeSliderDrag COMMAND VisIVOSizeSliderDragTest)
    set_tests_properties(VisIVOSizeSliderDrag PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

    # Temporary status-bar messages keep their room (issue #508).
    add_executable(VisIVOStatusMessageRoomTest
        test_status_message_room.cpp
        ${VISIVO_ROOT}/src/gui/StatusMessageRoom.cpp
    )
    set_target_properties(VisIVOStatusMessageRoomTest PROPERTIES AUTOMOC ON)
    target_include_directories(VisIVOStatusMessageRoomTest PRIVATE ${VISIVO_ROOT}/src/gui)
    target_link_libraries(VisIVOStatusMessageRoomTest PRIVATE Qt::Widgets Qt::Test)
    add_test(NAME VisIVOStatusMessageRoom COMMAND VisIVOStatusMessageRoomTest)
    set_tests_properties(VisIVOStatusMessageRoom PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()

# ── Save View As render test (issue #528) ────────────────────────────────────
# Needs VTK, QtGui and QtSvg, so it is a process of its own and only built where
# they are found (the main build has them; the standalone CI build skips it). It
# renders a small scene offscreen and skips itself, with a message, when no GL
# context can be made.
if (NOT TARGET VTK::RenderingOpenGL2)
    find_package(VTK QUIET COMPONENTS CommonCore FiltersSources InteractionWidgets
                 RenderingAnnotation RenderingCore RenderingOpenGL2)
endif()
find_package(Qt6 QUIET COMPONENTS Gui Svg)
if (TARGET VTK::RenderingOpenGL2 AND TARGET Qt6::Gui AND TARGET Qt6::Svg)
    add_executable(VisIVORenderExportTest
        test_render_export_render.cpp
        ${VISIVO_ROOT}/src/vtk/RenderExport.cpp
    )
    set_target_properties(VisIVORenderExportTest PROPERTIES AUTOMOC ON)
    target_include_directories(VisIVORenderExportTest PRIVATE ${VISIVO_ROOT}/src)
    target_link_libraries(VisIVORenderExportTest PRIVATE
        Qt::Gui Qt::Svg Qt::Test
        VTK::CommonCore VTK::FiltersSources VTK::InteractionWidgets
        VTK::RenderingAnnotation VTK::RenderingCore VTK::RenderingOpenGL2)
    vtk_module_autoinit(TARGETS VisIVORenderExportTest
        MODULES VTK::RenderingOpenGL2 VTK::InteractionWidgets VTK::RenderingAnnotation)
    add_test(NAME VisIVORenderExport COMMAND VisIVORenderExportTest)
    set_tests_properties(VisIVORenderExport PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120 SKIP_REGULAR_EXPRESSION "SKIP")
endif()
