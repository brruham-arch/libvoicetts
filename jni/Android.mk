LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := voicetts
LOCAL_SRC_FILES := voicetts.cpp

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

include $(BUILD_SHARED_LIBRARY)
