#===--- CMakeLists.txt - Observation support library ---------------------===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2023 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
#===----------------------------------------------------------------------===#

list(APPEND swift_runtime_library_compile_flags -I${SWIFT_SOURCE_DIR}/stdlib/include -I${SWIFT_SOURCE_DIR}/include)

add_swift_target_library(swiftObservation ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB
  Locking.cpp
  Locking.swift
  Observable.swift
  ObservationRegistrar.swift
  ObservationTracking.swift
  ThreadLocal.cpp
  ThreadLocal.swift

  SWIFT_COMPILE_FLAGS
    ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
    "-enable-experimental-feature" "Macros"
    -Xfrontend -disable-implicit-string-processing-module-import

  C_COMPILE_FLAGS
    ${swift_runtime_library_compile_flags}

  SWIFT_MODULE_DEPENDS _Concurrency
  INSTALL_IN_COMPONENT stdlib

  MACCATALYST_BUILD_FLAVOR "zippered"
)
