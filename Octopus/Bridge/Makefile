# OctopusBridge — Theos Makefile (rootless Dopamine, iOS 17.0, arm64e)
#
#   make THEOS=/var/jb/opt/theos
#   make package THEOS=/var/jb/opt/theos
#
# This build observes ptpd and changes nothing in it; see the header of OctopusBridge.m.

THEOS                    ?= /var/jb/opt/theos
TARGET                   := iphone:clang:latest:17.0
ARCHS                    := arm64e
THEOS_PACKAGE_SCHEME     := rootless
INSTALL_TARGET_PROCESSES := ptpd

include $(THEOS)/makefiles/common.mk

TWEAK_NAME := OctopusBridge

# Only the bridge's client half and the logger. These three depend on libc alone: none of the
# app's storage engine, responder or diagnostics is linked into ptpd.
OctopusBridge_FILES := \
    OctopusBridge.m \
    ../Core/oct_bridge_io.c \
    ../Core/oct_bridge_client.c \
    ../Core/oct_log.c

OctopusBridge_CFLAGS     := -I../Core -I../Protocol -fno-objc-arc
OctopusBridge_CONLYFLAGS := -std=gnu11
OctopusBridge_FRAMEWORKS := Foundation

include $(THEOS)/makefiles/tweak.mk
