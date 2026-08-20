LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := xvcen.sh

_CF := -O3 -ffast-math -march=armv8-a+simd -mtune=cortex-a76 \
       -fomit-frame-pointer -ffunction-sections -fdata-sections \
       -fvisibility=hidden -fvisibility-inlines-hidden \
       -DUSE_OPENGL -DNDEBUG -w \
       -fstack-protector-strong \
       -fPIE \
       -fno-unwind-tables \
       -fno-asynchronous-unwind-tables \
       -fno-ident \
       -fno-jump-tables \
       -fstrict-aliasing \
       -fno-delete-null-pointer-checks \
       -fno-strict-overflow \
       -fno-builtin-memcpy \
       -fno-builtin-memset \
       -fno-builtin-strlen \
       -fno-builtin-strcmp \

LOCAL_CPPFLAGS := $(_CF) -std=c++17 -fexceptions -fno-rtti -fpermissive -Wno-error=format-security -Wno-error=c++11-narrowing
LOCAL_CFLAGS   := $(_CF) -std=c99

LOCAL_LDFLAGS := -Wl,--gc-sections,-s,--strip-all -Wl,-x -Wl,--build-id=none -pie -fPIE

LOCAL_C_INCLUDES += $(LOCAL_PATH)/include $(LOCAL_PATH)/include/ImGui $(LOCAL_PATH)/include/ImGui/backends $(LOCAL_PATH)/src

LOCAL_SRC_FILES := \
    src/main.cpp src/game.cpp src/Android_draw/draw.cpp src/Android_touch/TouchHelperA.cpp \
    src/Blur/Blur.cpp \
    src/ImGui/imgui.cpp src/ImGui/imgui_draw.cpp src/ImGui/imgui_tables.cpp \
    src/ImGui/imgui_widgets.cpp src/ImGui/backends/imgui_impl_android.cpp \
    src/ImGui/backends/imgui_impl_opengl3.cpp

LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv3 -lOpenSLES -lz -ldl

include $(BUILD_EXECUTABLE)