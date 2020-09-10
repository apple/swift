#===--- CMakeLists.txt - Concurrency support library ---------------------===#
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

add_swift_target_library(swift_Concurrency ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB
  PartialAsyncTask.swift

  SWIFT_MODULE_DEPENDS_OSX Darwin
  SWIFT_MODULE_DEPENDS_IOS Darwin
  SWIFT_MODULE_DEPENDS_TVOS Darwin
  SWIFT_MODULE_DEPENDS_WATCHOS Darwin
  SWIFT_MODULE_DEPENDS_LINUX Glibc
  SWIFT_MODULE_DEPENDS_FREEBSD Glibc
  SWIFT_MODULE_DEPENDS_OPENBSD Glibc
  SWIFT_MODULE_DEPENDS_CYGWIN Glibc
  SWIFT_MODULE_DEPENDS_HAIKU Glibc
  SWIFT_MODULE_DEPENDS_WINDOWS MSVCRT

  SWIFT_COMPILE_FLAGS
    ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
    -parse-stdlib
    -enable-library-evolution # To make @_implementationOnly imports work
  LINK_FLAGS "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"
  DARWIN_INSTALL_NAME_DIR "@rpath"
  INSTALL_IN_COMPONENT stdlib)
