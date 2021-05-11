
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
if (SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY)
  list(APPEND swift_stdlib_unittest_link_libraries "swift_Concurrency")
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

  SWIFT_MODULE_DEPENDS SwiftPrivate SwiftPrivateLibcExtras
  SWIFT_MODULE_DEPENDS_IOS Darwin Foundation SwiftPrivateThreadExtras
  SWIFT_MODULE_DEPENDS_OSX Darwin Foundation SwiftPrivateThreadExtras
  SWIFT_MODULE_DEPENDS_TVOS Darwin Foundation SwiftPrivateThreadExtras
  SWIFT_MODULE_DEPENDS_WATCHOS Darwin Foundation SwiftPrivateThreadExtras
  SWIFT_MODULE_DEPENDS_FREESTANDING Darwin SwiftPrivateThreadExtras
  SWIFT_MODULE_DEPENDS_LINUX Glibc SwiftPrivateThreadExtras
  SWIFT_MODULE_DEPENDS_FREEBSD Glibc SwiftPrivateThreadExtras
  SWIFT_MODULE_DEPENDS_OPENBSD Glibc SwiftPrivateThreadExtras
  SWIFT_MODULE_DEPENDS_CYGWIN Glibc SwiftPrivateThreadExtras
  SWIFT_MODULE_DEPENDS_HAIKU Glibc SwiftPrivateThreadExtras
  SWIFT_MODULE_DEPENDS_WASI WASILibc
  SWIFT_MODULE_DEPENDS_WINDOWS CRT WinSDK SwiftPrivateThreadExtras
  SWIFT_COMPILE_FLAGS ${swift_stdlib_unittest_compile_flags} ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
  INSTALL_IN_COMPONENT stdlib-experimental
  DARWIN_INSTALL_NAME_DIR "${SWIFT_DARWIN_STDLIB_PRIVATE_INSTALL_NAME_DIR}"
  LINK_LIBRARIES ${swift_stdlib_unittest_link_libraries})
set_source_files_properties(InspectValue.cpp PROPERTIES COMPILE_FLAGS -std=c++14)

