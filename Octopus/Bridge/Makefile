# OctopusBridge — Theos Makefile
# Builds the ptpd integration tweak for rootless Dopamine on iOS 17.0 (iPhone 13, ARM64e).
#
# Prerequisites on the build machine:
#   • Theos installed (default: /var/jb/opt/theos; override with THEOS=)
#   • iOS 17.x SDK reachable via $(THEOS)/sdks/
#   • clang with -arch arm64e and -target arm64e-apple-ios17.0
#
# Usage:
#   make THEOS=/var/jb/opt/theos
#   make package THEOS=/var/jb/opt/theos   # builds the .deb

THEOS ?= /var/jb/opt/theos

TARGET               := iphone:clang:17.0:14.0
ARCHS                := arm64e
INSTALL_TARGET_PROCESSES := ptpd

include $(THEOS)/makefiles/common.mk

TWEAK_NAME := OctopusBridge

OctopusBridge_FILES := \
    OctopusBridge.m \
    ../Core/oct_bridge.c \
    ../Core/oct_log.c \
    ../Core/oct_sha256.c \
    ../Protocol/oct_ptp.c

OctopusBridge_CFLAGS := \
    -std=c11 \
    -I../Core \
    -I../Protocol \
    -Wno-unused-parameter \
    -fobjc-arc

OctopusBridge_CCFLAGS := \
    -std=c++11 \
    -I../Core

OctopusBridge_OBJCFLAGS := \
    -fobjc-arc

OctopusBridge_LIBRARIES := substrate

OctopusBridge_FILTER := \
    bundles: com.apple.ptpd

include $(THEOS)/makefiles/tweak.mk
