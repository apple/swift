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

set(swift_string_processing_link_libraries
  swiftCore
  swift_RegexParser)

set(swift_string_processing_compile_flags)

# Pass a flag to the build indicating that _StringProcessing's dependencies
# will be built with library evolution.
list(APPEND swift_string_processing_compile_flags
  "-DRESILIENT_LIBRARIES")

if(SWIFT_BUILD_STATIC_STDLIB)
  # Explicitly autolink swift_RegexParser because it's imported with @_implementationOnly
  list(APPEND swift_string_processing_compile_flags
    "-Xfrontend" "-public-autolink-library" "-Xfrontend" "swift_RegexParser")
endif()

file(GLOB_RECURSE _STRING_PROCESSING_SOURCES
  "${SWIFT_PATH_TO_STRING_PROCESSING_SOURCE}/Sources/_StringProcessing/*.swift"
  "${SWIFT_PATH_TO_STRING_PROCESSING_SOURCE}/Sources/_CUnicode/*.h"
  "${SWIFT_PATH_TO_STRING_PROCESSING_SOURCE}/Sources/_CUnicode/*.c")
set(STRING_PROCESSING_SOURCES)
foreach(source ${_STRING_PROCESSING_SOURCES})
  file(TO_CMAKE_PATH "${source}" source)
  list(APPEND STRING_PROCESSING_SOURCES ${source})
endforeach()
message(STATUS "Using Experimental String Processing library for _StringProcessing (${SWIFT_PATH_TO_STRING_PROCESSING_SOURCE}).")

add_swift_target_library(swift_StringProcessing ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB
  "${STRING_PROCESSING_SOURCES}"

  SWIFT_MODULE_DEPENDS_ANDROID Android
  SWIFT_MODULE_DEPENDS_LINUX Glibc
  SWIFT_MODULE_DEPENDS_FREEBSD Glibc
  SWIFT_MODULE_DEPENDS_OPENBSD Glibc
  SWIFT_MODULE_DEPENDS_CYGWIN Glibc
  SWIFT_MODULE_DEPENDS_HAIKU Glibc
  SWIFT_MODULE_DEPENDS_WINDOWS CRT

  LINK_LIBRARIES ${swift_string_processing_link_libraries}

  C_COMPILE_FLAGS
    -Dswift_StringProcessing_EXPORTS
  SWIFT_COMPILE_FLAGS
    ${swift_string_processing_compile_flags}
    ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
  LINK_FLAGS "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"

  SWIFT_MODULE_DEPENDS _RegexParser
  INSTALL_IN_COMPONENT stdlib
  MACCATALYST_BUILD_FLAVOR "zippered"
)
