#===--- CMakeLists.txt -----------------------------------------------------===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2019 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
#===------------------------------------------------------------------------===#

add_swift_target_library(swiftLowLevel ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB
  Demangle.swift

  SWIFT_COMPILE_FLAGS
    ${SWIFT_RUNTIME_SWIFT_COMPILE_FLAGS}
  LINK_FLAGS
    "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"
  INSTALL_IN_COMPONENT
    stdlib
)
