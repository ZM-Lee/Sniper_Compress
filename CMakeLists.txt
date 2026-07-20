cmake_minimum_required(VERSION 3.16)
project(rmcc_sniper VERSION 0.1.0 LANGUAGES CXX)

if(COMPRESS_ENABLE_TENSORRT)
    enable_language(CUDA)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OpenVINO
find_package(OpenVINO REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)
find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h)
find_library(MOSQUITTO_LIBRARY mosquitto)
if(NOT MOSQUITTO_INCLUDE_DIR OR NOT MOSQUITTO_LIBRARY)
    message(FATAL_ERROR "libmosquitto not found. Install libmosquitto-dev or set MOSQUITTO_INCLUDE_DIR/MOSQUITTO_LIBRARY.")
endif()

option(COMPRESS_ENABLE_TENSORRT "Enable optional TensorRT g_a sender backend" OFF)
option(RMCC_SNIPER_BUILD_TOOLS "Build developer test and dump tools" OFF)

set(RMCC_SNIPER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

if(COMPRESS_ENABLE_TENSORRT)
    find_path(TENSORRT_INCLUDE_DIR NvInfer.h)
    find_path(CUDA_RUNTIME_INCLUDE_DIR cuda_runtime_api.h
        PATHS
            /usr/local/cuda/include
            /usr/local/cuda-12.6/include
            /usr/local/cuda-12.6/targets/x86_64-linux/include
            /usr/local/cuda-12.5/include
            /usr/local/cuda-12.5/targets/x86_64-linux/include
            /usr/local/cuda-12.4/include
            /usr/local/cuda-12.4/targets/x86_64-linux/include
            /usr/local/cuda-12.3/include
            /usr/local/cuda-12.3/targets/x86_64-linux/include)
    find_library(TENSORRT_LIBRARY nvinfer)
    find_library(CUDART_LIBRARY cudart
        PATHS
            /usr/local/cuda/lib64
            /usr/local/cuda-12.6/lib64
            /usr/local/cuda-12.6/targets/x86_64-linux/lib
            /usr/local/cuda-12.5/lib64
            /usr/local/cuda-12.5/targets/x86_64-linux/lib
            /usr/local/cuda-12.4/lib64
            /usr/local/cuda-12.4/targets/x86_64-linux/lib
            /usr/local/cuda-12.3/lib64
            /usr/local/cuda-12.3/targets/x86_64-linux/lib
        NO_DEFAULT_PATH)
    if(NOT CUDART_LIBRARY)
        find_library(CUDART_LIBRARY cudart)
    endif()
    if(NOT TENSORRT_INCLUDE_DIR OR NOT CUDA_RUNTIME_INCLUDE_DIR OR
       NOT TENSORRT_LIBRARY OR NOT CUDART_LIBRARY)
        message(WARNING
            "COMPRESS_ENABLE_TENSORRT=ON requested, but NvInfer.h/libnvinfer/libcudart "
            "were not all found. Building OpenVINO-only with TensorRT runtime error stubs.")
        set(COMPRESS_TENSORRT_AVAILABLE OFF)
    else()
        set(COMPRESS_TENSORRT_AVAILABLE ON)
    endif()
endif()

# --- shared library: librmcc_sniper.so ---
add_library(rmcc_sniper SHARED
    src/compress/compressor.cpp
    src/compress/preprocess.cpp
    src/compress/mbt_encoder.cpp
    src/compress/ga_encoder.cpp
    src/compress/tensorrt_ga_encoder.cpp
    src/compress/entropy_coder.cpp
    src/compress/inference_engine.cpp
    src/compress/message.cpp
    src/compress/shm_ring.cpp
)
set_target_properties(rmcc_sniper PROPERTIES OUTPUT_NAME rmcc_sniper)
target_include_directories(rmcc_sniper PUBLIC
    ${RMCC_SNIPER_ROOT}/include
)
target_link_libraries(rmcc_sniper PUBLIC openvino::runtime)
if(COMPRESS_TENSORRT_AVAILABLE)
    target_sources(rmcc_sniper PRIVATE src/compress/preprocess_cuda.cu)
    target_compile_definitions(rmcc_sniper PRIVATE COMPRESS_ENABLE_TENSORRT=1)
    target_include_directories(rmcc_sniper PRIVATE
        ${TENSORRT_INCLUDE_DIR}
        ${CUDA_RUNTIME_INCLUDE_DIR}
    )
    target_link_libraries(rmcc_sniper PRIVATE ${TENSORRT_LIBRARY} ${CUDART_LIBRARY})
    set_target_properties(rmcc_sniper PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
endif()
# Resolve project root (where models/ lives) so the binary can be run from any CWD
target_compile_definitions(rmcc_sniper PRIVATE "RM_PROJECT_ROOT=${RMCC_SNIPER_ROOT}")

# --- CAN command receiver and ROI controller ---
add_library(rmcc_can STATIC
    src/can/can.cpp
)
target_include_directories(rmcc_can PUBLIC
    ${RMCC_SNIPER_ROOT}/include
)
target_link_libraries(rmcc_can PUBLIC Threads::Threads)

# --- CLI: compress ---
add_executable(compress
    src/cli/main.cpp
    src/serial/serial_tx.cpp
    src/mqtt/mqtt_sender.cpp
    src/mqtt/rmcc.pb.cpp
)
target_include_directories(compress PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${RMCC_SNIPER_ROOT}/include
    ${MOSQUITTO_INCLUDE_DIR}
    ${Protobuf_INCLUDE_DIRS}
)
target_link_libraries(compress PRIVATE
    rmcc_sniper
    rmcc_can
    ${MOSQUITTO_LIBRARY}
    protobuf::libprotobuf
)

if(RMCC_SNIPER_BUILD_TOOLS)
    # --- QVRF dry-run parity harness ---
    add_executable(qvrf_dryrun_dump
        src/test/qvrf_dryrun_dump.cpp
    )
    target_include_directories(qvrf_dryrun_dump PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${RMCC_SNIPER_ROOT}/include
    )
    target_link_libraries(qvrf_dryrun_dump PRIVATE rmcc_sniper)
endif()

# --- CLI: camera_capture ---
set(DAHENG_ROOT "" CACHE PATH "Daheng Galaxy SDK root")
set(DAHENG_INCLUDE_HINTS
    "${RMCC_SNIPER_ROOT}/include"
    "${DAHENG_ROOT}/include"
    "${DAHENG_ROOT}/inc"
    "${DAHENG_ROOT}/Galaxy_camera/inc"
    "${DAHENG_ROOT}/Galaxy_camera/include")
set(DAHENG_LIB_HINTS
    "${DAHENG_ROOT}/lib"
    "${DAHENG_ROOT}/lib/x86_64"
    "${DAHENG_ROOT}/Galaxy_camera/lib"
    "${DAHENG_ROOT}/Galaxy_camera/lib/x86_64"
    "/usr/lib"
    "/usr/local/lib")
find_path(DAHENG_INCLUDE_DIR
    NAMES daheng_camera/driver/GxIAPI.h GxIAPI.h
    PATHS ${DAHENG_INCLUDE_HINTS}
    NO_DEFAULT_PATH)
if(NOT DAHENG_INCLUDE_DIR)
    find_path(DAHENG_INCLUDE_DIR NAMES daheng_camera/driver/GxIAPI.h GxIAPI.h)
endif()
find_library(DAHENG_GXIAPI_LIBRARY
    NAMES gxiapi
    PATHS ${DAHENG_LIB_HINTS}
    NO_DEFAULT_PATH)
if(NOT DAHENG_GXIAPI_LIBRARY)
    find_library(DAHENG_GXIAPI_LIBRARY NAMES gxiapi)
endif()
if(DAHENG_INCLUDE_DIR AND DAHENG_GXIAPI_LIBRARY)
    add_executable(camera_capture
        src/camera/camera_capture.cpp
    )
    target_include_directories(camera_capture PRIVATE
        ${DAHENG_INCLUDE_DIR}
        ${RMCC_SNIPER_ROOT}/include
    )
    target_link_libraries(camera_capture PRIVATE
        rmcc_sniper
        ${DAHENG_GXIAPI_LIBRARY}
        pthread)
    get_filename_component(DAHENG_GXIAPI_LIB_DIR "${DAHENG_GXIAPI_LIBRARY}" DIRECTORY)
    set_target_properties(camera_capture PROPERTIES
        BUILD_RPATH "${DAHENG_GXIAPI_LIB_DIR}"
        INSTALL_RPATH "${DAHENG_GXIAPI_LIB_DIR}"
    )
    message(STATUS "Daheng camera enabled: include=${DAHENG_INCLUDE_DIR}, gxiapi=${DAHENG_GXIAPI_LIBRARY}")
else()
    message(WARNING
        "Daheng Galaxy SDK not found; camera_capture target disabled. "
        "Set DAHENG_ROOT to a Galaxy SDK root containing headers and libgxiapi.")
endif()

if(RMCC_SNIPER_BUILD_TOOLS)
    # --- tests (conditional: skip missing files) ---
    enable_testing()

    add_executable(test_can src/test/test_can.cpp)
    target_link_libraries(test_can PRIVATE rmcc_can)
    add_test(NAME can_tests COMMAND test_can)

    set(TEST_SOURCES "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/test/test_compressor.cpp")
        list(APPEND TEST_SOURCES src/test/test_compressor.cpp)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/test/test_entropy.cpp")
        list(APPEND TEST_SOURCES src/test/test_entropy.cpp)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/test/test_message.cpp")
        list(APPEND TEST_SOURCES src/test/test_message.cpp)
    endif()

    if(TEST_SOURCES)
        add_executable(test_compress ${TEST_SOURCES})
        target_include_directories(test_compress PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${RMCC_SNIPER_ROOT}/include
        )
        target_link_libraries(test_compress PRIVATE rmcc_sniper)
        add_test(NAME compress_tests COMMAND test_compress)
        message(STATUS "Tests enabled: ${TEST_SOURCES}")
    else()
        message(STATUS "No test sources found — skipping test target")
    endif()
endif()
