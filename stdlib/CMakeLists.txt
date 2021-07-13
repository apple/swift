
# TODO(compnerd) once we have a newer CMake we should be able to use the new
# `Swift` LANGUAGE support in CMake to simplify the build.  For now, just add
# the project so that it is possible to start working on extracting the Swift
# standard library from the Swift compiler build.
project(swift-stdlib LANGUAGES C CXX)

# Add path for custom CMake modules.
list(APPEND CMAKE_MODULE_PATH
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

if("${SWIFT_HOST_VARIANT_SDK}" MATCHES "(OSX|IOS*|TVOS*|WATCHOS*)")
  # All Darwin platforms have ABI stability.
  set(SWIFT_STDLIB_STABLE_ABI_default TRUE)
elseif("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "LINUX")
  # TODO(mracek): This should get turned off, as this is not an ABI stable platform.
  set(SWIFT_STDLIB_STABLE_ABI_default TRUE)
elseif("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "FREEBSD")
  # TODO(mracek): This should get turned off, as this is not an ABI stable platform.
  set(SWIFT_STDLIB_STABLE_ABI_default TRUE)
elseif("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "OPENBSD")
  # TODO(mracek): This should get turned off, as this is not an ABI stable platform.
  set(SWIFT_STDLIB_STABLE_ABI_default TRUE)
elseif("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "CYGWIN")
  # TODO(mracek): This should get turned off, as this is not an ABI stable platform.
  set(SWIFT_STDLIB_STABLE_ABI_default TRUE)
elseif("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "WINDOWS")
  # TODO(mracek): This should get turned off, as this is not an ABI stable platform.
  set(SWIFT_STDLIB_STABLE_ABI_default TRUE)
elseif("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "HAIKU")
  # TODO(mracek): This should get turned off, as this is not an ABI stable platform.
  set(SWIFT_STDLIB_STABLE_ABI_default TRUE)
elseif("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "ANDROID")
  # TODO(mracek): This should get turned off, as this is not an ABI stable platform.
  set(SWIFT_STDLIB_STABLE_ABI_default TRUE)
else()
  # Any new platform should have non-stable ABI to start with.
  set(SWIFT_STDLIB_STABLE_ABI_default FALSE)
endif()

if(SWIFT_BUILD_SDK_OVERLAY OR SWIFT_INCLUDE_TESTS)
  set(SWIFT_BUILD_TEST_SUPPORT_MODULES_default TRUE)
else()
  set(SWIFT_BUILD_TEST_SUPPORT_MODULES_default FALSE)
endif()

#
# User-configurable options for the standard library.
#

# NOTE: Some of these variables are also initialized in StandaloneOverlay.cmake
# so that interfaces are emitted when overlays are separately built.

option(SWIFT_STDLIB_STABLE_ABI
       "Should stdlib be built with stable ABI (library evolution, resilience)."
       "${SWIFT_STDLIB_STABLE_ABI_default}")

option(SWIFT_ENABLE_MODULE_INTERFACES
       "Generate .swiftinterface files alongside .swiftmodule files"
       "${SWIFT_STDLIB_STABLE_ABI}")

option(SWIFT_ENABLE_COMPATIBILITY_OVERRIDES
       "Support back-deploying compatibility fixes for newer apps running on older runtimes."
       TRUE)

option(SWIFT_RUNTIME_MACHO_NO_DYLD
       "Build stdlib assuming the runtime environment uses Mach-O but does not support dynamic modules."
       FALSE)

option(SWIFT_STDLIB_SINGLE_THREADED_RUNTIME
       "Build the standard libraries assuming that they will be used in an environment with only a single thread."
       FALSE)

option(SWIFT_STDLIB_OS_VERSIONING
       "Build stdlib with availability based on OS versions (Darwin only)."
       TRUE)

option(SWIFT_BUILD_TEST_SUPPORT_MODULES
       "Whether to build StdlibUnittest and other test support modules. Defaults to On when SWIFT_BUILD_SDK_OVERLAY is On, or when SWIFT_INCLUDE_TESTS is On."
       "${SWIFT_BUILD_TEST_SUPPORT_MODULES_default}")

set(SWIFT_STDLIB_ENABLE_LTO OFF CACHE STRING "Build Swift stdlib with LTO. One
    must specify the form of LTO by setting this to one of: 'full', 'thin'. This
    option only affects the standard library and runtime, not tools.")

option(SWIFT_FOUNDATION_OVERLAYS_BUILD_FROM_SOURCE
  "Whether to build Foundation overlays from swift-corelibs-foundation"
  OFF)

set(SWIFT_FOUNDATION_OVERLAYS_SUPPORTED_SDKS "IOS_SIMULATOR;OSX;TVOS_SIMULATOR;WATCHOS_SIMULATOR" CACHE STRING
  "List of SDKs that allow Foundation overlays to be built from sources")

set(SWIFT_FOUNDATION_OVERLAYS_SOURCE_PATH "${CMAKE_SOURCE_DIR}/../swift-corelibs-foundation/Darwin" CACHE STRING "Path to Foundation overlays sources")

set(SWIFT_FOUNDATION_OVERLAYS_ADDITIONAL_XCODEBUILD_ARGS "" CACHE STRING "Additional build arguments to pass to Xcode when building the Foundation overlays")
#
# End of user-configurable options.
#

include(AddSwiftStdlib)

# Create convenience targets for the Swift standard library.

# NOTE(compnerd) this will pass the *build* configuration to the *host*
# libraries.  Explicitly indicate to CMake that it should **NOT** track the
# implicit language runtimes.  This can go away once we migrate to an external
# project with its own configure with the CMAKE_SYSTEM_NAME set rather than
# using the custom cross-compilation solution
set(CMAKE_C_IMPLICIT_LINK_LIBRARIES "")
set(CMAKE_CXX_IMPLICIT_LINK_LIBRARIES "")

set(CMAKE_C_IMPLICIT_LINK_DIRECTORIES "")
set(CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES "")

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)

if(SWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER)
  if(NOT "${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    message(FATAL_ERROR "Building the swift runtime is not supported with ${CMAKE_C_COMPILER_ID}. Use the just-built clang instead.")
  else()
    message(WARNING "Building the swift runtime using the host compiler, and not the just-built clang.")
  endif()
else()
  # If we use Clang-cl or MSVC, CMake provides default compiler and linker flags that are incompatible
  # with the frontend of Clang or Clang++.
  if(SWIFT_COMPILER_IS_MSVC_LIKE)
    set(CMAKE_CXX_COMPILER "${SWIFT_NATIVE_CLANG_TOOLS_PATH}/clang-cl")
    set(CMAKE_C_COMPILER "${SWIFT_NATIVE_CLANG_TOOLS_PATH}/clang-cl")
  else()
    set(CMAKE_CXX_COMPILER "${SWIFT_NATIVE_CLANG_TOOLS_PATH}/clang++")
    set(CMAKE_C_COMPILER "${SWIFT_NATIVE_CLANG_TOOLS_PATH}/clang")
  endif()

  if(CMAKE_C_COMPILER_LAUNCHER MATCHES ".*distcc")
    set(CMAKE_C_COMPILER_LAUNCHER "")
  endif()
  if(CMAKE_CXX_COMPILER_LAUNCHER MATCHES ".*distcc")
    set(CMAKE_CXX_COMPILER_LAUNCHER "")
  endif()

  # The sanitizers require using the same version of the compiler for
  # everything and there are various places where we link runtime code with
  # code built by the host compiler. Disable sanitizers for the runtime for
  # now.
  add_compile_options(-fno-sanitize=all)
endif()

# Do not enforce checks for LLVM's ABI-breaking build settings.
# The Swift runtime uses some header-only code from LLVM's ADT classes,
# but we do not want to link libSupport into the runtime. These checks rely
# on the presence of symbols in libSupport to identify how the code was
# built and cause link failures for mismatches. Without linking that library,
# we get link failures regardless, so instead, this just disables the checks.
add_compile_definitions($<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:LLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1>)

set(SWIFT_STDLIB_LIBRARY_BUILD_TYPES)
if(SWIFT_BUILD_DYNAMIC_STDLIB)
  list(APPEND SWIFT_STDLIB_LIBRARY_BUILD_TYPES SHARED)
endif()
if(SWIFT_BUILD_STATIC_STDLIB)
  list_intersect("${SWIFT_APPLE_PLATFORMS}" "${SWIFT_SDKS}" building_darwin_sdks)
  if(building_darwin_sdks)
    message(SEND_ERROR "cannot build static standard library for Darwin SDKs")
  else()
    list(APPEND SWIFT_STDLIB_LIBRARY_BUILD_TYPES STATIC)
  endif()
endif()

function(swift_create_stdlib_targets name variant define_all_alias)
  if(NOT variant STREQUAL "")
    set(variant "-${variant}")
  endif()

  if(define_all_alias)
    add_custom_target(${name}${variant}-all)
    set_target_properties(${name}${variant}-all
      PROPERTIES
      FOLDER "Swift libraries/Aggregate")
  endif()

  foreach(sdk ${SWIFT_SDKS})
    add_custom_target(${name}-${SWIFT_SDK_${sdk}_LIB_SUBDIR}${variant})
    set_target_properties(${name}-${SWIFT_SDK_${sdk}_LIB_SUBDIR}${variant}
      PROPERTIES
      FOLDER "Swift libraries/Aggregate")

    foreach(arch ${SWIFT_SDK_${sdk}_ARCHITECTURES})
      set(target_variant -${SWIFT_SDK_${sdk}_LIB_SUBDIR}-${arch})

      add_custom_target(${name}${target_variant}${variant})
      set_target_properties(${name}${target_variant}${variant}
        PROPERTIES
        FOLDER "Swift libraries/Aggregate")
      if(define_all_alias)
        add_dependencies(${name}${variant}-all
          ${name}${target_variant}${variant})
      endif()
      add_dependencies(${name}-${SWIFT_SDK_${sdk}_LIB_SUBDIR}${variant}
        ${name}${target_variant}${variant})
    endforeach()
  endforeach()

  if(NOT define_all_alias)
    set(ALL_keyword ALL)
  endif()

  # When cross-compiling host tools for multiple architectures, targeting a
  # different SDK, the primary variant is not one of the variants being built,
  # so it can't be added as a target here. build-script will invoke the
  # more-specific target, so just skip creating this target and warn in case
  # someone is using the CMake more directly.
  if(SWIFT_PRIMARY_VARIANT_SDK IN_LIST SWIFT_SDKS)
    add_custom_target(${name}${variant}
      ${ALL_keyword}
      DEPENDS
      ${name}${SWIFT_PRIMARY_VARIANT_SUFFIX}${variant})
    set_target_properties(${name}${variant}
      PROPERTIES
      FOLDER "Swift libraries/Aggregate")
  else()
    message(WARNING "Primary variant ${SWIFT_PRIMARY_VARIANT_SDK} is not being built, not creating ${name}${variant} alias target for it.")
  endif()
endfunction()

swift_create_stdlib_targets("swift-stdlib" "" TRUE)
if(SWIFT_STDLIB_ENABLE_SIB_TARGETS)
  swift_create_stdlib_targets("swift-stdlib" "sib" TRUE)
  swift_create_stdlib_targets("swift-stdlib" "sibopt" TRUE)
  swift_create_stdlib_targets("swift-stdlib" "sibgen" TRUE)
endif()
swift_create_stdlib_targets("swift-test-stdlib" "" FALSE)

# FIXME: Include the toolchain directory before the public directory. Otherwise
# the clang resource directory symlink stops installing correctly.
add_subdirectory(toolchain)
add_subdirectory(public)
add_subdirectory(private)
add_subdirectory(tools)
