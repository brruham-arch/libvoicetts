#!/usr/bin/env python3
import os, glob, subprocess

# Kumpulkan semua .c dari espeak-ng/src
srcs = sorted(glob.glob("jni/espeak-ng/src/*.c"))
srcs += sorted(glob.glob("jni/espeak-ng/src/ucd/*.c"))
srcs_rel = [s.replace("jni/espeak-ng/src/", "espeak-ng/src/") for s in srcs]
srcs_str = " \\\n    ".join(srcs_rel)

# Cari header speak_lib.h
header_dirs = set()
for root, dirs, files in os.walk("jni/espeak-ng"):
    for f in files:
        if f == "speak_lib.h":
            header_dirs.add(root)

# Fix include di voicetts.cpp
if header_dirs:
    hdir = list(header_dirs)[0]
    hrel = hdir.replace("jni/", "")
    with open("jni/voicetts.cpp", "r") as f:
        content = f.read()
    content = content.replace('#include "espeak-ng/speak_lib.h"',
                               f'#include "{hrel}/speak_lib.h"')
    with open("jni/voicetts.cpp", "w") as f:
        f.write(content)
    print(f"Fixed include -> {hrel}/speak_lib.h")

mk = r"""LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE    := espeak-ng
LOCAL_SRC_FILES := """ + srcs_str + r"""
LOCAL_C_INCLUDES := $(LOCAL_PATH)/espeak-ng/src \
    $(LOCAL_PATH)/espeak-ng/src/include \
    $(LOCAL_PATH)/espeak-ng/src/include/espeak-ng \
    $(LOCAL_PATH)/espeak-ng/include \
    $(LOCAL_PATH)/espeak-ng/include/espeak-ng
LOCAL_CFLAGS    := -O2 -fPIC -DANDROID -DUSE_ASYNC=0 -DLIBESPEAK_NG_EXPORT="" -Wno-error
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE    := voicetts
LOCAL_SRC_FILES := voicetts.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/espeak-ng/src/include \
    $(LOCAL_PATH)/espeak-ng/src/include/espeak-ng \
    $(LOCAL_PATH)/espeak-ng/include \
    $(LOCAL_PATH)/espeak-ng/include/espeak-ng
LOCAL_STATIC_LIBRARIES := espeak-ng
LOCAL_LDLIBS    := -llog -landroid -ldl -lm
LOCAL_CPPFLAGS  := -std=c++17 -O2 -fPIC -fvisibility=hidden -DANDROID
include $(BUILD_SHARED_LIBRARY)
"""

with open("jni/Android.mk", "w") as f:
    f.write(mk)

print("Android.mk generated with", len(srcs_rel), "source files")
