cmake_minimum_required(VERSION 3.18)
project(voicetts)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Prebuilt sherpa-onnx
add_library(sherpa-onnx-c-api SHARED IMPORTED)
set_target_properties(sherpa-onnx-c-api PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/jni/sherpa-onnx/libsherpa-onnx-c-api.so
)

add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/jni/sherpa-onnx/libonnxruntime.so
)

add_library(voicetts SHARED jni/voicetts.cpp)

target_include_directories(voicetts PRIVATE jni)

target_compile_options(voicetts PRIVATE
    -O2 -fPIC -fvisibility=hidden -std=c++17
)

target_compile_definitions(voicetts PRIVATE ANDROID)

target_link_libraries(voicetts
    sherpa-onnx-c-api
    onnxruntime
    log android dl m
)
