if(SWIFT_BUILD_SDK_OVERLAY)
set(swift_stdlib_unittest_darwin_dependencies Darwin)
else()
set(swift_stdlib_unittest_darwin_dependencies)
endif()

set(swift_stdlib_unittest_compile_flags
  "-Xfrontend" "-disable-objc-attr-requires-foundation-module")
if (SWIFT_RUNTIME_ENABLE_LEAK_CHECKER)
  list(APPEND swift_stdlib_unittest_compile_flags "-DSWIFT_RUNTIME_ENABLE_LEAK_CHECKER")
endif()

is_build_type_optimized("${SWIFT_STDLIB_BUILD_TYPE}" IS_BUILD_TYPE_OPTIMIZED)
if (NOT IS_BUILD_TYPE_OPTIMIZED)
  list(APPEND swift_stdlib_unittest_compile_flags "-DSWIFT_STDLIB_DEBUG")
endif()

if(SWIFT_ENABLE_REFLECTION)
  list(APPEND swift_stdlib_unittest_compile_flags "-DSWIFT_ENABLE_REFLECTION")
endif()

if(SWIFT_STDLIB_STATIC_PRINT)
  list(APPEND swift_stdlib_unittest_compile_flags "-D" "SWIFT_STDLIB_STATIC_PRINT")
endif()

set(swift_stdlib_unittest_link_libraries "")
set(swift_stdlib_unittest_modules "")
if (SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY)
  list(APPEND swift_stdlib_unittest_link_libraries "swift_Concurrency")
  list(APPEND swift_stdlib_unittest_modules "_Concurrency")
endif()
if (SWIFT_ENABLE_EXPERIMENTAL_STRING_PROCESSING)
  list(APPEND swift_stdlib_unittest_modules "_StringProcessing")
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
  StringTestHelpers.swift
  TestHelpers.swift
  TypeIndexed.swift

  SWIFT_MODULE_DEPENDS SwiftPrivate SwiftPrivateThreadExtras SwiftPrivateLibcExtras ${swift_stdlib_unittest_modules}
  SWIFT_MODULE_DEPENDS_IOS ${swift_stdlib_unittest_darwin_dependencies}
  SWIFT_MODULE_DEPENDS_OSX ${swift_stdlib_unittest_darwin_dependencies}
  SWIFT_MODULE_DEPENDS_TVOS ${swift_stdlib_unittest_darwin_dependencies}
  SWIFT_MODULE_DEPENDS_WATCHOS ${swift_stdlib_unittest_darwin_dependencies}
  SWIFT_MODULE_DEPENDS_XROS ${swift_stdlib_unittest_darwin_dependencies}
  SWIFT_MODULE_DEPENDS_MACCATALYST ${swift_stdlib_unittest_darwin_dependencies}
  SWIFT_MODULE_DEPENDS_FREESTANDING "${SWIFT_FREESTANDING_TEST_DEPENDENCIES}"
  SWIFT_MODULE_DEPENDS_ANDROID Android
  SWIFT_MODULE_DEPENDS_LINUX Glibc
  SWIFT_MODULE_DEPENDS_FREEBSD Glibc
  SWIFT_MODULE_DEPENDS_OPENBSD Glibc
  SWIFT_MODULE_DEPENDS_CYGWIN Glibc
  SWIFT_MODULE_DEPENDS_HAIKU Glibc
  SWIFT_MODULE_DEPENDS_WASI WASILibc
  SWIFT_MODULE_DEPENDS_WINDOWS CRT WinSDK
  C_COMPILE_FLAGS -I${SWIFT_SOURCE_DIR}/stdlib/include
  SWIFT_COMPILE_FLAGS ${swift_stdlib_unittest_compile_flags} ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
  INSTALL_IN_COMPONENT stdlib-experimental
  DARWIN_INSTALL_NAME_DIR "${SWIFT_DARWIN_STDLIB_PRIVATE_INSTALL_NAME_DIR}"
  LINK_LIBRARIES ${swift_stdlib_unittest_link_libraries}
  MACCATALYST_BUILD_FLAVOR "zippered")
set_source_files_properties(InspectValue.cpp PROPERTIES COMPILE_FLAGS -std=c++17)

