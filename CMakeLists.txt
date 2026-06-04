cmake_minimum_required(VERSION 3.18)
project(voicetts)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

add_library(voicetts SHARED jni/voicetts.cpp)

target_compile_options(voicetts PRIVATE
    -O2
    -fPIC
    -fvisibility=hidden
    -std=c++17
)

target_compile_definitions(voicetts PRIVATE ANDROID)

target_link_libraries(voicetts
    log
    android
    dl
    m
)
