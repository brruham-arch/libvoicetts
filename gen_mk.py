#!/usr/bin/env python3
import os

skip_dirs  = {'tests', 'windows', 'ucd-tools', 'speechPlayer', 'include', 'compat'}
skip_files = {'sPlayer.c', 'compilembrola.c', 'getopt.c'}  # tidak dibutuhkan/crash

# Source files - hanya dari libespeak-ng
srcs = []
for root, dirs, files in os.walk("jni/espeak-ng/src"):
    dirs[:] = [d for d in sorted(dirs) if d not in skip_dirs]
    for f in sorted(files):
        if f.endswith('.c'):
            srcs.append(os.path.join(root, f).replace("jni/espeak-ng/src/", "espeak-ng/src/"))

# Tambah ucd.c dari ucd-tools
ucd_c = "jni/espeak-ng/src/ucd-tools/src/ucd.c"
if os.path.exists(ucd_c):
    srcs.append("espeak-ng/src/ucd-tools/src/ucd.c")
    print("Added ucd.c from ucd-tools")
else:
    # Fallback: cari semua .c di ucd-tools/src
    for root, dirs, files in os.walk("jni/espeak-ng/src/ucd-tools/src"):
        dirs[:] = [d for d in sorted(dirs) if d != 'tests']
        for f in sorted(files):
            if f.endswith('.c') and f not in skip_files:
                srcs.append(os.path.join(root, f).replace("jni/espeak-ng/src/", "espeak-ng/src/"))

srcs_str = " \\\n    ".join(srcs)
print(f"Found {len(srcs)} source files")

inc = [
    "$(LOCAL_PATH)/espeak-ng/src",
    "$(LOCAL_PATH)/espeak-ng/src/libespeak-ng",
    "$(LOCAL_PATH)/espeak-ng/src/ucd-tools/src/include",  # <-- ucd/ucd.h ada di sini
    "$(LOCAL_PATH)/espeak-ng/src/include",
    "$(LOCAL_PATH)/espeak-ng/src/include/espeak-ng",
    "$(LOCAL_PATH)/espeak-ng/src/include/espeak",
    "$(LOCAL_PATH)/espeak-ng/src/include/compat",
    "$(LOCAL_PATH)/espeak-ng/include",
    "$(LOCAL_PATH)/espeak-ng/include/espeak-ng",
    "$(LOCAL_PATH)/espeak-ng/include/espeak",
]
inc_str = " \\\n    ".join(inc)

# Fix include di voicetts.cpp
for root, dirs, files in os.walk("jni/espeak-ng"):
    for f in files:
        if f == "speak_lib.h":
            hrel = root.replace("jni/", "")
            with open("jni/voicetts.cpp", "r") as fp:
                c = fp.read()
            c = c.replace('#include "espeak-ng/speak_lib.h"', f'#include "{hrel}/speak_lib.h"')
            c = c.replace('#include "espeak/speak_lib.h"', f'#include "{hrel}/speak_lib.h"')
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
