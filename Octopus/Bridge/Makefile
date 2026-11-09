# OctopusBridge — Theos Makefile (rootless Dopamine, iOS 17.0, arm64e)
#
# Usage:
#   make THEOS=/var/jb/opt/theos
#   make package THEOS=/var/jb/opt/theos
#
# The tweak defaults to HOOKS_ENABLED=0 (observation-only). To enable PTP hooks after
# completing Stage 3 (runtime observation), pass -DHOOKS_ENABLED=1 to CFLAGS.

THEOS              ?= /var/jb/opt/theos
TARGET             := iphone:clang:latest:17.0
ARCHS              := arm64e
THEOS_PACKAGE_SCHEME := rootless
INSTALL_TARGET_PROCESSES := ptpd

include $(THEOS)/makefiles/common.mk

TWEAK_NAME := OctopusBridge

# The whole Octopus C core is linked so the tweak has the bridge transport and PTP helpers.
OctopusBridge_FILES := \
    OctopusBridge.m \
    $(wildcard ../Core/*.c) \
    $(wildcard ../Protocol/*.c)

OctopusBridge_CFLAGS   := -fno-objc-arc -I../Core -I../Protocol -std=gnu11
OctopusBridge_CCFLAGS  :=
OctopusBridge_OBJCFLAGS := -fno-objc-arc
OctopusBridge_FRAMEWORKS := Foundation CoreFoundation

# Suppress warnings for the included C core (compiled separately in CI under strict flags).
OctopusBridge_CFLAGS += -Wno-unused-variable -Wno-unused-function -Wno-shorten-64-to-32

include $(THEOS)/makefiles/tweak.mk
