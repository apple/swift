if(SWIFT_BUILD_STDLIB OR SWIFT_BUILD_SDK_OVERLAY)
  add_subdirectory(SwiftPrivate)
endif()

if(SWIFT_BUILD_SDK_OVERLAY)
  # SwiftPrivateThreadExtras makes use of Darwin/Glibc, which is part of the
  # SDK overlay. It can't be built separately from the SDK overlay.
  if(SWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING)
    add_subdirectory(DifferentiationUnittest)
  endif()
endif()

if(SWIFT_BUILD_SDK_OVERLAY OR SWIFT_BUILD_TEST_SUPPORT_MODULES)
  add_subdirectory(RuntimeUnittest)
  add_subdirectory(StdlibUnicodeUnittest)
  add_subdirectory(StdlibCollectionUnittest)
  add_subdirectory(SwiftPrivateLibcExtras)
  add_subdirectory(SwiftPrivateThreadExtras)

  # NOTE(compnerd) this must come after SwiftPrivateLibcExtras and
  # SwiftPrivateThreadExtras to ensure that the dependency targets are setup in
  # the correct order for Windows.
  add_subdirectory(StdlibUnittest)
endif()

if(SWIFT_BUILD_SDK_OVERLAY)
  add_subdirectory(OSLog)

  list_intersect("${SWIFT_APPLE_PLATFORMS}" "${SWIFT_SDKS}" building_darwin_sdks)
  if(building_darwin_sdks)
    add_subdirectory(StdlibUnittestFoundationExtras)
  endif()
  # Currently SwiftReflectionTest cannot be built on Windows, due to 
  # dependencies on POSIX symbols 
  if (SWIFT_INCLUDE_TESTS AND (NOT CMAKE_SYSTEM_NAME STREQUAL "Windows") AND NOT SWIFTWASM_DISABLE_REFLECTION_TEST)
    add_subdirectory(SwiftReflectionTest)
  endif()
endif()

# Keep in sync with stdlib/tools/CMakeLists.txt: swift-reflection-test is
# only used when testing dynamic stdlib.
if(SWIFT_BUILD_DYNAMIC_STDLIB AND SWIFT_INCLUDE_TESTS)
  # NOTE create a stub BlocksRuntime library that can be used for the
  # reflection tests
  add_subdirectory(BlocksRuntimeStubs)
endif()

