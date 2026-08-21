cmake_minimum_required(VERSION 3.16)

project(onekvm_device_nanokvm_mmf LANGUAGES C CXX)

option(BUILD_TESTING "Build host-side unit tests" OFF)
include(CTest)
include(GNUInstallDirs)

option(ONEKVM_BUILD_MMF_BACKEND "Build the NanoKVM MMF backend" ON)
option(ONEKVM_USE_VENDOR_TUNE_FLAGS
    "Build with Sophgo's vendor GCC SG2002 tune flags" ON)

function(onekvm_enable_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wall>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wextra>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wpedantic>)
endfunction()

if(BUILD_TESTING)
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
        src/no_signal.cpp)
    target_include_directories(no_signal_frame_test PRIVATE include)
    target_compile_features(no_signal_frame_test PRIVATE cxx_std_17)
    onekvm_enable_warnings(no_signal_frame_test)
    add_test(NAME no-signal-frame COMMAND no_signal_frame_test)
endif()

if(NOT ONEKVM_BUILD_MMF_BACKEND)
    return()
endif()

set(CVI_MPI_ROOT "" CACHE PATH "Built Sophgo cvi_mpi tree")
if(NOT CVI_MPI_ROOT)
    message(FATAL_ERROR "CVI_MPI_ROOT must point to a built Sophgo cvi_mpi tree")
endif()

set(CVI_MPI_SENSOR_DIR
    "${CVI_MPI_ROOT}/component/isp/sensor/cv182x/lontium_lt6911")

set(MMF_SOURCES
    src/mmf_runtime.cpp
    src/mmf_jpeg.cpp
    src/mmf_h26x.cpp
    src/mmf_capture.cpp
    src/lt6911_sample_platform.c
    src/lt6911_sample_sensor.c
    src/lt6911_sensor_ctl.c
    src/no_signal.cpp
    src/onekvm_crypto_backend.cpp
    src/onekvm_video_backend.cpp
    src/onekvm_video_encoder.cpp
    src/onekvm_video_source.cpp)

add_library(kvm_mmf SHARED
    ${MMF_SOURCES}
    "${CVI_MPI_ROOT}/sample/common/sample_common_sys.c"
    "${CVI_MPI_ROOT}/sample/common/sample_common_vpss.c"
    "${CVI_MPI_ROOT}/sample/common/sample_common_vi.c"
    "${CVI_MPI_ROOT}/sample/common/sample_common_isp.c"
    "${CVI_MPI_SENSOR_DIR}/lt6911_cmos.c")

target_include_directories(kvm_mmf PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CVI_MPI_ROOT}/component/panel/cv181x"
    "${CVI_MPI_ROOT}/include"
    "${CVI_MPI_ROOT}/modules/isp/include/cv181x"
    "${CVI_MPI_ROOT}/sample/common"
    "${CVI_MPI_ROOT}/component/isp/common"
    "${CVI_MPI_ROOT}/3rdparty/inih"
    "${CVI_MPI_SENSOR_DIR}")

target_compile_definitions(kvm_mmf PRIVATE
    USE_USER_SEN_DRIVER=1
    SENSOR_LONTIUM_LT6911
    SENSOR2_TYPE=LONTIUM_LT6911_2M_60FPS_8BIT)
if(ONEKVM_USE_VENDOR_TUNE_FLAGS)
    target_compile_options(kvm_mmf PRIVATE
        -mcpu=c906fdv
        -march=rv64imafdcv0p7xthead
        -mcmodel=medany
        -mabi=lp64d
        -Os)
endif()
target_compile_features(kvm_mmf PRIVATE cxx_std_17)
onekvm_enable_warnings(kvm_mmf)
set_target_properties(kvm_mmf PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "nanokvm-mmf"
    PREFIX ""
    POSITION_INDEPENDENT_CODE ON
    VISIBILITY_INLINES_HIDDEN ON)

set(CVI_MPI_LIB_DIR "${CVI_MPI_ROOT}/lib")
# Sophgo's cvi_mpi DSOs omit their cross-library DT_NEEDED metadata.  Keep the
# complete runtime set attached to this backend so dlopen can resolve symbols
# between vendor libraries regardless of their order on the link line.
target_link_options(kvm_mmf PRIVATE "-Wl,--no-as-needed")
target_link_libraries(kvm_mmf PRIVATE
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

# Keep vendor sample-common and LT6911 references bound inside this DSO. The
# version script makes the two OneKVM query functions its only public ABI.
set(ONEKVM_BACKEND_VERSION_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/src/onekvm_backend.map")
target_link_options(kvm_mmf PRIVATE
    "-Wl,-Bsymbolic"
    "-Wl,--version-script=${ONEKVM_BACKEND_VERSION_SCRIPT}"
    "-Wl,--wrap=CVI_ISP_SensorRegCallBack")
set_property(TARGET kvm_mmf APPEND PROPERTY
    LINK_DEPENDS "${ONEKVM_BACKEND_VERSION_SCRIPT}")

# Sophgo's GCC 10 linker places .riscv.attributes immediately after a full
# program-header table. Newer binutils tries to add PT_RISCV_ATTRIBUTES while
# stripping and cannot grow that table. The dynamic loader uses e_flags for
# the RVC/double-float ABI; removing this descriptive section keeps the ELF
# compatible with Yocto's strip and objcopy.
if(NOT CMAKE_OBJCOPY)
    message(FATAL_ERROR "CMAKE_OBJCOPY is required for the RISC-V ELF fixup")
endif()
add_custom_command(TARGET kvm_mmf POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" --remove-section=.riscv.attributes
        "$<TARGET_FILE:kvm_mmf>"
    VERBATIM)

set(ONEKVM_VIDEO_BACKEND_INSTALL_DIR
    "${CMAKE_INSTALL_LIBDIR}/onekvm/video-backends"
    CACHE STRING "Install directory for OneKVM video backend DSOs")
install(TARGETS kvm_mmf
    LIBRARY DESTINATION "${ONEKVM_VIDEO_BACKEND_INSTALL_DIR}")
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
