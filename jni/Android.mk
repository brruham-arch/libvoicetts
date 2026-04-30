LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := voicetts
LOCAL_SRC_FILES := voicetts.cpp

# eSpeak-NG static lib
LOCAL_C_INCLUDES := $(LOCAL_PATH)/espeak-ng/include
LOCAL_STATIC_LIBRARIES := espeak-ng

LOCAL_LDLIBS := \
    -llog \
    -landroid \
    -ldl \
    -lm

LOCAL_CPPFLAGS := \
    -std=c++17 \
    -O2 \
    -fvisibility=hidden \
    -DANDROID \
    -fPIC

LOCAL_CFLAGS := \
    -O2 \
    -fPIC \
    -DANDROID

include $(BUILD_SHARED_LIBRARY)

# ============================================================
# eSpeak-NG static library
# ============================================================
include $(CLEAR_VARS)
LOCAL_MODULE    := espeak-ng
LOCAL_SRC_FILES := espeak-ng/src/espeak-ng.c \
                   espeak-ng/src/speech.c \
                   espeak-ng/src/phoneme.c \
                   espeak-ng/src/synthesize.c \
                   espeak-ng/src/translate.c \
                   espeak-ng/src/readclause.c \
                   espeak-ng/src/setlengths.c \
                   espeak-ng/src/numbers.c \
                   espeak-ng/src/intonation.c \
                   espeak-ng/src/wave.c \
                   espeak-ng/src/voices.c \
                   espeak-ng/src/soundicon.c \
                   espeak-ng/src/mnemonics.c \
                   espeak-ng/src/encoding.c \
                   espeak-ng/src/error.c \
                   espeak-ng/src/compiledict.c \
                   espeak-ng/src/compilembrola.c \
                   espeak-ng/src/dictionary.c \
                   espeak-ng/src/fifo.c \
                   espeak-ng/src/ieee80.c \
                   espeak-ng/src/langopts.c \
                   espeak-ng/src/mbrola.c \
                   espeak-ng/src/syndata.c \
                   espeak-ng/src/vowelnet.c \
                   espeak-ng/src/wavegen.c \
                   espeak-ng/src/ucd/ucd.c

LOCAL_C_INCLUDES := $(LOCAL_PATH)/espeak-ng/include \
                    $(LOCAL_PATH)/espeak-ng/src

LOCAL_CFLAGS := \
    -O2 \
    -fPIC \
    -DANDROID \
    -DUSE_ASYNC=0 \
    -DLIBESPEAK_NG_EXPORT= \
    -DESPEAK_NG_DEFAULT_VOICE="\"id\""

include $(BUILD_STATIC_LIBRARY)
