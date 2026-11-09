# OctopusBridge — Theos Makefile (rootless Dopamine, iOS 17.0, arm64e)
#
#   make THEOS=/var/jb/opt/theos
#   make package THEOS=/var/jb/opt/theos
#
# This build observes ptpd and changes nothing in it; see the header of OctopusBridge.x.
#
# The source is .x, not .m, on purpose: Theos runs the Logos preprocessor only on .x (Objective-C)
# and .xm (Objective-C++) files. A .m file goes straight to clang, which rejects %hook.

THEOS                    ?= /var/jb/opt/theos
TARGET                   := iphone:clang:latest:17.0
ARCHS                    := arm64e
THEOS_PACKAGE_SCHEME     := rootless
INSTALL_TARGET_PROCESSES := ptpd

include $(THEOS)/makefiles/common.mk

TWEAK_NAME := OctopusBridge

# The bridge's client half, the logger, and status names. These depend on libc alone: none of the
# app's storage engine, responder or diagnostics is linked into ptpd.
OctopusBridge_FILES := \
    OctopusBridge.x \
    ../Core/oct_bridge_io.c \
    ../Core/oct_bridge_client.c \
    ../Core/oct_log.c \
    ../Core/oct_status.c

# -fno-implicit-module-maps: Theos turns Clang modules on, and Clang then finds the app's own module
# map one directory above Core/ (Octopus/module.modulemap) and builds module OctopusCore from it.
# Xcode compiles these files as that module's implementation (its library target is named
# OctopusCore), so it includes their headers textually; Theos names the module after the tweak, so
# Clang imports OctopusCore and also includes the headers textually, defining every type twice.
#
# -fno-modules didn't fix it: it is last-one-wins, and Theos adds its own -fmodules after these
# flags. Whether Clang looks for module maps is decided only by -fimplicit-module-maps versus
# -fno-implicit-module-maps, whatever the order of -fmodules, so this holds wherever Theos puts its
# flags. With no module map loaded, the headers are plain includes with guards, as GCC sees them.
OctopusBridge_CFLAGS     := -I../Core -I../Protocol -fno-objc-arc -fno-implicit-module-maps \
                            -Wno-unused-command-line-argument
OctopusBridge_FRAMEWORKS := Foundation

include $(THEOS)/makefiles/tweak.mk
