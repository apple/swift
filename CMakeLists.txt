cmake_minimum_required(VERSION 3.19.6)


#  set_property(GLOBAL PROPERTY GLOBAL_DEPENDS_DEBUG_MODE 1)

# TODO: Fix RPATH usage to be CMP0068 compliant
# Disable Policy CMP0068 for CMake 3.9
# rdar://37725888
if(POLICY CMP0068)
  cmake_policy(SET CMP0068 OLD)
endif()

# Honour CMAKE_CXX_STANDARD in try_compile(), needed for check_cxx_native_regex.
if(POLICY CMP0067)
  cmake_policy(SET CMP0067 NEW)
endif()

# Convert relative paths to absolute for subdirectory `target_sources`
if(POLICY CMP0076)
  cmake_policy(SET CMP0076 NEW)
endif()

# Don't clobber existing variable values when evaluating `option()` declarations.
if(POLICY CMP0077)
  cmake_policy(SET CMP0077 NEW)
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

enable_language(C)
enable_language(CXX)

# On Windows, use MASM or MARMASM
set(SWIFT_ASM_DIALECT ASM)
set(SWIFT_ASM_EXT S)
set(SWIFT_ASM_AVAILABLE YES)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64")
    if(CMAKE_VERSION VERSION_LESS "3.26")
      message(warning "We can't build assembly language for ARM64 until CMake 3.26")
      set(SWIFT_ASM_AVAILABLE NO)
    else()
      set(SWIFT_ASM_DIALECT ASM_MARMASM)
    endif()
  else()
    set(SWIFT_ASM_DIALECT ASM_MASM)
  endif()
  set(SWIFT_ASM_EXT asm)
endif()

if(SWIFT_ASM_AVAILABLE)
  enable_language(${SWIFT_ASM_DIALECT})
endif()

# Use C++17.
set(SWIFT_MIN_CXX_STANDARD 17)

# Unset CMAKE_CXX_STANDARD if it's too low and in the CMakeCache.txt
if($CACHE{CMAKE_CXX_STANDARD} AND $CACHE{CMAKE_CXX_STANDARD} LESS ${SWIFT_MIN_CXX_STANDARD})
  message(WARNING "Resetting cache value for CMAKE_CXX_STANDARD to ${SWIFT_MIN_CXX_STANDARD}")
  unset(CMAKE_CXX_STANDARD CACHE)
endif()

# Allow manually specified CMAKE_CXX_STANDARD if it's greater than the minimum
# required C++ version
if(DEFINED CMAKE_CXX_STANDARD AND CMAKE_CXX_STANDARD LESS ${SWIFT_MIN_CXX_STANDARD})
  message(FATAL_ERROR "Requested CMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD} which is less than the minimum C++ standard ${SWIFT_MIN_CXX_STANDARD}")
endif()

set(CMAKE_CXX_STANDARD ${SWIFT_MIN_CXX_STANDARD} CACHE STRING "C++ standard to conform to")
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

# First include general CMake utilities.
include(SwiftUtils)
include(CheckSymbolExists)
include(CMakeDependentOption)
include(CheckLanguage)
include(GNUInstallDirs)
include(SwiftImplicitImport)

# Enable Swift for the host compiler build if we have the language. It is
# optional until we have a bootstrap story.
check_language(Swift)
if(CMAKE_Swift_COMPILER)
  enable_language(Swift)
else()
  message(STATUS "WARNING! Did not find a host compiler swift?! Can not build
                  any compiler host sources written in Swift")
endif()

# A convenience pattern to match Darwin platforms. Example:
#  if(SWIFT_HOST_VARIANT MATCHES "${SWIFT_DARWIN_VARIANTS}")
#     ...
#  endif()
set(SWIFT_DARWIN_VARIANTS "^(macosx|iphoneos|iphonesimulator|appletvos|appletvsimulator|watchos|watchsimulator)")
set(SWIFT_DARWIN_EMBEDDED_VARIANTS "^(iphoneos|iphonesimulator|appletvos|appletvsimulator|watchos|watchsimulator)")

# A convenient list to match Darwin SDKs. Example:
#  if("${SWIFT_HOST_VARIANT_SDK}" IN_LIST SWIFT_DARWIN_PLATFORMS)
#    ...
#  endif()
set(SWIFT_DARWIN_PLATFORMS "IOS" "IOS_SIMULATOR" "TVOS" "TVOS_SIMULATOR" "WATCHOS" "WATCHOS_SIMULATOR" "OSX")

set(SWIFT_APPLE_PLATFORMS ${SWIFT_DARWIN_PLATFORMS})
if(SWIFT_FREESTANDING_FLAVOR STREQUAL "apple")
  list(APPEND SWIFT_APPLE_PLATFORMS "FREESTANDING")
  if(SWIFT_FREESTANDING_IS_DARWIN)
    list(APPEND SWIFT_DARWIN_PLATFORMS "FREESTANDING")
  endif()
endif()

# If SWIFT_HOST_VARIANT_SDK not given, try to detect from the CMAKE_SYSTEM_NAME.
if(SWIFT_HOST_VARIANT_SDK)
  set(SWIFT_HOST_VARIANT_SDK_default "${SWIFT_HOST_VARIANT_SDK}")
else()
  if("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    set(SWIFT_HOST_VARIANT_SDK_default "LINUX")
  elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "FreeBSD")
    set(SWIFT_HOST_VARIANT_SDK_default "FREEBSD")
  elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "OpenBSD")
    set(SWIFT_HOST_VARIANT_SDK_default "OPENBSD")
  elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "CYGWIN")
    set(SWIFT_HOST_VARIANT_SDK_default "CYGWIN")
  elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
    set(SWIFT_HOST_VARIANT_SDK_default "WINDOWS")
  elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Haiku")
    set(SWIFT_HOST_VARIANT_SDK_default "HAIKU")
  elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
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
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set(SWIFT_HOST_VARIANT_ARCH_default "x86_64")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
    if(SWIFT_HOST_VARIANT_SDK_default STREQUAL "OSX")
      set(SWIFT_HOST_VARIANT_ARCH_default "arm64")
    else()
      set(SWIFT_HOST_VARIANT_ARCH_default "aarch64")
    endif()
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "ppc64")
    set(SWIFT_HOST_VARIANT_ARCH_default "powerpc64")
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "ppc")
    set(SWIFT_HOST_VARIANT_ARCH_default "powerpc")
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "ppc64le")
    set(SWIFT_HOST_VARIANT_ARCH_default "powerpc64le")
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "s390x")
    set(SWIFT_HOST_VARIANT_ARCH_default "s390x")
  elseif("${CMAKE_SYSTEM_PROCESSOR}" MATCHES "armv5|armv5te")
    set(SWIFT_HOST_VARIANT_ARCH_default "armv5")
  # FIXME: Only matches v6l/v7l - by far the most common variants
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv6l")
    set(SWIFT_HOST_VARIANT_ARCH_default "armv6")
  elseif("${CMAKE_SYSTEM_PROCESSOR}" MATCHES "armv7l|armv7-a")
    set(SWIFT_HOST_VARIANT_ARCH_default "armv7")
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "IA64")
    set(SWIFT_HOST_VARIANT_ARCH_default "itanium")
  elseif("${CMAKE_SYSTEM_PROCESSOR}" MATCHES "(x86|i686)")
    set(SWIFT_HOST_VARIANT_ARCH_default "i686")
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "wasm32")
    set(SWIFT_HOST_VARIANT_ARCH_default "wasm32")
  else()
    message(FATAL_ERROR "Unrecognized architecture on host system: ${CMAKE_SYSTEM_PROCESSOR}")
  endif()
endif()

set(SWIFT_HOST_VARIANT_SDK "${SWIFT_HOST_VARIANT_SDK_default}" CACHE STRING
    "Deployment sdk for Swift host tools (the compiler).")
set(SWIFT_HOST_VARIANT_ARCH "${SWIFT_HOST_VARIANT_ARCH_default}" CACHE STRING
    "Deployment arch for Swift host tools (the compiler).")

#
# User-configurable options that control the inclusion and default build
# behavior for components which may not strictly be necessary (tools, examples,
# and tests).
#
# This is primarily to support building smaller or faster project files.
#

option(SWIFT_APPEND_VC_REV
  "Embed the version control system revision in Swift"
  TRUE)

option(SWIFT_INCLUDE_TOOLS
    "Generate build targets for swift tools"
    TRUE)

option(SWIFT_BUILD_REMOTE_MIRROR
    "Build the Swift Remote Mirror Library"
    TRUE)

option(SWIFT_BUILD_DYNAMIC_STDLIB
    "Build dynamic variants of the Swift standard library"
    TRUE)

option(SWIFT_BUILD_STATIC_STDLIB
    "Build static variants of the Swift standard library"
    FALSE)
    
option(SWIFT_STDLIB_STATIC_PRINT
    "Build compile-time evaluated vprintf()"
    FALSE)
    
option(SWIFT_STDLIB_ENABLE_UNICODE_DATA
    "Include Unicode data files in the standard library.
    NOTE: Disabling this will cause many String methods to crash."
    TRUE)

option(SWIFT_BUILD_DYNAMIC_SDK_OVERLAY
    "Build dynamic variants of the Swift SDK overlay"
    TRUE)

option(SWIFT_BUILD_STATIC_SDK_OVERLAY
    "Build static variants of the Swift SDK overlay"
    FALSE)

option(SWIFT_BUILD_STDLIB_EXTRA_TOOLCHAIN_CONTENT
    "If not building stdlib, controls whether to build 'stdlib/toolchain' content"
    TRUE)

option(SWIFT_BUILD_STDLIB_CXX_MODULE
  "If not building stdlib, controls whether to build the Cxx module"
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

option(SWIFT_BUILD_REGEX_PARSER_IN_COMPILER
    "Build the Swift regex parser as part of the compiler."
    TRUE)

option(SWIFT_INCLUDE_TESTS "Create targets for building/running tests." TRUE)

option(SWIFT_INCLUDE_TEST_BINARIES
  "Create targets for building/running test binaries even if SWIFT_INCLUDE_TESTS is disabled"
  TRUE)

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

include(${CMAKE_CURRENT_LIST_DIR}/cmake/SwiftVersion.cmake)

set(SWIFT_VENDOR "" CACHE STRING
    "The vendor name of the Swift compiler")
set(SWIFT_COMPILER_VERSION "" CACHE STRING
    "The internal version of the Swift compiler")
set(CLANG_COMPILER_VERSION "" CACHE STRING
    "The internal version of the Clang compiler")

option(SWIFT_DISABLE_DEAD_STRIPPING
      "Turn off Darwin-specific dead stripping for Swift host tools." FALSE)

set(SWIFT_TOOLS_ENABLE_LTO OFF CACHE STRING "Build Swift tools with LTO. One
    must specify the form of LTO by setting this to one of: 'full', 'thin'. This
    option only affects the tools that run on the host (the compiler), and has
    no effect on the target libraries (the standard library and the runtime).")

option(SWIFT_TOOLS_LD64_LTO_CODEGEN_ONLY_FOR_SUPPORTING_TARGETS
    "When building ThinLTO using ld64 on Darwin, controls whether to opt out of
    LLVM IR optimizations when linking targets that will get
    little benefit from it (e.g. tools for bootstrapping or
    debugging Swift)"
    FALSE)

option(BOOTSTRAPPING_MODE [=[
How to build the swift compiler modules. Possible values are
    OFF:           build without swift modules
    HOSTTOOLS:     build with a pre-installed toolchain
    BOOTSTRAPPING: build with a 2-stage bootstrapping process
    BOOTSTRAPPING-WITH-HOSTLIBS:   build with a 2-stage bootstrapping process,
                   but the compiler links against the host system swift libs (macOS only)
    CROSSCOMPILE:  cross-compiledwith a native host compiler, provided in
                   `SWIFT_NATIVE_SWIFT_TOOLS_PATH` (non-Darwin only)
    CROSSCOMPILE-WITH-HOSTLIBS:    build with a bootstrapping-with-hostlibs compiled
                                   compiler, provided in `SWIFT_NATIVE_SWIFT_TOOLS_PATH`
]=] OFF)

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

set(SWIFT_TOOLS_INSTALL_DIR "${CMAKE_INSTALL_BINDIR}" CACHE PATH
  "Path for binary subdirectory to use during installation.
  Used by add_swift_tool_symlink in AddSwift.cmake so that llvm_install_symlink generates the installation script properly.")

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
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
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

option(SWIFT_RUN_TESTS_WITH_HOST_COMPILER
       "Run tests against the host compiler and not the just built swift"
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
set(SWIFT_ANDROID_DEPLOY_DEVICE_PATH "" CACHE STRING
  "Path on an Android device where build products will be pushed. These are used when running the test suite against the device")

#
# User-configurable Darwin-specific options.
#
option(SWIFT_EMBED_BITCODE_SECTION
    "If non-empty, embeds LLVM bitcode binary sections in the standard library and overlay binaries for supported platforms"
    FALSE)

option(SWIFT_EMBED_BITCODE_SECTION_HIDE_SYMBOLS
  "If non-empty, when embedding the LLVM bitcode binary sections into the relevant binaries, pass in -bitcode_hide_symbols. Does nothing if SWIFT_EMBED_BITCODE_SECTION is set to false."
  FALSE)

if("${SWIFT_HOST_VARIANT_SDK}" MATCHES "(OSX|IOS*|TVOS*|WATCHOS*)")
  set(SWIFT_RUNTIME_CRASH_REPORTER_CLIENT_default TRUE)
else()
  set(SWIFT_RUNTIME_CRASH_REPORTER_CLIENT_default FALSE)
endif()

option(SWIFT_RUNTIME_CRASH_REPORTER_CLIENT
    "Whether to enable CrashReporter integration"
    "${SWIFT_RUNTIME_CRASH_REPORTER_CLIENT_default}")

set(SWIFT_DARWIN_XCRUN_TOOLCHAIN "XcodeDefault" CACHE STRING
    "The name of the toolchain to pass to 'xcrun'")

set(SWIFT_DARWIN_STDLIB_INSTALL_NAME_DIR "/usr/lib/swift" CACHE STRING
    "The directory of the install_name for standard library dylibs")

# We don't want to use the same install_name_dir as the standard library which
# will be installed in /usr/lib/swift. These private libraries should continue
# to use @rpath for now.
set(SWIFT_DARWIN_STDLIB_PRIVATE_INSTALL_NAME_DIR "@rpath" CACHE STRING
    "The directory of the install_name for the private standard library dylibs")

set(SWIFT_DARWIN_DEPLOYMENT_VERSION_OSX "10.13" CACHE STRING
    "Minimum deployment target version for OS X")

set(SWIFT_DARWIN_DEPLOYMENT_VERSION_IOS "11.0" CACHE STRING
    "Minimum deployment target version for iOS")

set(SWIFT_DARWIN_DEPLOYMENT_VERSION_TVOS "11.0" CACHE STRING
    "Minimum deployment target version for tvOS")

set(SWIFT_DARWIN_DEPLOYMENT_VERSION_WATCHOS "4.0" CACHE STRING
    "Minimum deployment target version for watchOS")

#
#  Compatibility library deployment versions
#

set(COMPATIBILITY_MINIMUM_DEPLOYMENT_VERSION_OSX "10.9")
set(COMPATIBILITY_MINIMUM_DEPLOYMENT_VERSION_IOS "7.0")
set(COMPATIBILITY_MINIMUM_DEPLOYMENT_VERSION_TVOS "9.0")
set(COMPATIBILITY_MINIMUM_DEPLOYMENT_VERSION_WATCHOS "2.0")
set(COMPATIBILITY_MINIMUM_DEPLOYMENT_VERSION_MACCATALYST "13.1")

#
# User-configurable debugging options.
#

option(SWIFT_SIL_VERIFY_ALL
    "Run SIL verification after each transform when building Swift files in the build process"
    FALSE)

option(SWIFT_SIL_VERIFY_ALL_MACOS_ONLY
    "Run SIL verification after each transform when building the macOS stdlib"
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
    "Compile the Swift standard library with -sil-based-debuginfo to enable debugging and profiling on SIL level"
    FALSE)

option(SWIFT_CHECK_INCREMENTAL_COMPILATION
    "Check if incremental compilation works when compiling the Swift libraries"
    FALSE)

option(SWIFT_ENABLE_ARRAY_COW_CHECKS
    "Compile the stdlib with Array COW checks enabled (only relevant for assert builds)"
    FALSE)

option(SWIFT_REPORT_STATISTICS
    "Create json files which contain internal compilation statistics"
    FALSE)

# Only Darwin platforms enable ObjC interop by default.
if("${SWIFT_HOST_VARIANT_SDK}" MATCHES "(OSX|IOS*|TVOS*|WATCHOS*)")
  set(SWIFT_STDLIB_ENABLE_OBJC_INTEROP_default TRUE)
else()
  set(SWIFT_STDLIB_ENABLE_OBJC_INTEROP_default FALSE)
endif()

# Used by stdlib/toolchain as well, so this cannot be in stdlib/CMakeLists.txt
option(SWIFT_STDLIB_ENABLE_OBJC_INTEROP
       "Should stdlib be built with Obj-C interop."
       "${SWIFT_STDLIB_ENABLE_OBJC_INTEROP_default}")

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

option(SWIFT_ENABLE_RUNTIME_FUNCTION_COUNTERS
  "Enable runtime function counters and expose the API."
  FALSE)

option(SWIFT_ENABLE_STDLIBCORE_EXCLUSIVITY_CHECKING
  "Build stdlibCore with exclusivity checking enabled"
  FALSE)

option(SWIFT_STDLIB_ENABLE_DEBUG_PRECONDITIONS_IN_RELEASE
  "Enable _debugPrecondition checks in the stdlib in Release configurations"
  FALSE)

option(SWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING
  "Enable experimental Swift differentiable programming features"
  FALSE)

option(SWIFT_IMPLICIT_CONCURRENCY_IMPORT
  "Implicitly import the Swift concurrency module"
  TRUE)

option(SWIFT_IMPLICIT_BACKTRACING_IMPORT
  "Implicitly import the Swift backtracing module"
  FALSE)

option(SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY
  "Enable build of the Swift concurrency module"
  FALSE)

option(SWIFT_ENABLE_EXPERIMENTAL_CXX_INTEROP
  "Enable experimental C++ interop modules"
  FALSE)

option(SWIFT_ENABLE_CXX_INTEROP_SWIFT_BRIDGING_HEADER
  "Install the <swift/bridging> C++ interoperability header alongside compiler"
  TRUE)

option(SWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED
  "Enable experimental distributed actors and functions"
  FALSE)

option(SWIFT_ENABLE_EXPERIMENTAL_STRING_PROCESSING
  "Enable experimental string processing"
  FALSE)

option(SWIFT_ENABLE_EXPERIMENTAL_OBSERVATION
  "Enable build of the Swift observation module"
  FALSE)

option(SWIFT_ENABLE_DISPATCH
  "Enable use of libdispatch"
  TRUE)

option(SWIFT_ENABLE_GLOBAL_ISEL_ARM64
  "Enable global isel on arm64"
  FALSE)

cmake_dependent_option(SWIFT_BUILD_SOURCEKIT
  "Build SourceKit" TRUE
  "SWIFT_ENABLE_DISPATCH" FALSE)
cmake_dependent_option(SWIFT_ENABLE_SOURCEKIT_TESTS
  "Enable running SourceKit tests" TRUE
  "SWIFT_BUILD_SOURCEKIT" FALSE)

option(SWIFT_THREADING_PACKAGE
  "Override the threading package used for the build.  This can either be a
   single package name, or a semicolon separated sequence of sdk:package pairs.
   Valid package names are 'pthreads', 'darwin', 'linux', 'win32', 'c11', 'none'
   or the empty string for the SDK default.")

option(SWIFT_THREADING_HAS_DLSYM
  "Enable the use of the dlsym() function.  This gets used to provide TSan
   support on some platforms."
  TRUE)

option(SWIFT_ENABLE_MACCATALYST
  "Build the Standard Library and overlays with MacCatalyst support"
  FALSE)

option(SWIFT_ENABLE_BACKTRACING
  "Build backtracing runtime support"
  FALSE)

set(SWIFT_DARWIN_DEPLOYMENT_VERSION_MACCATALYST "14.5" CACHE STRING
  "Minimum deployment target version for macCatalyst")

#
# End of user-configurable options.
#

set(SWIFT_BUILT_STANDALONE FALSE)
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
  set(SWIFT_BUILT_STANDALONE TRUE)
endif()

if(SWIFT_BUILT_STANDALONE)
  project(Swift C CXX ${SWIFT_ASM_DIALECT})
endif()

if(MSVC OR "${CMAKE_SIMULATE_ID}" STREQUAL "MSVC")
  include(ClangClCompileRules)
elseif(UNIX)
  include(UnixCompileRules)
endif()

if(CMAKE_C_COMPILER_ID MATCHES Clang)
  add_compile_options($<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:-Werror=gnu>)
endif()

# Make some warnings errors as they are commonly occurring and flood the build
# with unnecessary noise.
if(CMAKE_C_COMPILER_ID MATCHES Clang)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Werror=c++98-compat-extra-semi>)
endif()

# Make sure we know where swift-syntax is because we need it to build the parser.
if(NOT EXISTS "${SWIFT_PATH_TO_SWIFT_SYNTAX_SOURCE}")
  message(SEND_ERROR "swift-syntax is required to build the Swift compiler. Please run update-checkout or specify SWIFT_PATH_TO_SWIFT_SYNTAX_SOURCE")
endif()

set(SWIFT_BUILD_HOST_DISPATCH FALSE)
if(SWIFT_ENABLE_DISPATCH AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  # Only build libdispatch for the host if the host tools are being built and
  # specifically if these two libraries that depend on it are built.
  if(SWIFT_INCLUDE_TOOLS AND SWIFT_BUILD_SOURCEKIT)
    set(SWIFT_BUILD_HOST_DISPATCH TRUE)
  endif()

  if(SWIFT_BUILD_HOST_DISPATCH)
    if(NOT EXISTS "${SWIFT_PATH_TO_LIBDISPATCH_SOURCE}")
      message(SEND_ERROR "SourceKit requires libdispatch on non-Darwin hosts.  Please specify SWIFT_PATH_TO_LIBDISPATCH_SOURCE")
    endif()
  endif()
endif()

file(STRINGS "utils/availability-macros.def" SWIFT_STDLIB_AVAILABILITY_DEFINITIONS)
list(FILTER SWIFT_STDLIB_AVAILABILITY_DEFINITIONS EXCLUDE REGEX "^\\s*(#.*)?$")

#
# Include CMake modules
#

include(CheckCXXSourceRuns)
include(CMakeParseArguments)
include(CMakePushCheckState)

# Print out path and version of any installed commands
message(STATUS "CMake (${CMAKE_COMMAND}) Version: ${CMAKE_VERSION}")
if(XCODE)
  set(version_flag -version)
else()
  set(version_flag --version)
endif()
execute_process(COMMAND ${CMAKE_MAKE_PROGRAM} ${version_flag}
  OUTPUT_VARIABLE _CMAKE_MAKE_PROGRAM_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE)
message(STATUS "CMake Make Program (${CMAKE_MAKE_PROGRAM}) Version: ${_CMAKE_MAKE_PROGRAM_VERSION}")
message(STATUS "C Compiler (${CMAKE_C_COMPILER}) Version: ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "C++ Compiler (${CMAKE_CXX_COMPILER}) Version: ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Assembler (${CMAKE_${SWIFT_ASM_DIALECT}_COMPILER}) Version: ${CMAKE_${SWIFT_ASM_DIALECT}_COMPILER_VERSION}")
if (CMAKE_Swift_COMPILER)
  message(STATUS "Swift Compiler (${CMAKE_Swift_COMPILER}) Version: ${CMAKE_Swift_COMPILER_VERSION}")

  # Check if the current Swift compiler has implicit _StringProcessing module.
  swift_supports_implicit_module("string-processing"
    SWIFT_SUPPORTS_DISABLE_IMPLICIT_STRING_PROCESSING_MODULE_IMPORT)
  message(STATUS "  Implicit 'string-processing' import: ${SWIFT_SUPPORTS_DISABLE_IMPLICIT_STRING_PROCESSING_MODULE_IMPORT}")

  # Same for _Backtracing.
  swift_supports_implicit_module("backtracing"
    SWIFT_SUPPORTS_DISABLE_IMPLICIT_BACKTRACING_MODULE_IMPORT)
  message(STATUS "  Implicit 'backtracing' import: ${SWIFT_SUPPORTS_DISABLE_IMPLICIT_BACKTRACING_MODULE_IMPORT}")
else()
  message(STATUS "Swift Compiler (None).")
endif()
if(SWIFT_PATH_TO_CMARK_BUILD)
  execute_process(COMMAND ${SWIFT_PATH_TO_CMARK_BUILD}/src/cmark --version
    OUTPUT_VARIABLE _CMARK_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  message(STATUS "CMark Version: ${_CMARK_VERSION}")
endif()
message(STATUS "")

# Check if a prebuilt clang path was passed in, as this variable will be
# assigned if not, in SwiftSharedCMakeConfig.
if("${SWIFT_NATIVE_CLANG_TOOLS_PATH}" STREQUAL "")
  set(SWIFT_PREBUILT_CLANG FALSE)
else()
  set(SWIFT_PREBUILT_CLANG TRUE)
endif()

# Also mark if we have a prebuilt swift before we do anything.
if("${SWIFT_NATIVE_SWIFT_TOOLS_PATH}" STREQUAL "")
  set(SWIFT_PREBUILT_SWIFT FALSE)
else()
  set(SWIFT_PREBUILT_SWIFT TRUE)
endif()

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
include(AddSwift)
include(SwiftConfigureSDK)
include(SwiftComponents)
include(SwiftList)
include(AddPureSwift)

# Configure swift include, install, build components.
swift_configure_components()

# lipo is used to create universal binaries.
include(SwiftToolchainUtils)
if(NOT SWIFT_LIPO)
  find_toolchain_tool(SWIFT_LIPO "${SWIFT_DARWIN_XCRUN_TOOLCHAIN}" lipo)
endif()

get_filename_component(SWIFT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR} REALPATH)
set(SWIFT_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(SWIFT_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
set(SWIFT_MAIN_INCLUDE_DIR "${SWIFT_SOURCE_DIR}/include")
set(SWIFT_SHIMS_INCLUDE_DIR "${SWIFT_SOURCE_DIR}/stdlib/public/SwiftShims")
set(SWIFT_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")

set(SWIFT_RUNTIME_OUTPUT_INTDIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/bin")
set(SWIFT_LIBRARY_OUTPUT_INTDIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/lib")
if("${SWIFT_NATIVE_SWIFT_TOOLS_PATH}" STREQUAL "")
  # This is the normal case. We are not cross-compiling.
  set(SWIFT_NATIVE_SWIFT_TOOLS_PATH "${SWIFT_RUNTIME_OUTPUT_INTDIR}")
  set(SWIFT_EXEC_FOR_SWIFT_MODULES "${CMAKE_Swift_COMPILER}")
elseif(BOOTSTRAPPING_MODE MATCHES "BOOTSTRAPPING.*")
  # If cross-compiling, we don't have to bootstrap. We can just use the previously
  # built native swiftc to build the swift compiler modules.
  message(STATUS "Building swift modules with previously built tools instead of bootstrapping")
  set(SWIFT_EXEC_FOR_SWIFT_MODULES "${SWIFT_NATIVE_SWIFT_TOOLS_PATH}/swiftc")
  if(BOOTSTRAPPING_MODE STREQUAL "BOOTSTRAPPING-WITH-HOSTLIBS")
    set(BOOTSTRAPPING_MODE "CROSSCOMPILE-WITH-HOSTLIBS")
  elseif(BOOTSTRAPPING_MODE STREQUAL "BOOTSTRAPPING")
    set(BOOTSTRAPPING_MODE "CROSSCOMPILE")
  else()
    set(BOOTSTRAPPING_MODE "HOSTTOOLS")
  endif()
elseif(BOOTSTRAPPING_MODE STREQUAL "HOSTTOOLS" OR SWIFT_SWIFT_PARSER)
  # We are building using a pre-installed host toolchain but not bootstrapping
  # the Swift modules. This happens when building using 'build-tooling-libs'
  # where we haven't built a new Swift compiler. Use the Swift compiler from the
  # pre-installed host toolchain to build the Swift modules.
  set(SWIFT_EXEC_FOR_SWIFT_MODULES "${CMAKE_Swift_COMPILER}")
endif()

if(BOOTSTRAPPING_MODE MATCHES "HOSTTOOLS|.*-WITH-HOSTLIBS")
  if(SWIFT_ENABLE_ARRAY_COW_CHECKS)
    message(STATUS "array COW checks disabled when building the swift modules with host libraries")
    set(SWIFT_ENABLE_ARRAY_COW_CHECKS FALSE)
  endif()
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
  ${SWIFT_SHIMS_INCLUDE_DIR}
  )

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

# Which default linker to use. Prefer LLVM_USE_LINKER if it set, otherwise use
# our own defaults. This should only be possible in a unified (not stand alone)
# build environment.
if(LLVM_USE_LINKER)
  set(SWIFT_USE_LINKER_default "${LLVM_USE_LINKER}")
elseif(SWIFT_HOST_VARIANT_SDK STREQUAL "ANDROID")
  set(SWIFT_USE_LINKER_default "lld")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(SWIFT_USE_LINKER_default "lld")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(SWIFT_USE_LINKER_default "")
else()
  set(SWIFT_USE_LINKER_default "gold")
endif()
set(SWIFT_USE_LINKER ${SWIFT_USE_LINKER_default} CACHE STRING
    "Build Swift with a non-default linker")

#
# Enable additional warnings.
#
swift_common_cxx_warnings()

# Check if we're build with MSVC or Clang-cl, as these compilers have similar command line arguments.
if("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC" OR "${CMAKE_CXX_SIMULATE_ID}" STREQUAL "MSVC")
  set(SWIFT_COMPILER_IS_MSVC_LIKE TRUE)
endif()

#
# Display a message if the threading package has been overridden
#

if(SWIFT_THREADING_PACKAGE)
  message(STATUS "")
  message(STATUS "Threading package override enabled")
  foreach(elt ${SWIFT_THREADING_PACKAGE})
    string(REPLACE ":" ";" elt_list "${elt}")
    list(LENGTH elt_list elt_list_len)
    if(elt_list_len EQUAL 1)
      set(elt_sdk "Global")
      list(GET elt_list 0 elt_package)
    elseif(elt_list_len EQUAL 2)
      list(GET elt_list 0 elt_sdk)
      list(GET elt_list 1 elt_package)
      string(TOUPPER "${elt_sdk}" elt_sdk)
    else()
      message(FATAL_ERROR "Bad threading override \"${elt}\" - SWIFT_THREADING_PACKAGE must be a semicolon separated list of package or sdk:package pairs.")
    endif()
    string(TOLOWER "${elt_package}" elt_package)
    message(STATUS "  ${elt_sdk}: ${elt_package}")
  endforeach()
  message(STATUS "")
endif()

#
# Configure SDKs.
#

if(XCODE)
  # FIXME: It used to be the case that Xcode would force
  # -m${platform}-version-min flags that would conflict with those computed
  # by build-script. version-min flags are deprecated in favor of -target since
  # clang-11, so we might be able to undo this.
  set(SWIFT_SDKS "OSX")
endif()

# When we have the early SwiftSyntax build, we can include its parser.
if(SWIFT_PATH_TO_EARLYSWIFTSYNTAX_BUILD_DIR)
  set(SWIFT_PATH_TO_EARLYSWIFTSYNTAX_TARGETS
    ${SWIFT_PATH_TO_EARLYSWIFTSYNTAX_BUILD_DIR}/cmake/SwiftSyntaxTargets.cmake)
  if(NOT EXISTS "${SWIFT_PATH_TO_EARLYSWIFTSYNTAX_TARGETS}")
    message(STATUS "Skipping Swift Swift parser integration due to missing early SwiftSyntax")
  else()
    set(SWIFT_SWIFT_PARSER TRUE)
    include(${SWIFT_PATH_TO_EARLYSWIFTSYNTAX_TARGETS})
  endif()
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

  is_sdk_requested(FREESTANDING swift_build_freestanding)
  if(swift_build_freestanding AND (SWIFT_FREESTANDING_FLAVOR STREQUAL "linux"))
    # TODO
    # configure_sdk_unix("FREESTANDING" "${SWIFT_HOST_VARIANT_ARCH}")
  endif()

elseif("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "FREEBSD")

  set(SWIFT_HOST_VARIANT "freebsd" CACHE STRING
      "Deployment OS for Swift host tools (the compiler) [freebsd].")

  configure_sdk_unix("FreeBSD" "${SWIFT_HOST_VARIANT_ARCH}")
  set(SWIFT_PRIMARY_VARIANT_SDK_default  "${SWIFT_HOST_VARIANT_SDK}")
  set(SWIFT_PRIMARY_VARIANT_ARCH_default "${SWIFT_HOST_VARIANT_ARCH}")

elseif("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "OPENBSD")

  set(SWIFT_HOST_VARIANT "openbsd" CACHE STRING
      "Deployment OS for Swift host tools (the compiler) [openbsd].")

  configure_sdk_unix("OpenBSD" "${SWIFT_HOST_VARIANT_ARCH}")
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

elseif("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "WASI")
  set(SWIFT_HOST_VARIANT "wasi" CACHE STRING
      "Deployment OS for Swift host tools (the compiler) [wasi]")

  configure_sdk_unix("WASI" "wasm32")
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
  set(SWIFT_PRIMARY_VARIANT_ARCH_default "${CMAKE_HOST_SYSTEM_PROCESSOR}")

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

# Should we cross-compile the standard library for WASI?
is_sdk_requested(WASI swift_build_wasm)
if(swift_build_wasm AND NOT "${SWIFT_HOST_VARIANT_SDK}" STREQUAL "WASI")
  configure_sdk_unix(WASI wasm32)
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
  message(STATUS "  Bootstrapping:  ${BOOTSTRAPPING_MODE}")
  message(STATUS "  Swift parser:   ${SWIFT_SWIFT_PARSER}")
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

  message(STATUS "Differentiable Programming Support: ${SWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING}")
  message(STATUS "Concurrency Support:                ${SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY}")
  message(STATUS "Distributed Support:                ${SWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED}")
  message(STATUS "String Processing Support:          ${SWIFT_ENABLE_EXPERIMENTAL_STRING_PROCESSING}")
  message(STATUS "Backtracing Support:                ${SWIFT_ENABLE_BACKTRACING}")
  message(STATUS "Unicode Support:                    ${SWIFT_STDLIB_ENABLE_UNICODE_DATA}")
  message(STATUS "Observation Support:                ${SWIFT_ENABLE_EXPERIMENTAL_OBSERVATION}")
  message(STATUS "")
else()
  message(STATUS "Not building Swift standard library, SDK overlays, and runtime")
  message(STATUS "")
endif()

if(SWIFT_BUILD_LIBEXEC)
  message(STATUS "Building Swift auxiliary executables for SDKs: ${SWIFT_SDKS}")
  message(STATUS "")
endif()

if(SWIFT_BUILD_REMOTE_MIRROR)
  message(STATUS "Building Swift Remote Mirror for SDKs: ${SWIFT_SDKS}")
  message(STATUS "")
endif()

#
# Find required dependencies.
#

find_package(Python3 3.6 COMPONENTS Interpreter REQUIRED)

#
# Find optional dependencies.
#

if(LLVM_ENABLE_LIBXML2)
  find_package(LibXml2 REQUIRED)
else()
  find_package(LibXml2)
endif()

if(LLVM_ENABLE_LIBEDIT)
  find_package(LibEdit REQUIRED)
else()
  find_package(LibEdit)
endif()

if(LibEdit_FOUND)
  cmake_push_check_state()
  list(APPEND CMAKE_REQUIRED_INCLUDES ${LibEdit_INCLUDE_DIRS})
  list(APPEND CMAKE_REQUIRED_LIBRARIES ${LibEdit_LIBRARIES})
  check_symbol_exists(el_wgets "histedit.h" HAVE_EL_WGETS)
  if(HAVE_EL_WGETS)
    set(LibEdit_HAS_UNICODE YES)
  else()
    set(LibEdit_HAS_UNICODE NO)
  endif()
  cmake_pop_check_state()
endif()

check_symbol_exists(wait4 "sys/wait.h" HAVE_WAIT4)

check_symbol_exists(proc_pid_rusage "libproc.h" HAVE_PROC_PID_RUSAGE)
if(HAVE_PROC_PID_RUSAGE)
    list(APPEND CMAKE_REQUIRED_LIBRARIES proc)
endif()

if (LLVM_ENABLE_DOXYGEN)
  message(STATUS "Doxygen: enabled")
endif()

if(SWIFT_ENABLE_DISPATCH)
  include(Libdispatch)
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
# https://github.com/apple/swift/issues/48534
if(SWIFT_BUILD_STDLIB)
  add_subdirectory(stdlib)
else()
  set(SWIFT_STDLIB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/stdlib")
  # Some of the things below depend on the threading library
  add_subdirectory(stdlib/public/Threading)

  if(SWIFT_BUILD_STDLIB_EXTRA_TOOLCHAIN_CONTENT)
    add_subdirectory(stdlib/toolchain)

    if(SWIFT_BUILD_STDLIB_CXX_MODULE)
      add_subdirectory(stdlib/public/Cxx)
    endif()
  endif()

  if (BUILD_SWIFT_CONCURRENCY_BACK_DEPLOYMENT_LIBRARIES)
    # Build the back-deployed concurrency library.
    add_subdirectory(stdlib/public/BackDeployConcurrency)
  endif()

  # Some tools (e.g. swift-reflection-dump) rely on a host swiftRemoteInspection,
  # so ensure we build that when building tools.
  if(SWIFT_INCLUDE_TOOLS OR SWIFT_BUILD_STDLIB_CXX_MODULE)
    add_subdirectory(stdlib/public/SwiftShims/swift/shims)
  endif()

  # We might want to build Remote Mirror separately
  if(SWIFT_BUILD_REMOTE_MIRROR)
    add_subdirectory(stdlib/public/LLVMSupport)
    add_subdirectory(stdlib/public/Demangling)
    add_subdirectory(stdlib/public/RemoteInspection)
    add_subdirectory(stdlib/public/SwiftRemoteMirror)
  endif()

  # We might also want to build the things in libexec seaprately
  if(SWIFT_BUILD_LIBEXEC)
    add_subdirectory(stdlib/public/libexec)
  endif()
endif()

if(SWIFT_INCLUDE_APINOTES)
  add_subdirectory(apinotes)
endif()

add_subdirectory(include)

if(SWIFT_INCLUDE_TOOLS)
  add_subdirectory(lib)

  # SwiftCompilerSources must come before "tools".
  # It adds swift module names to the global property "swift_compiler_modules"
  # which is used in add_swift_host_tool for the lldb workaround.
  add_subdirectory(SwiftCompilerSources)

  # Always include this after including stdlib/!
  # Refer to the large comment above the add_subdirectory(stdlib) call.
  # https://github.com/apple/swift/issues/48534
  add_subdirectory(tools)

  # Localization targets are configured in a way that assume the swift
  # frontend is being built, so trying to include them for other builds
  # (like stdlib) fail!
  #
  # Diagnostics information is only useful for the frontend compiler
  # anyway, so let's only include it if the compiler is being built,
  # which at the moment seems like if SWIFT_INCLUDE_TOOLS is defined.
  add_subdirectory(localization)
endif()

add_subdirectory(utils)

add_subdirectory(userdocs)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin")
  if(SWIFT_BUILD_PERF_TESTSUITE)
    add_subdirectory(benchmark)
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
