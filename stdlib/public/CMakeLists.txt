add_subdirectory(LLVMSupport)

set(SWIFT_RUNTIME_CXX_FLAGS)
set(SWIFT_RUNTIME_LINK_FLAGS)
set(SWIFT_RUNTIME_SWIFT_COMPILE_FLAGS)
set(SWIFT_RUNTIME_SWIFT_LINK_FLAGS)
set(SWIFT_RUNTIME_CONCURRENCY_SWIFT_LINK_FLAGS)

if(SWIFT_RUNTIME_USE_SANITIZERS)
  # TODO: Refactor this
  if("Thread" IN_LIST SWIFT_RUNTIME_USE_SANITIZERS)
    list(APPEND SWIFT_RUNTIME_CXX_FLAGS "-fsanitize=thread")
    list(APPEND SWIFT_RUNTIME_LINK_FLAGS "-fsanitize=thread")
    list(APPEND SWIFT_RUNTIME_SWIFT_COMPILE_FLAGS "-sanitize=thread")
    list(APPEND SWIFT_RUNTIME_SWIFT_LINK_FLAGS "-fsanitize=thread")
  endif()
endif()

list(APPEND  SWIFT_RUNTIME_SWIFT_COMPILE_FLAGS "-Xfrontend" "-verify-syntax-tree")

if(SWIFT_STDLIB_SIL_DEBUGGING)
  list(APPEND SWIFT_RUNTIME_SWIFT_COMPILE_FLAGS "-Xfrontend" "-sil-based-debuginfo")
endif()

# Build the runtime with -Wall to catch, e.g., uninitialized variables
# warnings.
if(SWIFT_COMPILER_IS_MSVC_LIKE)
  list(APPEND SWIFT_RUNTIME_CXX_FLAGS "/W3")
else()
  list(APPEND SWIFT_RUNTIME_CXX_FLAGS "-Wall")
endif()

set(SWIFT_RUNTIME_CORE_CXX_FLAGS "${SWIFT_RUNTIME_CXX_FLAGS}")
set(SWIFT_RUNTIME_CORE_LINK_FLAGS "${SWIFT_RUNTIME_LINK_FLAGS}")

if(CMAKE_SYSTEM_NAME STREQUAL "CYGWIN")
  list(APPEND SWIFT_RUNTIME_CORE_CXX_FLAGS "-mcmodel=large")
elseif(CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
  list(APPEND SWIFT_RUNTIME_CORE_CXX_FLAGS "-xc++")
endif()

# C++ code in the runtime and standard library should generally avoid
# introducing static constructors or destructors.
check_cxx_compiler_flag("-Werror -Wglobal-constructors" CXX_SUPPORTS_GLOBAL_CONSTRUCTORS_WARNING)
if(CXX_SUPPORTS_GLOBAL_CONSTRUCTORS_WARNING)
  list(APPEND SWIFT_RUNTIME_CORE_CXX_FLAGS "-Wglobal-constructors")
endif()

# C++ code in the runtime and standard library should generally avoid
# introducing static constructors or destructors.
check_cxx_compiler_flag("-Wexit-time-destructors" CXX_SUPPORTS_EXIT_TIME_DESTRUCTORS_WARNING)
if(CXX_SUPPORTS_EXIT_TIME_DESTRUCTORS_WARNING)
  list(APPEND SWIFT_RUNTIME_CORE_CXX_FLAGS "-Wexit-time-destructors")
endif()

add_subdirectory(SwiftShims)

# This static library is shared across swiftCore and swiftReflection
if(SWIFT_BUILD_STDLIB OR SWIFT_BUILD_REMOTE_MIRROR)
  # TODO: due to the use of `add_swift_target_library` rather than `add_library`
  # we cannot use `target_sources` and thus must resort to list manipulations to
  # adjust the source list.
  set(swiftDemanglingSources
    "${SWIFT_SOURCE_DIR}/lib/Demangling/Context.cpp"
    "${SWIFT_SOURCE_DIR}/lib/Demangling/Demangler.cpp"
    "${SWIFT_SOURCE_DIR}/lib/Demangling/ManglingUtils.cpp"
    "${SWIFT_SOURCE_DIR}/lib/Demangling/NodePrinter.cpp"
    "${SWIFT_SOURCE_DIR}/lib/Demangling/OldDemangler.cpp"
    "${SWIFT_SOURCE_DIR}/lib/Demangling/OldRemangler.cpp"
    "${SWIFT_SOURCE_DIR}/lib/Demangling/Punycode.cpp"
    "${SWIFT_SOURCE_DIR}/lib/Demangling/Remangler.cpp")

  # When we're building with assertions, include the demangle node dumper to aid
  # in debugging.
  if(LLVM_ENABLE_ASSERTIONS)
    list(APPEND swiftDemanglingSources
      "${SWIFT_SOURCE_DIR}/lib/Demangling/NodeDumper.cpp")
  endif()

  add_swift_target_library(swiftDemangling OBJECT_LIBRARY
    ${swiftDemanglingSources}
    C_COMPILE_FLAGS -DswiftCore_EXPORTS
    INSTALL_IN_COMPONENT never_install)
endif()

if(SWIFT_BUILD_STDLIB)
  # These must be kept in dependency order so that any referenced targets
  # exist at the time we look for them in add_swift_*.
  add_subdirectory(runtime)
  add_subdirectory(stubs)
  add_subdirectory(core)
  add_subdirectory(SwiftOnoneSupport)

  if(SWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING)
    add_subdirectory(Differentiation)
  endif()

  if(SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY)
    add_subdirectory(Concurrency)
  endif()

  if(SWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED)
    add_subdirectory(Distributed)
  endif()
endif()

if(SWIFT_BUILD_STDLIB OR SWIFT_BUILD_REMOTE_MIRROR)
  add_subdirectory(Reflection)
  add_subdirectory(SwiftRemoteMirror)
endif()

if(SWIFT_BUILD_SDK_OVERLAY OR SWIFT_BUILD_TEST_SUPPORT_MODULES)
  add_subdirectory(Platform)
endif()

if(SWIFT_BUILD_SDK_OVERLAY)
  # On Apple platforms, we aren't building any overlays (other than Darwin in
  # Platform above). Instead, we're picking them up from the SDK.
  if(SWIFT_FOUNDATION_OVERLAYS_BUILD_FROM_SOURCE)
    include(OverlayXcodeExternalProject)
   
    list_intersect("${SWIFT_SDKS}" "${SWIFT_FOUNDATION_OVERLAYS_SUPPORTED_SDKS}" SDKS_TO_BUILD_FOR)
    message(STATUS "Building CoreFoundation and Foundation overlays for SDKs: " ${SDKS_TO_BUILD_FOR})
    build_corefoundation_and_foundation_overlays(
      TARGET_SDKS ${SDKS_TO_BUILD_FOR}
      FOUNDATION_OVERLAYS_SOURCE_DIR ${SWIFT_FOUNDATION_OVERLAYS_SOURCE_REPO}
      ADDITIONAL_XCODEBUILD_ARGUMENTS ${SWIFT_FOUNDATION_OVERLAYS_ADDITIONAL_XCODEBUILD_ARGS})
    add_stdlib_dependencies_to(
      OVERLAYS CoreFoundation
      TARGET_SDKS ${SDKS_TO_BUILD_FOR})
  endif()

  if(WINDOWS IN_LIST SWIFT_SDKS)
    add_subdirectory(Windows)
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  list(APPEND SWIFT_RUNTIME_CONCURRENCY_SWIFT_LINK_FLAGS
    "-Wl,-reexported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/Concurrency/ReexportedSymbols")
endif()

list(APPEND SWIFT_RUNTIME_CONCURRENCY_SWIFT_LINK_FLAGS "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}")
