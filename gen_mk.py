#!/usr/bin/env python3
import os, glob

# Kumpulkan semua .c dari espeak-ng/src (semua subfolder)
srcs = []
skip_dirs = {'tests', 'windows', 'include'}
for root, dirs, files in os.walk("jni/espeak-ng/src"):
    dirs[:] = [d for d in sorted(dirs) if d not in skip_dirs]
    for f in sorted(files):
        if f.endswith('.c'):
            srcs.append(os.path.join(root, f))

srcs_rel = [s.replace("jni/espeak-ng/src/", "espeak-ng/src/") for s in srcs]
srcs_str = " \\\n    ".join(srcs_rel)
print(f"Found {len(srcs_rel)} source files")

# Kumpulkan semua subfolder dari src/ untuk include path
include_dirs = set()
include_dirs.add("$(LOCAL_PATH)/espeak-ng/src")
include_dirs.add("$(LOCAL_PATH)/espeak-ng/src/include")
include_dirs.add("$(LOCAL_PATH)/espeak-ng/src/include/espeak-ng")
include_dirs.add("$(LOCAL_PATH)/espeak-ng/include")
include_dirs.add("$(LOCAL_PATH)/espeak-ng/include/espeak-ng")
for root, dirs, files in os.walk("jni/espeak-ng/src"):
    dirs[:] = [d for d in sorted(dirs) if d not in skip_dirs]
    rel = root.replace("jni/espeak-ng/src", "$(LOCAL_PATH)/espeak-ng/src")
    include_dirs.add(rel)

includes_str = " \\\n    ".join(sorted(include_dirs))

# Cari header speak_lib.h dan fix include di voicetts.cpp
for root, dirs, files in os.walk("jni/espeak-ng"):
    for f in files:
        if f == "speak_lib.h":
            hrel = root.replace("jni/", "")
            with open("jni/voicetts.cpp", "r") as fp:
                content = fp.read()
            for old in ['espeak-ng/speak_lib.h', 'espeak/speak_lib.h']:
                content = content.replace(f'#include "{old}"', f'#include "{hrel}/speak_lib.h"')
            with open("jni/voicetts.cpp", "w") as fp:
                fp.write(content)
            print(f"Fixed include -> {hrel}/speak_lib.h")
            break

mk = f"""LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE    := espeak-ng
LOCAL_SRC_FILES := {srcs_str}
LOCAL_C_INCLUDES := {includes_str}
LOCAL_CFLAGS    := -O2 -fPIC -DANDROID -DUSE_ASYNC=0 -DLIBESPEAK_NG_EXPORT="" \\
    -Wno-error -Wno-implicit-function-declaration -Wno-incompatible-pointer-types
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE    := voicetts
LOCAL_SRC_FILES := voicetts.cpp
LOCAL_C_INCLUDES := {includes_str}
LOCAL_STATIC_LIBRARIES := espeak-ng
LOCAL_LDLIBS    := -llog -landroid -ldl -lm
LOCAL_CPPFLAGS  := -std=c++17 -O2 -fPIC -fvisibility=hidden -DANDROID
include $(BUILD_SHARED_LIBRARY)
"""

with open("jni/Android.mk", "w") as f:
    f.write(mk)

print("Android.mk generated OK")
