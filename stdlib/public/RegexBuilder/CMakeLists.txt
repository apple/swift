#===--- CMakeLists.txt - String processing support library -----------------===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2021 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
#===------------------------------------------------------------------------===#

set(swift_regex_builder_link_libraries
  swiftCore
  swift_RegexParser
  swift_StringProcessing)

file(GLOB_RECURSE _REGEX_BUILDER_SOURCES
  "${SWIFT_PATH_TO_STRING_PROCESSING_SOURCE}/Sources/RegexBuilder/*.swift")
set(REGEX_BUILDER_SOURCES)
foreach(source ${_REGEX_BUILDER_SOURCES})
  file(TO_CMAKE_PATH "${source}" source)
  list(APPEND REGEX_BUILDER_SOURCES ${source})
endforeach()
message(STATUS "Using Experimental String Processing library for RegexBuilder (${SWIFT_PATH_TO_STRING_PROCESSING_SOURCE}).")

add_swift_target_library(swiftRegexBuilder ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB
  "${REGEX_BUILDER_SOURCES}"

  SWIFT_MODULE_DEPENDS_ANDROID Android
  SWIFT_MODULE_DEPENDS_LINUX Glibc
  SWIFT_MODULE_DEPENDS_FREEBSD Glibc
  SWIFT_MODULE_DEPENDS_OPENBSD Glibc
  SWIFT_MODULE_DEPENDS_CYGWIN Glibc
  SWIFT_MODULE_DEPENDS_HAIKU Glibc
  SWIFT_MODULE_DEPENDS_WINDOWS CRT

  LINK_LIBRARIES ${swift_regex_builder_link_libraries}

  C_COMPILE_FLAGS
    -DswiftRegexBuilder_EXPORTS
  SWIFT_COMPILE_FLAGS
    ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
  LINK_FLAGS "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"

  SWIFT_MODULE_DEPENDS _StringProcessing
  INSTALL_IN_COMPONENT stdlib
)
