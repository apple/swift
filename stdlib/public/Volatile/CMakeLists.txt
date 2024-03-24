#===--- CMakeLists.txt ---------------------------------------------------===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2019 - 2022 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
#===----------------------------------------------------------------------===#

add_swift_target_library(swift_Volatile ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB
  Volatile.swift

  SWIFT_COMPILE_FLAGS
    ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
    -parse-stdlib
  LINK_FLAGS "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"

  INSTALL_IN_COMPONENT stdlib
)

if(SWIFT_SHOULD_BUILD_EMBEDDED_STDLIB)
  add_custom_target(embedded-volatile ALL)
  foreach(entry ${EMBEDDED_STDLIB_TARGET_TRIPLES})
    string(REGEX REPLACE "[ \t]+" ";" list "${entry}")
    list(GET list 0 arch)
    list(GET list 1 mod)
    list(GET list 2 triple)

    set(SWIFT_SDK_embedded_ARCH_${arch}_MODULE "${mod}")
    set(SWIFT_SDK_embedded_LIB_SUBDIR "embedded")
    set(SWIFT_SDK_embedded_ARCH_${arch}_TRIPLE "${triple}")
    add_swift_target_library_single(
      embedded-volatile-${mod}
      swift_Volatile
      ONLY_SWIFTMODULE
      IS_SDK_OVERLAY IS_FRAGILE

      Volatile.swift
      
      SWIFT_COMPILE_FLAGS
        -Xcc -D__MACH__ -Xcc -D__APPLE__ -Xcc -ffreestanding -enable-experimental-feature Embedded
        -parse-stdlib
      C_COMPILE_FLAGS
        -D__MACH__ -D__APPLE__ -ffreestanding
      MODULE_DIR "${CMAKE_BINARY_DIR}/lib/swift/embedded"
      SDK "embedded"
      ARCHITECTURE "${arch}"
      DEPENDS embedded-stdlib-${mod}
      INSTALL_IN_COMPONENT stdlib
      )
    add_dependencies(embedded-volatile embedded-volatile-${mod})
  endforeach()
endif()
