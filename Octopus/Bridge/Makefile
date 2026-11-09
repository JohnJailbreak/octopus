# OctopusBridge — Theos Makefile for the ptpd integration tweak (rootless Dopamine, iOS 17, arm64e).
#
# DRAFT: never compiled, not built by CI, and not meant to be installed yet (see OctopusBridge.m).
#
#   make THEOS=/var/jb/opt/theos            # or wherever Theos lives
#   make package THEOS=/var/jb/opt/theos    # the .deb

THEOS ?= /var/jb/opt/theos

TARGET := iphone:clang:latest:17.0
ARCHS := arm64e
THEOS_PACKAGE_SCHEME := rootless
INSTALL_TARGET_PROCESSES := ptpd

include $(THEOS)/makefiles/common.mk

TWEAK_NAME := OctopusBridge

# oct_bridge.c holds both halves of the transport, and its server half and self-test use the rest of the core,
# so the whole core is linked; only the client half runs in ptpd.
OctopusBridge_FILES := OctopusBridge.m $(wildcard ../Core/*.c) $(wildcard ../Protocol/*.c)
OctopusBridge_CFLAGS := -fno-objc-arc -I../Core -I../Protocol
OctopusBridge_CONLYFLAGS := -std=gnu11
OctopusBridge_FRAMEWORKS := Foundation CoreFoundation

include $(THEOS)/makefiles/tweak.mk
