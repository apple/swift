
set(swift_stdlib_unittest_compile_flags
  "-Xfrontend" "-disable-objc-attr-requires-foundation-module")
if (SWIFT_RUNTIME_ENABLE_LEAK_CHECKER)
  list(APPEND swift_stdlib_unittest_compile_flags "-DSWIFT_RUNTIME_ENABLE_LEAK_CHECKER")
endif()

is_build_type_optimized("${SWIFT_STDLIB_BUILD_TYPE}" IS_BUILD_TYPE_OPTIMIZED)
if (NOT IS_BUILD_TYPE_OPTIMIZED)
  list(APPEND swift_stdlib_unittest_compile_flags "-DSWIFT_STDLIB_DEBUG")
endif()

set(swift_stdlib_unittest_link_libraries "")
set(swift_stdlib_unittest_modules "")
if (SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY)
  list(APPEND swift_stdlib_unittest_link_libraries "swift_Concurrency")
  list(APPEND swift_stdlib_unittest_modules "_Concurrency")
endif()
if (SWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED)
  list(APPEND swift_stdlib_unittest_link_libraries "swift_Distributed")
endif()

add_swift_target_library(swiftStdlibUnittest ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB
  # This file should be listed the first.  Module name is inferred from the
  # filename.
  StdlibUnittest.swift

  CheckStrideable.swift
  InspectValue.cpp
  InspectValue.swift
  InterceptTraps.cpp
  LifetimeTracked.swift
  MinimalTypes.swift
  OpaqueIdentityFunctions.cpp
  OpaqueIdentityFunctions.swift
  RaceTest.swift
  Statistics.swift
  StdlibCoreExtras.swift
  StringConvertible.swift
  SymbolLookup.swift
  TestHelpers.swift
  TypeIndexed.swift

  SWIFT_MODULE_DEPENDS SwiftPrivate SwiftPrivateThreadExtras SwiftPrivateLibcExtras ${swift_stdlib_unittest_modules}
  SWIFT_MODULE_DEPENDS_IOS Darwin
  SWIFT_MODULE_DEPENDS_OSX Darwin
  SWIFT_MODULE_DEPENDS_TVOS Darwin
  SWIFT_MODULE_DEPENDS_WATCHOS Darwin
  SWIFT_MODULE_DEPENDS_FREESTANDING Darwin
  SWIFT_MODULE_DEPENDS_LINUX Glibc
  SWIFT_MODULE_DEPENDS_FREEBSD Glibc
  SWIFT_MODULE_DEPENDS_OPENBSD Glibc
  SWIFT_MODULE_DEPENDS_CYGWIN Glibc
  SWIFT_MODULE_DEPENDS_HAIKU Glibc
  SWIFT_MODULE_DEPENDS_WINDOWS CRT WinSDK
  SWIFT_COMPILE_FLAGS ${swift_stdlib_unittest_compile_flags} ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
  INSTALL_IN_COMPONENT stdlib-experimental
  DARWIN_INSTALL_NAME_DIR "${SWIFT_DARWIN_STDLIB_PRIVATE_INSTALL_NAME_DIR}"
  LINK_LIBRARIES ${swift_stdlib_unittest_link_libraries})
set_source_files_properties(InspectValue.cpp PROPERTIES COMPILE_FLAGS -std=c++14)

if(SWIFT_FOUNDATION_OVERLAYS_BUILD_FROM_SOURCE)
    include(OverlayXcodeExternalProject)

    add_overlay_dependencies_to(swiftStdlibUnittest
      OVERLAYS Foundation
      TARGET_SDKS ${SWIFT_SDKS})
endif()
