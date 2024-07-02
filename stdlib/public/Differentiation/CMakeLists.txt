#===--- CMakeLists.txt - Differentiable programming support library ------===#
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

if(SWIFT_STDLIB_ENABLE_VECTOR_TYPES)
set(swiftDifferentiationSIMDFiles SIMDDifferentiation.swift.gyb)
else()
set(swiftDifferentiationSIMDFiles)
endif()

if(SWIFT_BUILD_SDK_OVERLAY)
set(swiftDifferentiationDarwinDependencies Darwin)
else()
set(swiftDifferentiationDarwinDependencies)
endif()

add_swift_target_library(swift_Differentiation ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB
  Differentiable.swift
  DifferentialOperators.swift
  DifferentiationUtilities.swift
  AnyDifferentiable.swift
  ArrayDifferentiation.swift
  OptionalDifferentiation.swift

  "${SWIFT_SOURCE_DIR}/stdlib/linker-support/magic-symbols-for-install-name.c"

  GYB_SOURCES
    FloatingPointDifferentiation.swift.gyb
    TgmathDerivatives.swift.gyb
    ${swiftDifferentiationSIMDFiles}

  SWIFT_MODULE_DEPENDS_OSX ${swiftDifferentiationDarwinDependencies}
  SWIFT_MODULE_DEPENDS_IOS ${swiftDifferentiationDarwinDependencies}
  SWIFT_MODULE_DEPENDS_TVOS ${swiftDifferentiationDarwinDependencies}
  SWIFT_MODULE_DEPENDS_WATCHOS ${swiftDifferentiationDarwinDependencies}
  SWIFT_MODULE_DEPENDS_XROS ${swiftDifferentiationDarwinDependencies}
  SWIFT_MODULE_DEPENDS_ANDROID Android
  SWIFT_MODULE_DEPENDS_LINUX Glibc
  SWIFT_MODULE_DEPENDS_LINUX_STATIC Musl
  SWIFT_MODULE_DEPENDS_FREEBSD Glibc
  SWIFT_MODULE_DEPENDS_OPENBSD Glibc
  SWIFT_MODULE_DEPENDS_CYGWIN Glibc
  SWIFT_MODULE_DEPENDS_HAIKU Glibc
  SWIFT_MODULE_DEPENDS_WASI WASILibc
  SWIFT_MODULE_DEPENDS_WINDOWS CRT

  C_COMPILE_FLAGS
    -Dswift_Differentiation_EXPORTS
  SWIFT_COMPILE_FLAGS
    ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
    -parse-stdlib
  LINK_FLAGS "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"
  INSTALL_IN_COMPONENT stdlib
  SWIFT_MODULE_DEPENDS_MACCATALYST ${swiftDifferentiationDarwinDependencies}
  MACCATALYST_BUILD_FLAVOR "zippered")
