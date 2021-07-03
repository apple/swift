#===--- CMakeLists.txt - Platform differentiation library ----------------===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2019 - 2020 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
#===----------------------------------------------------------------------===#

add_swift_target_library(swift_Darwin_Differentiation ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_SDK_OVERLAY
  "${SWIFT_SOURCE_DIR}/stdlib/linker-support/magic-symbols-for-install-name.c"

  GYB_SOURCES
    TgmathDerivatives.swift.gyb

  SWIFT_MODULE_DEPENDS_OSX Darwin _Differentiation
  SWIFT_MODULE_DEPENDS_IOS Darwin _Differentiation 
  SWIFT_MODULE_DEPENDS_TVOS Darwin _Differentiation
  SWIFT_MODULE_DEPENDS_WATCHOS Darwin _Differentiation

  SWIFT_COMPILE_FLAGS
    ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
  LINK_FLAGS "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"
  TARGET_SDKS ALL_APPLE_PLATFORMS FREESTANDING
  INSTALL_IN_COMPONENT sdk-overlay)


add_swift_target_library(swift_Glibc_Differentiation ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_SDK_OVERLAY
  GYB_SOURCES
    TgmathDerivatives.swift.gyb

  SWIFT_MODULE_DEPENDS_LINUX Glibc _Differentiation
  SWIFT_MODULE_DEPENDS_FREEBSD Glibc _Differentiation
  SWIFT_MODULE_DEPENDS_OPENBSD Glibc _Differentiation
  SWIFT_MODULE_DEPENDS_CYGWIN Glibc _Differentiation
  SWIFT_MODULE_DEPENDS_HAIKU Glibc _Differentiation

  SWIFT_COMPILE_FLAGS
    ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
  LINK_FLAGS "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"
  TARGET_SDKS ANDROID CYGWIN FREEBSD OPENBSD LINUX HAIKU
  INSTALL_IN_COMPONENT sdk-overlay)

add_swift_target_library(swift_CRT_Differentiation ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_SDK_OVERLAY
  GYB_SOURCES
    TgmathDerivatives.swift.gyb

  SWIFT_MODULE_DEPENDS_WINDOWS CRT _Differentiation

  SWIFT_COMPILE_FLAGS
    ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
  LINK_FLAGS "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"
  TARGET_SDKS WINDOWS
  INSTALL_IN_COMPONENT sdk-overlay)
