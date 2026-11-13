cmake_minimum_required(VERSION 3.16)

project(onekvm_video_soph_mmf LANGUAGES C CXX)

option(BUILD_TESTING "Build host-side unit tests" OFF)
include(CTest)
include(GNUInstallDirs)

option(ONEKVM_BUILD_MMF_BACKEND "Build the NanoKVM cvi_mpi backend (soph-mmf.so)" ON)
option(ONEKVM_BUILD_V4L2_BACKEND
    "Build the NanoKVM V4L2 backend without cvi_mpi (soph-v4l2.so)" ON)
option(ONEKVM_USE_VENDOR_TUNE_FLAGS
    "Build with Sophgo's vendor GCC SG2002 tune flags" ON)

function(onekvm_enable_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wall>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wextra>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wpedantic>)
endfunction()

if(BUILD_TESTING)
    add_executable(vi_direct_policy_test tests/vi-direct-policy-test.cpp)
    target_include_directories(vi_direct_policy_test PRIVATE src)
    target_compile_features(vi_direct_policy_test PRIVATE cxx_std_17)
    target_compile_options(vi_direct_policy_test PRIVATE -UNDEBUG)
    onekvm_enable_warnings(vi_direct_policy_test)
    add_test(NAME vi-direct-policy COMMAND vi_direct_policy_test)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME lt6911-receiver
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/lt6911-receiver-test.py)
    add_executable(crypto_policy_test tests/crypto-policy-test.cpp src/onekvm_crypto_backend.cpp)
    target_include_directories(crypto_policy_test PRIVATE include)
    target_compile_features(crypto_policy_test PRIVATE cxx_std_17)
    target_compile_options(crypto_policy_test PRIVATE -UNDEBUG)
    target_link_options(crypto_policy_test PRIVATE -Wl,--wrap=open -Wl,--wrap=ioctl -Wl,--wrap=close -Wl,--wrap=read)
    onekvm_enable_warnings(crypto_policy_test)
    foreach(caps IN ITEMS 0 4 11 15 -1)
        foreach(mode IN ITEMS Y N missing error empty)
            add_test(NAME crypto-policy-${caps}-${mode} COMMAND crypto_policy_test ${caps} ${mode})
        endforeach()
    endforeach()
    add_executable(d_flash_test tests/d-flash.cpp)
    target_compile_features(d_flash_test PRIVATE cxx_std_17)
    target_compile_options(d_flash_test PRIVATE -fsanitize=address,undefined)
    target_link_options(d_flash_test PRIVATE -fsanitize=address,undefined)
    onekvm_enable_warnings(d_flash_test)
    add_test(NAME d-flash COMMAND d_flash_test)
    add_executable(d_edid_cache_test tests/d-edid-cache.cpp)
    target_include_directories(d_edid_cache_test PRIVATE include src)
    target_compile_features(d_edid_cache_test PRIVATE cxx_std_17)
    target_link_options(d_edid_cache_test PRIVATE -Wl,--wrap=open)
    add_test(NAME d-edid-cache COMMAND d_edid_cache_test)
    add_executable(capture_startup_test tests/capture-startup-test.cpp)
    target_include_directories(capture_startup_test PRIVATE include)
    target_compile_features(capture_startup_test PRIVATE cxx_std_17)
    onekvm_enable_warnings(capture_startup_test)
    add_test(NAME capture-startup COMMAND capture_startup_test)

    add_executable(edid_apply_test tests/edid-apply.cpp src/edid.cpp)
    target_include_directories(edid_apply_test PRIVATE include src)
    target_compile_features(edid_apply_test PRIVATE cxx_std_17)
    onekvm_enable_warnings(edid_apply_test)
    add_test(NAME edid-apply-cube COMMAND edid_apply_test cube)
    add_test(NAME edid-apply-unverified COMMAND edid_apply_test unverified)
    add_test(NAME edid-apply-pcie COMMAND edid_apply_test pcie)
    add_test(NAME edid-apply-d-cube COMMAND edid_apply_test d-cube)
    add_test(NAME edid-apply-d-pcie COMMAND edid_apply_test d-pcie)

    add_executable(c_shadow_test tests/c-shadow.cpp)
    target_compile_features(c_shadow_test PRIVATE cxx_std_17)
    onekvm_enable_warnings(c_shadow_test)
    add_test(NAME c-shadow COMMAND c_shadow_test)

    add_executable(uxc_shadow_test tests/uxc-shadow.cpp)
    target_compile_features(uxc_shadow_test PRIVATE cxx_std_17)
    onekvm_enable_warnings(uxc_shadow_test)
    add_test(NAME uxc-shadow COMMAND uxc_shadow_test)

    add_executable(venc_stream_map_test tests/venc-stream-map-test.c)
    target_compile_features(venc_stream_map_test PRIVATE c_std_11)
    onekvm_enable_warnings(venc_stream_map_test)
    add_test(NAME venc-stream-map COMMAND venc_stream_map_test)

    add_executable(packet_buffer_pool_test tests/packet-buffer-pool-test.cpp)
    target_include_directories(packet_buffer_pool_test PRIVATE include)
    target_compile_features(packet_buffer_pool_test PRIVATE cxx_std_17)
    onekvm_enable_warnings(packet_buffer_pool_test)
    add_test(NAME packet-buffer-pool COMMAND packet_buffer_pool_test)

    add_executable(snapshot_admission_test tests/snapshot-admission-test.cpp)
    target_include_directories(snapshot_admission_test PRIVATE include)
    target_compile_features(snapshot_admission_test PRIVATE cxx_std_17)
    onekvm_enable_warnings(snapshot_admission_test)
    add_test(NAME snapshot_admission_test COMMAND snapshot_admission_test)

    add_executable(input_resolution_tracker_test
        tests/input-resolution-tracker-test.cpp)
    target_include_directories(input_resolution_tracker_test PRIVATE include)
    target_compile_features(input_resolution_tracker_test PRIVATE cxx_std_17)
    onekvm_enable_warnings(input_resolution_tracker_test)
    add_test(NAME input-resolution-tracker COMMAND input_resolution_tracker_test)

    add_executable(vi_fps_parser_test tests/vi-fps-parser-test.cpp)
    target_compile_features(vi_fps_parser_test PRIVATE cxx_std_17)
    onekvm_enable_warnings(vi_fps_parser_test)
    add_test(NAME vi-fps-parser COMMAND vi_fps_parser_test)

    add_executable(output_fps_test tests/output-fps-test.cpp)
    target_include_directories(output_fps_test PRIVATE include)
    target_compile_features(output_fps_test PRIVATE cxx_std_17)
    onekvm_enable_warnings(output_fps_test)
    add_test(NAME output-fps COMMAND output_fps_test)

    add_executable(h264_annexb_test tests/h264-annexb-test.cpp)
    target_compile_features(h264_annexb_test PRIVATE cxx_std_17)
    onekvm_enable_warnings(h264_annexb_test)
    add_test(NAME h264-annexb COMMAND h264_annexb_test)

    add_executable(hw_latency_parser_test tests/hw-latency-parser-test.cpp)
    target_compile_features(hw_latency_parser_test PRIVATE cxx_std_17)
    onekvm_enable_warnings(hw_latency_parser_test)
    add_test(NAME hw-latency-parser COMMAND hw_latency_parser_test)

    add_executable(no_signal_frame_test
        tests/no-signal-frame-test.cpp
        tests/no_signal_h264_fixture.cpp
        src/no_signal.cpp)
    target_include_directories(no_signal_frame_test PRIVATE include src)
    target_compile_features(no_signal_frame_test PRIVATE cxx_std_17)
    onekvm_enable_warnings(no_signal_frame_test)
    add_test(NAME no-signal-frame COMMAND no_signal_frame_test)

    add_executable(edid_board_test tests/edid-board-test.cpp)
    target_include_directories(edid_board_test PRIVATE src)
    target_compile_features(edid_board_test PRIVATE cxx_std_17)
    onekvm_enable_warnings(edid_board_test)
    add_test(NAME edid-board COMMAND edid_board_test)

    add_executable(lt6911_info_test tests/lt6911-info-test.cpp)
    target_compile_features(lt6911_info_test PRIVATE cxx_std_17)
    onekvm_enable_warnings(lt6911_info_test)
    add_test(NAME lt6911-info COMMAND lt6911_info_test)
endif()

if(NOT ONEKVM_BUILD_MMF_BACKEND AND NOT ONEKVM_BUILD_V4L2_BACKEND)
    return()
endif()

# Upper layers shared by both runtimes. They reach the hardware only through
# the onekvm::mmf interface in src/mmf.hpp.
set(ONEKVM_BACKEND_COMMON_SOURCES
    src/lt6911_bridge.c
    src/no_signal.cpp
    src/onekvm_crypto_backend.cpp
    src/onekvm_video_backend.cpp
    src/onekvm_video_encoder.cpp
    src/onekvm_video_source.cpp
    src/onekvm_video_snapshot.cpp
    src/edid.cpp
    src/lt6911_edid.cpp)

# The version script makes the two OneKVM query functions the only public ABI.
set(ONEKVM_BACKEND_VERSION_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/src/onekvm_backend.map")
set(ONEKVM_VIDEO_BACKEND_INSTALL_DIR
    "${CMAKE_INSTALL_LIBDIR}/onekvm/video-backends"
    CACHE STRING "Install directory for OneKVM video backend DSOs")

function(onekvm_backend_target target output_name)
    target_include_directories(${target} BEFORE PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    if(ONEKVM_USE_VENDOR_TUNE_FLAGS)
        target_compile_options(${target} PRIVATE
            -mcpu=c906fdv
            -march=rv64imafdcv0p7xthead
            -mcmodel=medany
            -mabi=lp64d
            -Os)
    endif()
    target_compile_features(${target} PRIVATE cxx_std_17)
    onekvm_enable_warnings(${target})
    set_target_properties(${target} PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        CXX_EXTENSIONS OFF
        OUTPUT_NAME "${output_name}"
        PREFIX ""
        POSITION_INDEPENDENT_CODE ON
        VISIBILITY_INLINES_HIDDEN ON)
    target_link_options(${target} PRIVATE
        "-Wl,-Bsymbolic"
        "-Wl,--version-script=${ONEKVM_BACKEND_VERSION_SCRIPT}")
    set_property(TARGET ${target} APPEND PROPERTY
        LINK_DEPENDS "${ONEKVM_BACKEND_VERSION_SCRIPT}")
    # Sophgo's GCC 10 linker places .riscv.attributes immediately after a full
    # program-header table. Newer binutils tries to add PT_RISCV_ATTRIBUTES
    # while stripping and cannot grow that table. The dynamic loader uses
    # e_flags for the RVC/double-float ABI; removing this descriptive section
    # keeps the ELF compatible with Yocto's strip and objcopy.
    if(NOT CMAKE_OBJCOPY)
        message(FATAL_ERROR "CMAKE_OBJCOPY is required for the RISC-V ELF fixup")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_OBJCOPY}" --remove-section=.riscv.attributes
            "$<TARGET_FILE:${target}>"
        VERBATIM)
    install(TARGETS ${target}
        LIBRARY DESTINATION "${ONEKVM_VIDEO_BACKEND_INSTALL_DIR}")
endfunction()

if(ONEKVM_BUILD_V4L2_BACKEND)
    # soph-vi / soph-vpss / soph-venc / soph-jpegenc nodes; no vendor runtime.
    add_library(soph_v4l2 SHARED
        ${ONEKVM_BACKEND_COMMON_SOURCES}
        src/v4l2_node.cpp
        src/v4l2_capture.cpp
        src/v4l2_h26x.cpp
        src/v4l2_jpeg.cpp)
    onekvm_backend_target(soph_v4l2 soph-v4l2)
    target_link_libraries(soph_v4l2 PRIVATE pthread)
endif()

if(ONEKVM_BUILD_MMF_BACKEND)
    set(CVI_MPI_ROOT "" CACHE PATH "Built Sophgo cvi_mpi tree")
    if(NOT CVI_MPI_ROOT)
        message(FATAL_ERROR "CVI_MPI_ROOT must point to a built Sophgo cvi_mpi tree")
    endif()

    set(CVI_MPI_SENSOR_DIR
        "${CVI_MPI_ROOT}/component/isp/sensor/cv182x/lontium_lt6911")

    add_library(soph_mmf SHARED
        ${ONEKVM_BACKEND_COMMON_SOURCES}
        src/mmf_runtime.cpp
        src/mmf_jpeg.cpp
        src/mmf_h26x.cpp
        src/mmf_capture.cpp
        src/mmf_state.cpp
        src/lt6911_sample_platform.c
        src/lt6911_sample_sensor.c
        "${CVI_MPI_ROOT}/sample/common/sample_common_sys.c"
        "${CVI_MPI_ROOT}/sample/common/sample_common_vpss.c"
        "${CVI_MPI_ROOT}/sample/common/sample_common_vi.c"
        "${CVI_MPI_ROOT}/sample/common/sample_common_isp.c"
        "${CVI_MPI_SENSOR_DIR}/lt6911_cmos.c")

    target_include_directories(soph_mmf PRIVATE
        "${CVI_MPI_ROOT}/component/panel/cv181x"
        "${CVI_MPI_ROOT}/include"
        "${CVI_MPI_ROOT}/modules/isp/include/cv181x"
        "${CVI_MPI_ROOT}/sample/common"
        "${CVI_MPI_ROOT}/component/isp/common"
        "${CVI_MPI_ROOT}/3rdparty/inih"
        "${CVI_MPI_SENSOR_DIR}")

    target_compile_definitions(soph_mmf PRIVATE
        USE_USER_SEN_DRIVER=1
        SENSOR_LONTIUM_LT6911
        SENSOR2_TYPE=LONTIUM_LT6911_2M_60FPS_8BIT)
    onekvm_backend_target(soph_mmf soph-mmf)

    set(CVI_MPI_LIB_DIR "${CVI_MPI_ROOT}/lib")
    # Sophgo's cvi_mpi DSOs omit their cross-library DT_NEEDED metadata. Keep
    # the complete runtime set attached to this backend so dlopen can resolve
    # symbols between vendor libraries regardless of their order on the link
    # line.
    target_link_options(soph_mmf PRIVATE "-Wl,--no-as-needed")
    target_link_libraries(soph_mmf PRIVATE
        "${CVI_MPI_LIB_DIR}/libcvi_bin.so"
        "${CVI_MPI_LIB_DIR}/libae.so"
        "${CVI_MPI_LIB_DIR}/libaf.so"
        "${CVI_MPI_LIB_DIR}/libawb.so"
        "${CVI_MPI_LIB_DIR}/libcvi_bin_isp.so"
        "${CVI_MPI_LIB_DIR}/libisp_algo.so"
        "${CVI_MPI_LIB_DIR}/libisp.so"
        "${CVI_MPI_LIB_DIR}/libsys.so"
        "${CVI_MPI_LIB_DIR}/libvi.so"
        "${CVI_MPI_LIB_DIR}/libvpss.so"
        "${CVI_MPI_LIB_DIR}/libvo.so"
        "${CVI_MPI_LIB_DIR}/librgn.so"
        "${CVI_MPI_LIB_DIR}/libgdc.so"
        "${CVI_MPI_LIB_DIR}/libvdec.so"
        "${CVI_MPI_LIB_DIR}/libvenc.so"
        "${CVI_MPI_LIB_DIR}/3rd/libini.so"
        pthread)
    # Keep vendor sample-common and LT6911 references bound inside this DSO.
    target_link_options(soph_mmf PRIVATE
        "-Wl,--wrap=CVI_ISP_SensorRegCallBack"
        "-Wl,--wrap=CVI_VI_SetChnAttr")
endif()

install(FILES packaging/onekvm/nanokvm-device-backend.json
    DESTINATION "${CMAKE_INSTALL_DATADIR}/onekvm/device-backends"
    RENAME nanokvm.json)
install(FILES packaging/onekvm/nanokvm-system-plugin.json
    DESTINATION "${CMAKE_INSTALL_DATADIR}/onekvm/system-plugins"
    RENAME nanokvm-device.json)
install(FILES
    include/onekvm/crypto_backend_v1.h
    include/onekvm/video_backend_v1.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/onekvm")
install(DIRECTORY assets/edid/presets/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/onekvm/edid/presets"
    FILES_MATCHING PATTERN "*.bin")

if(BUILD_TESTING)
    add_executable(managed_snapshot_smoke tests/managed-snapshot-smoke.cpp)
    target_include_directories(managed_snapshot_smoke PRIVATE include)
    target_compile_features(managed_snapshot_smoke PRIVATE cxx_std_17)
    target_link_libraries(managed_snapshot_smoke PRIVATE dl pthread)
    onekvm_enable_warnings(managed_snapshot_smoke)

    add_executable(bound_reader_stress tests/bound-reader-stress.cpp)
    target_include_directories(bound_reader_stress PRIVATE include)
    target_compile_features(bound_reader_stress PRIVATE cxx_std_17)
    target_link_libraries(bound_reader_stress PRIVATE dl pthread)
    onekvm_enable_warnings(bound_reader_stress)
    install(TARGETS bound_reader_stress managed_snapshot_smoke
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
