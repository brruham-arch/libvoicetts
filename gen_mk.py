#!/usr/bin/env python3
import os

skip_dirs = {'tests', 'windows', 'include'}

# Source files
srcs = []
for root, dirs, files in os.walk("jni/espeak-ng/src"):
    dirs[:] = [d for d in sorted(dirs) if d not in skip_dirs]
    for f in sorted(files):
        if f.endswith('.c'):
            srcs.append(os.path.join(root, f).replace("jni/espeak-ng/src/", "espeak-ng/src/"))

srcs_str = " \\\n    ".join(srcs)
print(f"Found {len(srcs)} source files")

# Include paths - eksplisit semua yang dibutuhkan
# <ucd/ucd.h> butuh parent dari folder ucd = src/libespeak-ng atau src
# Kita tambah semua subfolder langsung
inc = [
    "$(LOCAL_PATH)/espeak-ng/src",
    "$(LOCAL_PATH)/espeak-ng/src/libespeak-ng",  # parent dari ucd include
    "$(LOCAL_PATH)/espeak-ng/src/ucd",
    "$(LOCAL_PATH)/espeak-ng/src/compat",
    "$(LOCAL_PATH)/espeak-ng/src/include",
    "$(LOCAL_PATH)/espeak-ng/src/include/espeak-ng",
    "$(LOCAL_PATH)/espeak-ng/src/include/espeak",
    "$(LOCAL_PATH)/espeak-ng/include",
    "$(LOCAL_PATH)/espeak-ng/include/espeak-ng",
    "$(LOCAL_PATH)/espeak-ng/include/espeak",
]
# Tambah semua subfolder lainnya secara dinamis
for root, dirs, files in os.walk("jni/espeak-ng/src"):
    dirs[:] = [d for d in sorted(dirs) if d not in skip_dirs]
    rel = root.replace("jni/espeak-ng/src", "$(LOCAL_PATH)/espeak-ng/src")
    if rel not in inc:
        inc.append(rel)

inc_str = " \\\n    ".join(inc)

# Fix include di voicetts.cpp
for root, dirs, files in os.walk("jni/espeak-ng"):
    for f in files:
        if f == "speak_lib.h":
            hrel = root.replace("jni/", "")
            with open("jni/voicetts.cpp", "r") as fp:
                c = fp.read()
            for old in ['espeak-ng/speak_lib.h', 'espeak/speak_lib.h']:
                c = c.replace(f'#include "{old}"', f'#include "{hrel}/speak_lib.h"')
            with open("jni/voicetts.cpp", "w") as fp:
                fp.write(c)
            print(f"Fixed include -> {hrel}/speak_lib.h")
            break

mk = f"""LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE    := espeak-ng
LOCAL_SRC_FILES := {srcs_str}
LOCAL_C_INCLUDES := {inc_str}
LOCAL_CFLAGS    := -O2 -fPIC -DANDROID -DUSE_ASYNC=0 -DLIBESPEAK_NG_EXPORT="" \\
    -Wno-error -Wno-implicit-function-declaration -Wno-incompatible-pointer-types
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE    := voicetts
LOCAL_SRC_FILES := voicetts.cpp
LOCAL_C_INCLUDES := {inc_str}
LOCAL_STATIC_LIBRARIES := espeak-ng
LOCAL_LDLIBS    := -llog -landroid -ldl -lm
LOCAL_CPPFLAGS  := -std=c++17 -O2 -fPIC -fvisibility=hidden -DANDROID
include $(BUILD_SHARED_LIBRARY)
"""

with open("jni/Android.mk", "w") as f:
    f.write(mk)
print("Android.mk generated OK")
