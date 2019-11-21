cmake_minimum_required(VERSION 3.4.3)

# TODO: Fix RPATH usage to be CMP0068 compliant
# Disable Policy CMP0068 for CMake 3.9
# rdar://37725888
if(POLICY CMP0068)
  cmake_policy(SET CMP0068 OLD)
endif()

# Add path for custom CMake modules.
list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

set(CMAKE_DISABLE_IN_SOURCE_BUILD YES)

if(DEFINED CMAKE_JOB_POOLS)
  # CMake < 3.11 doesn't support CMAKE_JOB_POOLS. Manually set the property.
  set_property(GLOBAL PROPERTY JOB_POOLS "${CMAKE_JOB_POOLS}")
else()
  # Make a job pool for things that can't yet be distributed
  cmake_host_system_information(
    RESULT localhost_logical_cores QUERY NUMBER_OF_LOGICAL_CORES)
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS local_jobs=${localhost_logical_cores})
  # Put linking in that category
  set(CMAKE_JOB_POOL_LINK local_jobs)
endif()

ENABLE_LANGUAGE(C)

# First include general CMake utilities.
include(SwiftUtils)
include(CheckSymbolExists)

#
# User-configurable options that control the inclusion and default build
# behavior for components which may not strictly be necessary (tools, examples,
# and tests).
#
# This is primarily to support building smaller or faster project files.
#

option(SWIFT_BUILD_TOOLS
  "Build the swift tools.  If OFF, just generate build targets."  TRUE)

option(SWIFT_BUILD_REMOTE_MIRROR
    "Build the Swift Remote Mirror Library"
    TRUE)

option(SWIFT_BUILD_DYNAMIC_STDLIB
    "Build dynamic variants of the Swift standard library"
    TRUE)

option(SWIFT_BUILD_STATIC_STDLIB
    "Build static variants of the Swift standard library"
    FALSE)

option(SWIFT_BUILD_DYNAMIC_SDK_OVERLAY
    "Build dynamic variants of the Swift SDK overlay"
    TRUE)

option(SWIFT_BUILD_STATIC_SDK_OVERLAY
    "Build static variants of the Swift SDK overlay"
    FALSE)

option(SWIFT_BUILD_STDLIB_EXTRA_TOOLCHAIN_CONTENT
    "If not building stdlib, controls whether to build 'stdlib/toolchain' content"
    TRUE)

# In many cases, the CMake build system needs to determine whether to include
# a directory, or perform other actions, based on whether the stdlib or SDK is
# being built at all -- statically or dynamically. Please note that these
# flags are not related to the deprecated build-script-impl arguments
# 'build-swift-stdlib' and 'build-swift-sdk-overlay'. These are not flags that
# the build script should be able to set.
if(SWIFT_BUILD_DYNAMIC_STDLIB OR SWIFT_BUILD_STATIC_STDLIB)
  set(SWIFT_BUILD_STDLIB TRUE)
else()
  set(SWIFT_BUILD_STDLIB FALSE)
endif()

if(SWIFT_BUILD_DYNAMIC_SDK_OVERLAY OR SWIFT_BUILD_STATIC_SDK_OVERLAY)
  set(SWIFT_BUILD_SDK_OVERLAY TRUE)
else()
  set(SWIFT_BUILD_SDK_OVERLAY FALSE)
endif()

option(SWIFT_BUILD_PERF_TESTSUITE
    "Create in-tree targets for building swift performance benchmarks."
    FALSE)

option(SWIFT_BUILD_EXTERNAL_PERF_TESTSUITE
    "Create out-of-tree targets for building swift performance benchmarks."
    FALSE)

option(SWIFT_INCLUDE_TESTS "Create targets for building/running tests." TRUE)

option(SWIFT_INCLUDE_DOCS
    "Create targets for building docs."
    TRUE)

set(_swift_include_apinotes_default FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(_swift_include_apinotes_default TRUE)
endif()

option(SWIFT_INCLUDE_APINOTES
  "Create targets for installing the remaining apinotes in the built toolchain."
  ${_swift_include_apinotes_default})

#
# Miscellaneous User-configurable options.
#
# TODO: Please categorize these!
#

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING
      "Build type for Swift [Debug, RelWithDebInfo, Release, MinSizeRel]"
      FORCE)
  message(STATUS "No build type was specified, will default to ${CMAKE_BUILD_TYPE}")
endif()

set(SWIFT_ANALYZE_CODE_COVERAGE FALSE CACHE STRING
    "Build Swift with code coverage instrumenting enabled [FALSE, NOT-MERGED, MERGED]")

# SWIFT_VERSION is deliberately /not/ cached so that an existing build directory
# can be reused when a new version of Swift comes out (assuming the user hasn't
# manually set it as part of their own CMake configuration).
set(SWIFT_VERSION "5.1.1")

set(SWIFT_VENDOR "" CACHE STRING
    "The vendor name of the Swift compiler")
set(SWIFT_COMPILER_VERSION "" CACHE STRING
    "The internal version of the Swift compiler")
set(CLANG_COMPILER_VERSION "" CACHE STRING
    "The internal version of the Clang compiler")

# Indicate whether Swift should attempt to use the lld linker.
set(SWIFT_ENABLE_LLD_LINKER TRUE CACHE BOOL
    "Enable using the lld linker when available")

# Indicate whether Swift should attempt to use the gold linker.
# This is not used on Darwin.
set(SWIFT_ENABLE_GOLD_LINKER TRUE CACHE BOOL
    "Enable using the gold linker when available")

set(SWIFT_TOOLS_ENABLE_LTO OFF CACHE STRING "Build Swift tools with LTO. One
    must specify the form of LTO by setting this to one of: 'full', 'thin'. This
    option only affects the tools that run on the host (the compiler), and has
    no effect on the target libraries (the standard library and the runtime).")

# The following only works with the Ninja generator in CMake >= 3.0.
set(SWIFT_PARALLEL_LINK_JOBS "" CACHE STRING
  "Define the maximum number of linker jobs for swift.")

option(SWIFT_FORCE_OPTIMIZED_TYPECHECKER "Override the optimization setting of
  the type checker so that it always compiles with optimization. This eases
  debugging after type checking occurs by speeding up type checking" FALSE)

# Allow building Swift with Clang's Profile Guided Optimization
if(SWIFT_PROFDATA_FILE AND EXISTS ${SWIFT_PROFDATA_FILE})
  if(NOT CMAKE_C_COMPILER_ID MATCHES Clang)
    message(FATAL_ERROR "SWIFT_PROFDATA_FILE can only be specified when compiling with clang")
  endif()
  add_definitions("-fprofile-instr-use=${SWIFT_PROFDATA_FILE}")
endif()

#
# User-configurable Swift Standard Library specific options.
#
# TODO: Once the stdlib/compiler builds are split, this should be sunk into the
# stdlib cmake.
#

set(SWIFT_STDLIB_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE STRING
    "Build type for the Swift standard library and SDK overlays [Debug, RelWithDebInfo, Release, MinSizeRel]")
# Allow the user to specify the standard library CMAKE_MSVC_RUNTIME_LIBRARY
# value.  The following values are valid:
#   - MultiThreaded (/MT)
#   - MultiThreadedDebug (/MTd)
#   - MultiThreadedDLL (/MD)
#   - MultiThreadedDebugDLL (/MDd)
if(CMAKE_BUILD_TYPE STREQUAL Debug)
  set(SWIFT_STDLIB_MSVC_RUNTIME_LIBRARY_default MultiThreadedDebugDLL)
else()
  set(SWIFT_STDLIB_MSVC_RUNTIME_LIBRARY_default MultiThreadedDLL)
endif()
set(SWIFT_STDLIB_MSVC_RUNTIME_LIBRARY
  ${SWIFT_STDLIB_MSVC_RUNTIME_LIBRARY_default}
  CACHE STRING "MSVC Runtime Library for the standard library")

is_build_type_optimized("${SWIFT_STDLIB_BUILD_TYPE}" swift_optimized)
if(swift_optimized)
  set(SWIFT_STDLIB_ASSERTIONS_default FALSE)
else()
  set(SWIFT_STDLIB_ASSERTIONS_default TRUE)
endif()
option(SWIFT_STDLIB_ASSERTIONS
    "Enable internal checks for the Swift standard library (useful for debugging the library itself, does not affect checks required for safety)"
    "${SWIFT_STDLIB_ASSERTIONS_default}")

option(SWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER
       "Use the host compiler and not the internal clang to build the swift runtime"
       FALSE)

set(SWIFT_SDKS "" CACHE STRING
    "If non-empty, limits building target binaries only to specified SDKs (despite other SDKs being available)")

set(SWIFT_PRIMARY_VARIANT_SDK "" CACHE STRING
    "Primary SDK for target binaries")
set(SWIFT_PRIMARY_VARIANT_ARCH "" CACHE STRING
    "Primary arch for target binaries")

set(SWIFT_NATIVE_LLVM_TOOLS_PATH "" CACHE STRING
    "Path to the directory that contains LLVM tools that are executable on the build machine")

set(SWIFT_NATIVE_CLANG_TOOLS_PATH "" CACHE STRING
    "Path to the directory that contains Clang tools that are executable on the build machine")

set(SWIFT_NATIVE_SWIFT_TOOLS_PATH "" CACHE STRING
   "Path to the directory that contains Swift tools that are executable on the build machine")

option(SWIFT_ENABLE_MODULE_INTERFACES
       "Generate .swiftinterface files alongside .swiftmodule files"
       TRUE)

option(SWIFT_STDLIB_ENABLE_SIB_TARGETS
       "Should we generate sib targets for the stdlib or not?"
       FALSE)


set(SWIFT_DARWIN_SUPPORTED_ARCHS "" CACHE STRING
  "Semicolon-separated list of architectures to configure on Darwin platforms. \
If left empty all default architectures are configured.")

set(SWIFT_DARWIN_MODULE_ARCHS "" CACHE STRING
  "Semicolon-separated list of architectures to configure Swift module-only \
targets on Darwin platforms. These targets are in addition to the full \
library targets.")


#
# User-configurable Android specific options.
#

set(SWIFT_ANDROID_API_LEVEL "" CACHE STRING
  "Version number for the Android API")

set(SWIFT_ANDROID_NDK_PATH "" CACHE STRING
  "Path to the directory that contains the Android NDK tools that are executable on the build machine")
set(SWIFT_ANDROID_NDK_GCC_VERSION "" CACHE STRING
  "The GCC version to use when building for Android. Currently only 4.9 is supported.")
set(SWIFT_ANDROID_DEPLOY_DEVICE_PATH "" CACHE STRING
  "Path on an Android device where build products will be pushed. These are used when running the test suite against the device")

#
# User-configurable ICU specific options for Android, FreeBSD, Linux and Haiku.
#

foreach(sdk ANDROID;FREEBSD;LINUX;WINDOWS;HAIKU)
  foreach(arch aarch64;armv6;armv7;i686;powerpc64;powerpc64le;s390x;x86_64)
    set(SWIFT_${sdk}_${arch}_ICU_UC "" CACHE STRING
        "Path to a directory containing the icuuc library for ${sdk}")
    set(SWIFT_${sdk}_${arch}_ICU_UC_INCLUDE "" CACHE STRING
        "Path to a directory containing headers for icuuc for ${sdk}")
    set(SWIFT_${sdk}_${arch}_ICU_I18N "" CACHE STRING
        "Path to a directory containing the icui18n library for ${sdk}")
    set(SWIFT_${sdk}_${arch}_ICU_I18N_INCLUDE "" CACHE STRING
        "Path to a directory containing headers icui18n for ${sdk}")
  endforeach()
endforeach()

#
# User-configurable Darwin-specific options.
#
option(SWIFT_EMBED_BITCODE_SECTION
    "If non-empty, embeds LLVM bitcode binary sections in the standard library and overlay binaries for supported platforms"
    FALSE)

option(SWIFT_EMBED_BITCODE_SECTION_HIDE_SYMBOLS
  "If non-empty, when embedding the LLVM bitcode binary sections into the relevant binaries, pass in -bitcode_hide_symbols. Does nothing if SWIFT_EMBED_BITCODE_SECTION is set to false."
  FALSE)

option(SWIFT_RUNTIME_CRASH_REPORTER_CLIENT
    "Whether to enable CrashReporter integration"
    FALSE)

set(SWIFT_DARWIN_XCRUN_TOOLCHAIN "XcodeDefault" CACHE STRING
    "The name of the toolchain to pass to 'xcrun'")

set(SWIFT_DARWIN_STDLIB_INSTALL_NAME_DIR "/usr/lib/swift" CACHE STRING
    "The directory of the install_name for standard library dylibs")

# We don't want to use the same install_name_dir as the standard library which
# will be installed in /usr/lib/swift. These private libraries should continue
# to use @rpath for now.
set(SWIFT_DARWIN_STDLIB_PRIVATE_INSTALL_NAME_DIR "@rpath" CACHE STRING
    "The directory of the install_name for the private standard library dylibs")

set(SWIFT_DARWIN_DEPLOYMENT_VERSION_OSX "10.9" CACHE STRING
    "Minimum deployment target version for OS X")

set(SWIFT_DARWIN_DEPLOYMENT_VERSION_IOS "7.0" CACHE STRING
    "Minimum deployment target version for iOS")

set(SWIFT_DARWIN_DEPLOYMENT_VERSION_TVOS "9.0" CACHE STRING
    "Minimum deployment target version for tvOS")

set(SWIFT_DARWIN_DEPLOYMENT_VERSION_WATCHOS "2.0" CACHE STRING
    "Minimum deployment target version for watchOS")

#
# User-configurable debugging options.
#

option(SWIFT_AST_VERIFIER
    "Enable the AST verifier in the built compiler, and run it on every compilation"
    TRUE)

option(SWIFT_SIL_VERIFY_ALL
    "Run SIL verification after each transform when building Swift files in the build process"
    FALSE)

option(SWIFT_EMIT_SORTED_SIL_OUTPUT
    "Sort SIL output by name to enable diffing of output"
    FALSE)

if(SWIFT_STDLIB_ASSERTIONS)
  set(SWIFT_RUNTIME_CLOBBER_FREED_OBJECTS_default TRUE)
else()
  set(SWIFT_RUNTIME_CLOBBER_FREED_OBJECTS_default FALSE)
endif()

option(SWIFT_RUNTIME_CLOBBER_FREED_OBJECTS
    "Overwrite memory for deallocated Swift objects"
    "${SWIFT_RUNTIME_CLOBBER_FREED_OBJECTS_default}")

option(SWIFT_STDLIB_SIL_DEBUGGING
    "Compile the Swift standard library with -gsil to enable debugging and profiling on SIL level"
    FALSE)

option(SWIFT_CHECK_INCREMENTAL_COMPILATION
    "Check if incremental compilation works when compiling the Swift libraries"
    FALSE)

option(SWIFT_REPORT_STATISTICS
    "Create json files which contain internal compilation statistics"
    FALSE)

#
# User-configurable experimental options.  Do not use in production builds.
#

set(SWIFT_EXPERIMENTAL_EXTRA_FLAGS "" CACHE STRING
    "Extra flags to pass when compiling swift files.  Use this option *only* for one-off experiments")

set(SWIFT_EXPERIMENTAL_EXTRA_REGEXP_FLAGS "" CACHE STRING
  "A list of [module_regexp1;flags1;module_regexp2;flags2,...] which can be used to apply specific flags to modules that match a cmake regexp. It always applies the first regexp that matches.")

set(SWIFT_EXPERIMENTAL_EXTRA_NEGATIVE_REGEXP_FLAGS "" CACHE STRING
    "A list of [module_regexp1;flags1;module_regexp2;flags2,...] which can be used to apply specific flags to modules that do not match a cmake regexp. It always applies the first regexp that does not match. The reason this is necessary is that cmake does not provide negative matches in the regex. Instead you have to use NOT in the if statement requiring a separate variable.")

option(SWIFT_RUNTIME_ENABLE_LEAK_CHECKER
  "Should the runtime be built with support for non-thread-safe leak detecting entrypoints"
  FALSE)

option(SWIFT_STDLIB_USE_NONATOMIC_RC
    "Build the standard libraries and overlays with nonatomic reference count operations enabled"
    FALSE)

option(SWIFT_ENABLE_RUNTIME_FUNCTION_COUNTERS
  "Enable runtime function counters and expose the API."
  FALSE)

option(SWIFT_ENABLE_STDLIBCORE_EXCLUSIVITY_CHECKING
  "Build stdlibCore with exclusivity checking enabled"
  FALSE)

option(SWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING
  "Enable experimental Swift differentiable programming features"
  FALSE)

#
# End of user-configurable options.
#

# Look for either a program in execute_process()'s path or for a hardcoded path.
# Find a program's version and set it in the parent scope.
# Replace newlines with spaces so it prints on one line.
function(find_version cmd flag find_in_path)
  if(find_in_path)
    message(STATUS "Finding installed version for: ${cmd}")
  else()
    message(STATUS "Finding version for: ${cmd}")
  endif()
  execute_process(
    COMMAND ${cmd} ${flag}
    OUTPUT_VARIABLE out
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT out)
    if(find_in_path)
      message(STATUS "tried to find version for ${cmd}, but ${cmd} not found in path, continuing")
    else()
      message(FATAL_ERROR "tried to find version for ${cmd}, but ${cmd} not found")
    endif()
  else()
    string(REPLACE "\n" " " out2 ${out})
    message(STATUS "Found version: ${out2}")
  endif()
  message(STATUS "")
endfunction()

# Print out path and version of any installed commands.
# We migth be using the wrong version of a command, so record them all.
function(print_versions)
  find_version("${CMAKE_COMMAND}" "--version" TRUE)

  message(STATUS "Finding version for: ${CMAKE_COMMAND}")
  message(STATUS "Found version: ${CMAKE_VERSION}")
  message(STATUS "")

  get_filename_component(CMAKE_MAKE_PROGRAM_BN "${CMAKE_MAKE_PROGRAM}" NAME_WE)
  if(${CMAKE_MAKE_PROGRAM_BN} STREQUAL "ninja" OR
     ${CMAKE_MAKE_PROGRAM_BN} STREQUAL "make")
    find_version(${CMAKE_MAKE_PROGRAM_BN} "--version" TRUE)
    find_version(${CMAKE_MAKE_PROGRAM} "--version" FALSE)
  endif()

  if(${SWIFT_PATH_TO_CMARK_BUILD})
    find_version("cmark" "--version" TRUE)
    find_version("${SWIFT_PATH_TO_CMARK_BUILD}/src/cmark" "--version" FALSE)
  endif()

  message(STATUS "Finding version for: ${CMAKE_C_COMPILER}")
  message(STATUS "Found version: ${CMAKE_C_COMPILER_VERSION}")
  message(STATUS "")

  message(STATUS "Finding version for: ${CMAKE_CXX_COMPILER}")
  message(STATUS "Found version: ${CMAKE_CXX_COMPILER_VERSION}")
  message(STATUS "")
endfunction()


set(SWIFT_BUILT_STANDALONE FALSE)
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
  set(SWIFT_BUILT_STANDALONE TRUE)
endif()

if(SWIFT_BUILT_STANDALONE)
  project(Swift C CXX ASM)
endif()

if(MSVC OR "${CMAKE_SIMULATE_ID}" STREQUAL MSVC)
  include(ClangClCompileRules)
endif()

if(CMAKE_C_COMPILER_ID MATCHES Clang)
  add_compile_options($<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:-Werror=gnu>)
endif()

option(SWIFT_BUILD_SYNTAXPARSERLIB "Build the Swift Syntax Parser library" TRUE)
option(SWIFT_BUILD_ONLY_SYNTAXPARSERLIB "Only build the Swift Syntax Parser library" FALSE)
option(SWIFT_BUILD_SOURCEKIT "Build SourceKit" TRUE)
option(SWIFT_ENABLE_SOURCEKIT_TESTS "Enable running SourceKit tests" ${SWIFT_BUILD_SOURCEKIT})

if(SWIFT_BUILD_SYNTAXPARSERLIB OR SWIFT_BUILD_SOURCEKIT)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL Darwin)
    if(NOT EXISTS "${SWIFT_PATH_TO_LIBDISPATCH_SOURCE}")
      message(SEND_ERROR "SyntaxParserLib and SourceKit require libdispatch on non-Darwin hosts.  Please specify SWIFT_PATH_TO_LIBDISPATCH_SOURCE")
    endif()
  endif()
endif()

#
# Assume a new enough ar to generate the index at construction time. This avoids
# having to invoke ranlib as a secondary command.
#

set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> crs <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_APPEND "<CMAKE_AR> qs <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH "")

set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> crs <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_CXX_ARCHIVE_APPEND "<CMAKE_AR> qs <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_CXX_ARCHIVE_FINISH "")

#
# Include CMake modules
#

include(CheckCXXSourceRuns)
include(CMakeParseArguments)
include(CMakePushCheckState)

print_versions()

include(SwiftSharedCMakeConfig)

# NOTE: We include this before SwiftComponents as it relies on some LLVM CMake
# functionality.
# Support building Swift as a standalone project, using LLVM as an
# external library.
if(SWIFT_BUILT_STANDALONE)
  swift_common_standalone_build_config(SWIFT)
else()
  swift_common_unified_build_config(SWIFT)
endif()

include(SwiftComponents)
include(SwiftHandleGybSources)
include(SwiftSetIfArchBitness)
include(SwiftSource)
include(AddSwift)
include(SwiftConfigureSDK)
include(SwiftComponents)
include(SwiftList)

# Configure swift include, install, build components.
swift_configure_components()

# lipo is used to create universal binaries.
include(SwiftToolchainUtils)
if(NOT SWIFT_LIPO)
  find_toolchain_tool(SWIFT_LIPO "${SWIFT_DARWIN_XCRUN_TOOLCHAIN}" lipo)
endif()

# Reset CMAKE_SYSTEM_PROCESSOR if not cross-compiling.
# CMake refuses to use `uname -m` on OS X
# http://public.kitware.com/Bug/view.php?id=10326
if(NOT CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR STREQUAL "i386")
  execute_process(
      COMMAND "uname" "-m"
      OUTPUT_VARIABLE CMAKE_SYSTEM_PROCESSOR
      OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()

get_filename_component(SWIFT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR} REALPATH)
set(SWIFT_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(SWIFT_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
set(SWIFT_MAIN_INCLUDE_DIR "${SWIFT_SOURCE_DIR}/include")
set(SWIFT_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")

set(SWIFT_RUNTIME_OUTPUT_INTDIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/bin")
set(SWIFT_LIBRARY_OUTPUT_INTDIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/lib")
if("${SWIFT_NATIVE_SWIFT_TOOLS_PATH}" STREQUAL "")
  set(SWIFT_NATIVE_SWIFT_TOOLS_PATH "${SWIFT_RUNTIME_OUTPUT_INTDIR}")
endif()

# This setting causes all CMakeLists.txt to automatically have
# ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CURRENT_SOURCE_DIR} as an
# include_directories path. This is done for developer
# convenience. Additionally, LLVM/Clang build with this option enabled, so we
# should match them unless it is removed from LLVM/Clang as well.
#
# *NOTE* Even though these directories are added to the include path for a
# specific CMakeLists.txt, these include paths are not propagated down to
# subdirectories.
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# We'll need this once we have generated headers
include_directories(BEFORE
  ${SWIFT_MAIN_INCLUDE_DIR}
  ${SWIFT_INCLUDE_DIR}
  )

# A convenience pattern to match Darwin platforms. Example:
#  if(SWIFT_HOST_VARIANT MATCHES "${SWIFT_DARWIN_VARIANTS}")
#     ...
#  endif()
set(SWIFT_DARWIN_VARIANTS "^(macosx|iphoneos|iphonesimulator|appletvos|appletvsimulator|watchos|watchsimulator)")
set(SWIFT_DARWIN_EMBEDDED_VARIANTS "^(iphoneos|iphonesimulator|appletvos|appletvsimulator|watchos|watchsimulator)")

# A convenient list to match Darwin SDKs. Example:
#  if("${SWIFT_HOST_VARIANT_SDK}" IN_LIST SWIFT_APPLE_PLATFORMS)
#    ...
#  endif()
set(SWIFT_APPLE_PLATFORMS "IOS" "IOS_SIMULATOR" "TVOS" "TVOS_SIMULATOR" "WATCHOS" "WATCHOS_SIMULATOR" "OSX")

# Configuration flags passed to all of our invocations of gyb.  Try to
# avoid making up new variable names here if you can find a CMake
# variable that will do the job.
set(SWIFT_GYB_FLAGS
    "-DunicodeGraphemeBreakPropertyFile=${SWIFT_SOURCE_DIR}/utils/UnicodeData/GraphemeBreakProperty.txt"
    "-DunicodeGraphemeBreakTestFile=${SWIFT_SOURCE_DIR}/utils/UnicodeData/GraphemeBreakTest.txt")

# Directory to use as the Clang module cache when building Swift source files.
set(SWIFT_MODULE_CACHE_PATH
    "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/module-cache")

# Xcode: use libc++ and c++11 using proper build settings.
if(XCODE)
  swift_common_xcode_cxx_config()
endif()

include(SwiftCheckCXXNativeRegex)
check_cxx_native_regex(SWIFT_HAVE_WORKING_STD_REGEX)

# If SWIFT_HOST_VARIANT_SDK not given, try to detect from the CMAKE_SYSTEM_NAME.
if(SWIFT_HOST_VARIANT_SDK)
  set(SWIFT_HOST_VARIANT_SDK_default "${SWIFT_HOST_VARIANT_SDK}")
else()
  if("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    # CMake on an Android host sets this to Linux, so check for the ANDROID_DATA
    # environment variable to see if we're building on Android.
    if(NOT "$ENV{ANDROID_DATA}" STREQUAL "")
      set(SWIFT_HOST_VARIANT_SDK_default "ANDROID")
    else()
      set(SWIFT_HOST_VARIANT_SDK_default "LINUX")
    endif()
  elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "FreeBSD")
    set(SWIFT_HOST_VARIANT_SDK_default "FREEBSD")
  elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "CYGWIN")
    set(SWIFT_HOST_VARIANT_SDK_default "CYGWIN")
  elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
    set(SWIFT_HOST_VARIANT_SDK_default "WINDOWS")
  elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Haiku")
    set(SWIFT_HOST_VARIANT_SDK_default "HAIKU")
  elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
    # CMAKE_SYSTEM_NAME might be set this way when cross-compiling to Android.
    set(SWIFT_HOST_VARIANT_SDK_default "ANDROID")
  elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin")
    set(SWIFT_HOST_VARIANT_SDK_default "OSX")
  else()
    message(FATAL_ERROR "Unable to detect SDK for host system: ${CMAKE_SYSTEM_NAME}")
  endif()
endif()

# If SWIFT_HOST_VARIANT_ARCH not given, try to detect from the CMAKE_SYSTEM_PROCESSOR.
if(SWIFT_HOST_VARIANT_ARCH)
  set(SWIFT_HOST_VARIANT_ARCH_default "${SWIFT_HOST_VARIANT_ARCH}")
else()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(SWIFT_HOST_VARIANT_ARCH_default "x86_64")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
    set(SWIFT_HOST_VARIANT_ARCH_default "aarch64")
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "ppc64")
    set(SWIFT_HOST_VARIANT_ARCH_default "powerpc64")
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "ppc64le")
    set(SWIFT_HOST_VARIANT_ARCH_default "powerpc64le")
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "s390x")
    set(SWIFT_HOST_VARIANT_ARCH_default "s390x")
  # FIXME: Only matches v6l/v7l - by far the most common variants
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv6l")
    set(SWIFT_HOST_VARIANT_ARCH_default "armv6")
  elseif("${CMAKE_SYSTEM_PROCESSOR}" MATCHES "armv7l|armv7-a")
    set(SWIFT_HOST_VARIANT_ARCH_default "armv7")
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "IA64")
    set(SWIFT_HOST_VARIANT_ARCH_default "itanium")
  elseif("${CMAKE_SYSTEM_PROCESSOR}" MATCHES "(x86|i686)")
    set(SWIFT_HOST_VARIANT_ARCH_default "i686")
  else()
    message(FATAL_ERROR "Unrecognized architecture on host system: ${CMAKE_SYSTEM_PROCESSOR}")
  endif()
endif()

set(SWIFT_HOST_VARIANT_SDK "${SWIFT_HOST_VARIANT_SDK_default}" CACHE STRING
    "Deployment sdk for Swift host tools (the compiler).")
set(SWIFT_HOST_VARIANT_ARCH "${SWIFT_HOST_VARIANT_ARCH_default}" CACHE STRING
    "Deployment arch for Swift host tools (the compiler).")

#
# Enable additional warnings.
#
swift_common_cxx_warnings()

# Check if we're build with MSVC or Clang-cl, as these compilers have similar command line arguments.
if("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC" OR "${CMAKE_CXX_SIMULATE_ID}" STREQUAL "MSVC")
  set(SWIFT_COMPILER_IS_MSVC_LIKE TRUE)
endif()

#
# Configure SDKs.
#

if(XCODE)
  # FIXME: Cannot cross-compile the standard library using Xcode.  Xcode
  # insists on passing -mmacosx-version-min to the compiler, and we need
  # to pass -mios-version-min.  Clang sees both options and complains.
  set(SWIFT_SDKS "OSX")
endif()

# FIXME: the parameters we specify in SWIFT_SDKS are lacking architecture specifics,
# so we need to hard-code it. For example, the SDK for Android is just 'ANDROID',
# and we have to specify SWIFT_SDK_ANDROID_ARCHITECTURES separately.
# The iOS SDKs all have their architectures hardcoded because they are just specified by name (e.g. 'IOS' or 'WATCHOS').
# We can't cross-compile the standard library for another linux architecture,
# because the SDK list would just be 'LINUX' and we couldn't disambiguate it from the host.
#
# To fix it, we would need to append the architecture to the SDKs,
# for example: 'OSX-x86_64;IOS-armv7;...etc'.
# We could easily do that - we have all of that information in build-script-impl.
# Darwin targets cheat and use `xcrun`.

if("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "LINUX")

  set(SWIFT_HOST_VARIANT "linux" CACHE STRING
      "Deployment OS for Swift host tools (the compiler) [linux].")

  # Should we build the standard library for the host?
  is_sdk_requested(LINUX swift_build_linux)
  if(swift_build_linux)
    configure_sdk_unix("Linux" "${SWIFT_HOST_VARIANT_ARCH}")
    set(SWIFT_PRIMARY_VARIANT_SDK_default  "${SWIFT_HOST_VARIANT_SDK}")
    set(SWIFT_PRIMARY_VARIANT_ARCH_default "${SWIFT_HOST_VARIANT_ARCH}")
  endif()

elseif("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "FREEBSD")

  set(SWIFT_HOST_VARIANT "freebsd" CACHE STRING
      "Deployment OS for Swift host tools (the compiler) [freebsd].")

  configure_sdk_unix("FreeBSD" "${SWIFT_HOST_VARIANT_ARCH}")
  set(SWIFT_PRIMARY_VARIANT_SDK_default  "${SWIFT_HOST_VARIANT_SDK}")
  set(SWIFT_PRIMARY_VARIANT_ARCH_default "${SWIFT_HOST_VARIANT_ARCH}")

elseif("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "CYGWIN")

  set(SWIFT_HOST_VARIANT "cygwin" CACHE STRING
      "Deployment OS for Swift host tools (the compiler) [cygwin].")

  configure_sdk_unix("Cygwin" "${SWIFT_HOST_VARIANT_ARCH}")
  set(SWIFT_PRIMARY_VARIANT_SDK_default "${SWIFT_HOST_VARIANT_SDK}")
  set(SWIFT_PRIMARY_VARIANT_ARCH_default "${SWIFT_HOST_VARIANT_ARCH}")

elseif("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "WINDOWS")

  set(SWIFT_HOST_VARIANT "windows" CACHE STRING
      "Deployment OS for Swift host tools (the compiler) [windows].")

  configure_sdk_windows("Windows" "msvc" "${SWIFT_HOST_VARIANT_ARCH}")
  set(SWIFT_PRIMARY_VARIANT_SDK_default  "${SWIFT_HOST_VARIANT_SDK}")
  set(SWIFT_PRIMARY_VARIANT_ARCH_default "${SWIFT_HOST_VARIANT_ARCH}")

elseif("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "HAIKU")

  set(SWIFT_HOST_VARIANT "haiku" CACHE STRING
      "Deployment OS for Swift host tools (the compiler) [haiku].")

  configure_sdk_unix("Haiku" "${SWIFT_HOST_VARIANT_ARCH}")
  set(SWIFT_PRIMARY_VARIANT_SDK_default  "${SWIFT_HOST_VARIANT_SDK}")
  set(SWIFT_PRIMARY_VARIANT_ARCH_default "${SWIFT_HOST_VARIANT_ARCH}")

elseif("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "ANDROID")

  set(SWIFT_HOST_VARIANT "android" CACHE STRING
      "Deployment OS for Swift host tools (the compiler) [android]")

  set(SWIFT_ANDROID_NATIVE_SYSROOT "/data/data/com.termux/files" CACHE STRING
      "Path to Android sysroot, default initialized to the Termux app's layout")

  if("${SWIFT_SDK_ANDROID_ARCHITECTURES}" STREQUAL "")
    set(SWIFT_SDK_ANDROID_ARCHITECTURES ${SWIFT_HOST_VARIANT_ARCH})
  endif()

  configure_sdk_unix("Android" "${SWIFT_SDK_ANDROID_ARCHITECTURES}")
  set(SWIFT_PRIMARY_VARIANT_SDK_default  "${SWIFT_HOST_VARIANT_SDK}")
  set(SWIFT_PRIMARY_VARIANT_ARCH_default "${SWIFT_HOST_VARIANT_ARCH}")

elseif("${SWIFT_HOST_VARIANT_SDK}" MATCHES "(OSX|IOS*|TVOS*|WATCHOS*)")

  set(SWIFT_HOST_VARIANT "macosx" CACHE STRING
      "Deployment OS for Swift host tools (the compiler) [macosx, iphoneos].")

  # Display Xcode toolchain version.
  # The SDK configuration below prints each SDK version.
  execute_process(
    COMMAND "xcodebuild" "-version"
    OUTPUT_VARIABLE xcode_version
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REPLACE "\n" ", " xcode_version "${xcode_version}")
  message(STATUS "${xcode_version}")
  message(STATUS "")

  include(DarwinSDKs)

  # FIXME: guess target variant based on the host.
  # if(SWIFT_HOST_VARIANT MATCHES "^macosx")
  #   set(SWIFT_PRIMARY_VARIANT_GUESS "OSX-R")
  # elseif(SWIFT_HOST_VARIANT MATCHES "^iphoneos")
  #   set(SWIFT_PRIMARY_VARIANT_GUESS "IOS-R")
  # else()
  #   message(FATAL_ERROR "Unknown SWIFT_HOST_VARIANT '${SWIFT_HOST_VARIANT}'")
  # endif()
  #
  # set(SWIFT_PRIMARY_VARIANT ${SWIFT_PRIMARY_VARIANT_GUESS} CACHE STRING
  #    "[OSX-DA, OSX-RA, OSX-R, IOS-DA, IOS-RA, IOS-R, IOS_SIMULATOR-DA, IOS_SIMULATOR-RA, IOS_SIMULATOR-R]")
  #
  # Primary variant is always OSX; even on iOS hosts.
  set(SWIFT_PRIMARY_VARIANT_SDK_default "OSX")
  set(SWIFT_PRIMARY_VARIANT_ARCH_default "x86_64")

endif()

if("${SWIFT_PRIMARY_VARIANT_SDK}" STREQUAL "")
  set(SWIFT_PRIMARY_VARIANT_SDK "${SWIFT_PRIMARY_VARIANT_SDK_default}")
endif()
if("${SWIFT_PRIMARY_VARIANT_ARCH}" STREQUAL "")
  set(SWIFT_PRIMARY_VARIANT_ARCH "${SWIFT_PRIMARY_VARIANT_ARCH_default}")
endif()

# Should we cross-compile the standard library for Android?
is_sdk_requested(ANDROID swift_build_android)
if(swift_build_android AND NOT "${SWIFT_HOST_VARIANT_SDK}" STREQUAL "ANDROID")
  if ("${SWIFT_ANDROID_NDK_PATH}" STREQUAL "")
    message(FATAL_ERROR "You must set SWIFT_ANDROID_NDK_PATH to cross-compile the Swift runtime for Android")
  endif()
  if (NOT ("${CMAKE_HOST_SYSTEM_NAME}" STREQUAL "Darwin" OR "${CMAKE_HOST_SYSTEM_NAME}" STREQUAL "Linux"))
    message(FATAL_ERROR "A Darwin or Linux host is required to build the Swift runtime for Android")
  endif()

  if("${SWIFT_SDK_ANDROID_ARCHITECTURES}" STREQUAL "")
    set(SWIFT_SDK_ANDROID_ARCHITECTURES armv7;aarch64)
  endif()
  configure_sdk_unix("Android" "${SWIFT_SDK_ANDROID_ARCHITECTURES}")
endif()

# Should we cross-compile the standard library for Windows?
is_sdk_requested(WINDOWS swift_build_windows)
if(swift_build_windows AND NOT "${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
  if("${SWIFT_SDK_WINDOWS_ARCHITECTURES}" STREQUAL "")
    set(SWIFT_SDK_WINDOWS_ARCHITECTURES aarch64;armv7;i686;x86_64)
  endif()
  configure_sdk_windows("Windows" "msvc" "${SWIFT_SDK_WINDOWS_ARCHITECTURES}")
endif()

if("${SWIFT_SDKS}" STREQUAL "")
  set(SWIFT_SDKS "${SWIFT_CONFIGURED_SDKS}")
endif()

list_subtract("${SWIFT_SDKS}" "${SWIFT_CONFIGURED_SDKS}" unknown_sdks)

precondition(unknown_sdks NEGATE MESSAGE "Unknown SDKs: ${unknown_sdks}")
precondition(SWIFT_CONFIGURED_SDKS MESSAGE "No SDKs selected.")
precondition(SWIFT_HOST_VARIANT_SDK MESSAGE "No SDK for host tools.")
precondition(SWIFT_HOST_VARIANT_ARCH MESSAGE "No arch for host tools")

set(SWIFT_PRIMARY_VARIANT_SUFFIX
    "-${SWIFT_SDK_${SWIFT_PRIMARY_VARIANT_SDK}_LIB_SUBDIR}-${SWIFT_PRIMARY_VARIANT_ARCH}")

# Clear universal library names to prevent adding duplicates
foreach(sdk ${SWIFT_SDKS})
  unset(UNIVERSAL_LIBRARY_NAMES_${SWIFT_SDK_${sdk}_LIB_SUBDIR} CACHE)
endforeach()

if(SWIFT_PARALLEL_LINK_JOBS)
  if(NOT CMAKE_MAKE_PROGRAM MATCHES "ninja")
    message(WARNING "Job pooling is only available with Ninja generators.")
  else()
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS swift_link_job_pool=${SWIFT_PARALLEL_LINK_JOBS})
    set(CMAKE_JOB_POOL_LINK swift_link_job_pool)
  endif()
endif()

# Set the CMAKE_OSX_* variables in a way that minimizes conflicts.
if("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin" AND NOT CMAKE_CROSSCOMPILING)
  set(CMAKE_OSX_SYSROOT "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_PATH}")
  set(CMAKE_OSX_ARCHITECTURES "")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "")
endif()

if(SWIFT_INCLUDE_TOOLS)
  message(STATUS "Building host Swift tools for ${SWIFT_HOST_VARIANT_SDK} ${SWIFT_HOST_VARIANT_ARCH}")
  message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
  message(STATUS "  Assertions:     ${LLVM_ENABLE_ASSERTIONS}")
  message(STATUS "  LTO:            ${SWIFT_TOOLS_ENABLE_LTO}")
  message(STATUS "")
else()
  message(STATUS "Not building host Swift tools")
  message(STATUS "")
endif()

if(SWIFT_BUILD_STDLIB OR SWIFT_BUILD_SDK_OVERLAY)
  message(STATUS "Building Swift standard library and overlays for SDKs: ${SWIFT_SDKS}")
  message(STATUS "  Build type:       ${SWIFT_STDLIB_BUILD_TYPE}")
  message(STATUS "  Assertions:       ${SWIFT_STDLIB_ASSERTIONS}")
  message(STATUS "")

  message(STATUS "Building Swift runtime with:")
  message(STATUS "  Leak Detection Checker Entrypoints: ${SWIFT_RUNTIME_ENABLE_LEAK_CHECKER}")
  message(STATUS "")
else()
  message(STATUS "Not building Swift standard library, SDK overlays, and runtime")
  message(STATUS "")
endif()

#
# Find required dependencies.
#

function(swift_icu_variables_set sdk arch result)
  string(TOUPPER "${sdk}" sdk)

  set(icu_var_ICU_UC_INCLUDE ${SWIFT_${sdk}_${arch}_ICU_UC_INCLUDE})
  set(icu_var_ICU_UC ${SWIFT_${sdk}_${arch}_ICU_UC})
  set(icu_var_ICU_I18N_INCLUDE ${SWIFT_${sdk}_${arch}_ICU_I18N_INCLUDE})
  set(icu_var_ICU_I18N ${SWIFT_${sdk}_${arch}_ICU_I18N})

  if(icu_var_ICU_UC_INCLUDE AND icu_var_ICU_UC AND
     icu_var_ICU_I18N_INCLUDE AND icu_var_ICU_I18N)
    set(${result} TRUE PARENT_SCOPE)
  else()
    set(${result} FALSE PARENT_SCOPE)
  endif()
endfunction()

# ICU is provided through CoreFoundation on Darwin.  On other hosts, if the ICU
# unicode and i18n include and library paths are not defined, perform a standard
# package lookup.  Otherwise, rely on the paths specified by the user.  These
# need to be defined when cross-compiling.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  if(SWIFT_BUILD_STDLIB OR SWIFT_BUILD_SDK_OVERLAY)
    swift_icu_variables_set("${SWIFT_PRIMARY_VARIANT_SDK}"
                            "${SWIFT_PRIMARY_VARIANT_ARCH}"
                            ICU_CONFIGURED)
    if("${SWIFT_PATH_TO_LIBICU_BUILD}" STREQUAL "" AND NOT ${ICU_CONFIGURED})
      find_package(ICU REQUIRED COMPONENTS uc i18n)
    endif()
  endif()
endif()

find_package(PythonInterp REQUIRED)

#
# Find optional dependencies.
#

if(LLVM_ENABLE_LIBXML2)
  find_package(Libxml2 REQUIRED)
else()
  find_package(LibXml2)
endif()

# You need libedit linked in order to check if you have el_wgets.
cmake_push_check_state()
list(APPEND CMAKE_REQUIRED_LIBRARIES "edit")
check_symbol_exists(el_wgets "histedit.h" HAVE_EL_WGETS)
if(HAVE_EL_WGETS)
  set(HAVE_UNICODE_LIBEDIT 1)
endif()
cmake_pop_check_state()

check_symbol_exists(wait4 "sys/wait.h" HAVE_WAIT4)

check_symbol_exists(proc_pid_rusage "libproc.h" HAVE_PROC_PID_RUSAGE)
if(HAVE_PROC_PID_RUSAGE)
    list(APPEND CMAKE_REQUIRED_LIBRARIES proc)
endif()

if (LLVM_ENABLE_DOXYGEN)
  message(STATUS "Doxygen: enabled")
endif()

if(SWIFT_BUILD_SYNTAXPARSERLIB OR SWIFT_BUILD_SOURCEKIT)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL Darwin)
    if(CMAKE_C_COMPILER_ID STREQUAL Clang AND
       CMAKE_C_COMPILER_VERSION VERSION_GREATER 3.8
       OR LLVM_USE_SANITIZER)
      set(SWIFT_LIBDISPATCH_C_COMPILER ${CMAKE_C_COMPILER})
      set(SWIFT_LIBDISPATCH_CXX_COMPILER ${CMAKE_CXX_COMPILER})
    elseif(${CMAKE_SYSTEM_NAME} STREQUAL ${CMAKE_HOST_SYSTEM_NAME})
      if(CMAKE_SYSTEM_NAME STREQUAL Windows)
        set(SWIFT_LIBDISPATCH_C_COMPILER
            $<TARGET_FILE_DIR:clang>/clang-cl${CMAKE_EXECUTABLE_SUFFIX})
        set(SWIFT_LIBDISPATCH_CXX_COMPILER
            $<TARGET_FILE_DIR:clang>/clang-cl${CMAKE_EXECUTABLE_SUFFIX})
      else()
        set(SWIFT_LIBDISPATCH_C_COMPILER $<TARGET_FILE_DIR:clang>/clang)
        set(SWIFT_LIBDISPATCH_CXX_COMPILER $<TARGET_FILE_DIR:clang>/clang++)
      endif()
    else()
      message(SEND_ERROR "libdispatch requires a newer clang compiler (${CMAKE_C_COMPILER_VERSION} < 3.9)")
    endif()

    if(SWIFT_HOST_VARIANT_SDK STREQUAL WINDOWS)
      set(SOURCEKIT_LIBDISPATCH_RUNTIME_DIR bin)
    else()
      set(SOURCEKIT_LIBDISPATCH_RUNTIME_DIR lib)
    endif()

    include(ExternalProject)
    ExternalProject_Add(libdispatch
                        SOURCE_DIR
                          "${SWIFT_PATH_TO_LIBDISPATCH_SOURCE}"
                        CMAKE_ARGS
                          -DCMAKE_AR=${CMAKE_AR}
                          -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
                          -DCMAKE_C_COMPILER=${SWIFT_LIBDISPATCH_C_COMPILER}
                          -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
                          -DCMAKE_CXX_COMPILER=${SWIFT_LIBDISPATCH_CXX_COMPILER}
                          -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
                          -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
                          -DCMAKE_INSTALL_LIBDIR=lib
                          -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
                          -DCMAKE_LINKER=${CMAKE_LINKER}
                          -DCMAKE_RANLIB=${CMAKE_RANLIB}
                          -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
                          -DBUILD_SHARED_LIBS=YES
                          -DENABLE_SWIFT=NO
                          -DENABLE_TESTING=NO
                        INSTALL_COMMAND
                          # NOTE(compnerd) provide a custom install command to
                          # ensure that we strip out the DESTDIR environment
                          # from the sub-build
                          ${CMAKE_COMMAND} -E env --unset=DESTDIR ${CMAKE_COMMAND} --build . --target install
                        STEP_TARGETS
                          install
                        BUILD_BYPRODUCTS
                          <INSTALL_DIR>/${SOURCEKIT_LIBDISPATCH_RUNTIME_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}dispatch${CMAKE_SHARED_LIBRARY_SUFFIX}
                          <INSTALL_DIR>/lib/${CMAKE_IMPORT_LIBRARY_PREFIX}dispatch${CMAKE_IMPORT_LIBRARY_SUFFIX}
                          <INSTALL_DIR>/${SOURCEKIT_LIBDISPATCH_RUNTIME_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}BlocksRuntime${CMAKE_SHARED_LIBRARY_SUFFIX}
                          <INSTALL_DIR>/lib/${CMAKE_IMPORT_LIBRARY_PREFIX}BlocksRuntime${CMAKE_IMPORT_LIBRARY_SUFFIX}
                        BUILD_ALWAYS
                          1)

    ExternalProject_Get_Property(libdispatch install_dir)

    # CMake does not like the addition of INTERFACE_INCLUDE_DIRECTORIES without
    # the directory existing.  Just create the location which will be populated
    # during the installation.
    file(MAKE_DIRECTORY ${install_dir}/include)

    add_library(dispatch SHARED IMPORTED)
    set_target_properties(dispatch
                          PROPERTIES
                            IMPORTED_LOCATION
                              ${install_dir}/${SOURCEKIT_LIBDISPATCH_RUNTIME_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}dispatch${CMAKE_SHARED_LIBRARY_SUFFIX}
                            IMPORTED_IMPLIB
                              ${install_dir}/lib/${CMAKE_IMPORT_LIBRARY_PREFIX}dispatch${CMAKE_IMPORT_LIBRARY_SUFFIX}
                            INTERFACE_INCLUDE_DIRECTORIES
                              ${install_dir}/include)

    add_library(BlocksRuntime SHARED IMPORTED)
    set_target_properties(BlocksRuntime
                          PROPERTIES
                            IMPORTED_LOCATION
                              ${install_dir}/${SOURCEKIT_LIBDISPATCH_RUNTIME_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}BlocksRuntime${CMAKE_SHARED_LIBRARY_SUFFIX}
                            IMPORTED_IMPLIB
                              ${install_dir}/lib/${CMAKE_IMPORT_LIBRARY_PREFIX}BlocksRuntime${CMAKE_IMPORT_LIBRARY_SUFFIX}
                            INTERFACE_INCLUDE_DIRECTORIES
                              ${SWIFT_PATH_TO_LIBDISPATCH_SOURCE}/src/BlocksRuntime)

    add_dependencies(dispatch libdispatch-install)
    add_dependencies(BlocksRuntime libdispatch-install)

    if(SWIFT_HOST_VARIANT_SDK STREQUAL WINDOWS)
      set(SOURCEKIT_RUNTIME_DIR bin)
    else()
      set(SOURCEKIT_RUNTIME_DIR lib)
    endif()
    add_dependencies(sourcekit-inproc BlocksRuntime dispatch)
    swift_install_in_component(FILES
                                 $<TARGET_FILE:dispatch>
                                 $<TARGET_FILE:BlocksRuntime>
                               DESTINATION ${SOURCEKIT_RUNTIME_DIR}
                               COMPONENT sourcekit-inproc)
    if(SWIFT_HOST_VARIANT_SDK STREQUAL WINDOWS)
      swift_install_in_component(FILES
                                   $<TARGET_LINKER_FILE:dispatch>
                                   $<TARGET_LINKER_FILE:BlocksRuntime>
                                 DESTINATION lib
                                 COMPONENT sourcekit-inproc)
    endif()


    # FIXME(compnerd) this should be taken care of by the
    # INTERFACE_INCLUDE_DIRECTORIES above
    include_directories(AFTER
                          ${SWIFT_PATH_TO_LIBDISPATCH_SOURCE}/src/BlocksRuntime
                          ${SWIFT_PATH_TO_LIBDISPATCH_SOURCE})
  endif()
endif()

# Add all of the subdirectories, where we actually do work.

###############
# PLEASE READ #
###############
#
# We have to include stdlib/ before tools/.
# Do not move add_subdirectory(stdlib) after add_subdirectory(tools)!
#
# We must include stdlib/ before tools/ because stdlib/CMakeLists.txt
# declares the swift-stdlib-* set of targets. These targets will then
# implicitly depend on any targets declared with IS_STDLIB.
#
# https://bugs.swift.org/browse/SR-5975
if(SWIFT_BUILD_STDLIB)
  add_subdirectory(stdlib)
else()
  if(SWIFT_BUILD_STDLIB_EXTRA_TOOLCHAIN_CONTENT)
    add_subdirectory(stdlib/toolchain)
  endif()

  # Some tools (e.g. swift-reflection-dump) rely on a host swiftReflection, so
  # ensure we build that when building tools.
  if(SWIFT_INCLUDE_TOOLS)
    add_subdirectory(stdlib/public/Reflection)
    add_subdirectory(stdlib/public/SwiftShims)
  endif()
endif()

if(SWIFT_INCLUDE_APINOTES)
  add_subdirectory(apinotes)
endif()

add_subdirectory(include)

if(SWIFT_BUILD_TOOLS)
  add_subdirectory(lib)

  # Always include this after including stdlib/!
  # Refer to the large comment above the add_subdirectory(stdlib) call.
  # https://bugs.swift.org/browse/SR-5975
  add_subdirectory(tools)
endif()

add_subdirectory(utils)

add_subdirectory(userdocs)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin")
  if(SWIFT_BUILD_PERF_TESTSUITE)
    add_subdirectory(benchmark)
  endif()
  if(SWIFT_BUILD_EXTERNAL_PERF_TESTSUITE)
    include(SwiftExternalBenchmarkBuild)
    add_external_benchmark_suite()
  endif()
endif()

if(SWIFT_INCLUDE_TESTS)
  add_subdirectory(test)
  add_subdirectory(unittests)
endif()
if(SWIFT_INCLUDE_DOCS)
  add_subdirectory(docs)
endif()

add_subdirectory(cmake/modules)

swift_install_in_component(FILES "LICENSE.txt"
                           DESTINATION "share/swift"
                           COMPONENT license)

# Add a documentation target so that documentation shows up in the
# Xcode project.
if(XCODE)
  add_custom_target(Documentation
      SOURCES
        README.md
        docs)

  file(GLOB SWIFT_TOPLEVEL_HEADERS
      ${CMAKE_CURRENT_SOURCE_DIR}/include/swift${dir}/*.h
      ${CMAKE_CURRENT_SOURCE_DIR}/include/swift${dir}/*.td
      ${CMAKE_CURRENT_SOURCE_DIR}/include/swift${dir}/*.def)
  add_custom_target(Miscellaneous
      SOURCES ${SWIFT_TOPLEVEL_HEADERS})
endif()
