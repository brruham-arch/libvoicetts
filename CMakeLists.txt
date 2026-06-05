cmake_minimum_required(VERSION 3.18)
project(voicetts)

# Target: armeabi-v7a, API 19
# Dipanggil via GitHub Actions dengan Android NDK

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ============================================================
# eSpeak-NG static library
# ============================================================
set(ESPEAK_SRC
    jni/espeak-ng/src/espeak-ng.c
    jni/espeak-ng/src/speech.c
    jni/espeak-ng/src/phoneme.c
    jni/espeak-ng/src/synthesize.c
    jni/espeak-ng/src/translate.c
    jni/espeak-ng/src/readclause.c
    jni/espeak-ng/src/setlengths.c
    jni/espeak-ng/src/numbers.c
    jni/espeak-ng/src/intonation.c
    jni/espeak-ng/src/wave.c
    jni/espeak-ng/src/voices.c
    jni/espeak-ng/src/soundicon.c
    jni/espeak-ng/src/mnemonics.c
    jni/espeak-ng/src/encoding.c
    jni/espeak-ng/src/error.c
    jni/espeak-ng/src/compiledict.c
    jni/espeak-ng/src/compilembrola.c
    jni/espeak-ng/src/dictionary.c
    jni/espeak-ng/src/fifo.c
    jni/espeak-ng/src/ieee80.c
    jni/espeak-ng/src/langopts.c
    jni/espeak-ng/src/mbrola.c
    jni/espeak-ng/src/syndata.c
    jni/espeak-ng/src/vowelnet.c
    jni/espeak-ng/src/wavegen.c
    jni/espeak-ng/src/ucd/ucd.c
)

add_library(espeak-ng STATIC ${ESPEAK_SRC})
target_include_directories(espeak-ng PUBLIC
    jni/espeak-ng/include
    jni/espeak-ng/src
)
target_compile_definitions(espeak-ng PRIVATE
    ANDROID
    USE_ASYNC=0
    LIBESPEAK_NG_EXPORT=
    ESPEAK_NG_DEFAULT_VOICE="id"
)
target_compile_options(espeak-ng PRIVATE -O2 -fPIC)

# ============================================================
# voicetts shared library
# ============================================================
add_library(voicetts SHARED jni/voicetts.cpp)

target_include_directories(voicetts PRIVATE
    jni/espeak-ng/include
)

target_compile_options(voicetts PRIVATE
    -O2
    -fPIC
    -fvisibility=hidden
    -std=c++17
)

target_compile_definitions(voicetts PRIVATE ANDROID)

target_link_libraries(voicetts
    espeak-ng
    log
    android
    dl
    m
)
