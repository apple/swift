# In some configurations (e.g. back deploy concurrency) we
# configure the build from the root of the Swift repo but we skip
# stdlib/CMakeLists.txt, with the risk of missing important parameters.
# To account for this scenario, we include the stdlib options
# before the guard
include(${CMAKE_CURRENT_LIST_DIR}/../../stdlib/cmake/modules/StdlibOptions.cmake)

# CMAKE_SOURCE_DIR is the directory that cmake got initially invoked on.
# CMAKE_CURRENT_SOURCE_DIR is the current directory. If these are equal, it's
# a top-level build of the CMAKE_SOURCE_DIR. Otherwise, define a guard variable
# and return.
if(DEFINED SWIFT_MASTER_LOADED
    OR NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(SWIFT_MASTER_LOADED TRUE)
  return()
endif()


list(APPEND CMAKE_MODULE_PATH
  "${SWIFT_SOURCE_ROOT}/llvm-project/llvm/cmake/modules"
  "${PROJECT_SOURCE_DIR}/../../../../cmake/modules"
  "${PROJECT_SOURCE_DIR}/../../../cmake/modules")


# -----------------------------------------------------------------------------
# Preconditions

include(SwiftUtils)

precondition(CMAKE_INSTALL_PREFIX)
precondition(SWIFT_DEST_ROOT)
precondition(SWIFT_HOST_VARIANT_SDK)
precondition(SWIFT_SOURCE_ROOT)
precondition(TOOLCHAIN_DIR)


# -----------------------------------------------------------------------------
# Cache Variables and Options

set(SWIFT_SOURCE_DIR "${SWIFT_SOURCE_ROOT}/swift" CACHE PATH
  "Path to the directory containing the Swift sources.")

set(SWIFT_DARWIN_XCRUN_TOOLCHAIN "XcodeDefault" CACHE STRING
  "The name of the toolchain to pass to 'xcrun'.")

set(SWIFT_DARWIN_DEPLOYMENT_VERSION_OSX "11.4" CACHE STRING
    "Minimum deployment target version for macOS.")
set(SWIFT_DARWIN_DEPLOYMENT_VERSION_IOS "14.3" CACHE STRING
    "Minimum deployment target version for iOS.")
set(SWIFT_DARWIN_DEPLOYMENT_VERSION_TVOS "14.3" CACHE STRING
    "Minimum deployment target version for tvOS.")
set(SWIFT_DARWIN_DEPLOYMENT_VERSION_WATCHOS "7.2" CACHE STRING
    "Minimum deployment target version for watchOS.")
set(SWIFT_DARWIN_DEPLOYMENT_VERSION_MACCATALYST "13.0" CACHE STRING
  "Minimum deployment target version for macCatalyst")

set(SWIFT_INSTALL_COMPONENTS "sdk-overlay" CACHE STRING
  "A semicolon-separated list of install components.")

set(SWIFT_SDKS "${SWIFT_HOST_VARIANT_SDK}" CACHE STRING
  "List of Swift SDKs to build.")

set(SWIFT_NATIVE_LLVM_TOOLS_PATH "${TOOLCHAIN_DIR}/usr/bin" CACHE STRING
  "Path to LLVM tools that are executable on the build machine.")
set(SWIFT_NATIVE_CLANG_TOOLS_PATH "${TOOLCHAIN_DIR}/usr/bin" CACHE STRING
  "Path to Clang tools that are executable on the build machine.")
set(SWIFT_NATIVE_SWIFT_TOOLS_PATH "${TOOLCHAIN_DIR}/usr/bin" CACHE STRING
  "Path to Swift tools that are executable on the build machine.")

# NOTE: The initialization in stdlib/CMakeLists.txt will be bypassed if we
# directly invoke CMake for this directory, so we initialize the variables
# related to library evolution here as well.

option(SWIFT_STDLIB_STABLE_ABI
  "Should stdlib be built with stable ABI (library evolution, resilience)."
  TRUE)

option(SWIFT_ENABLE_MODULE_INTERFACES
  "Generate .swiftinterface files alongside .swiftmodule files."
  "${SWIFT_STDLIB_STABLE_ABI}")

set(SWIFT_STDLIB_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE STRING
  "Build type for the Swift standard library and SDK overlays.")

file(STRINGS "../../utils/availability-macros.def" SWIFT_STDLIB_AVAILABILITY_DEFINITIONS)
list(FILTER SWIFT_STDLIB_AVAILABILITY_DEFINITIONS EXCLUDE REGEX "^\\s*(#.*)?$")

set(SWIFT_DARWIN_SUPPORTED_ARCHS "" CACHE STRING
  "Semicolon-separated list of architectures to configure on Darwin platforms. \
If left empty all default architectures are configured.")

set(SWIFT_DARWIN_MODULE_ARCHS "" CACHE STRING
  "Semicolon-separated list of architectures to configure Swift module-only \
targets on Darwin platforms. These targets are in addition to the full \
library targets.")

# -----------------------------------------------------------------------------
# Constants

set(CMAKE_INSTALL_PREFIX
  "${SWIFT_DEST_ROOT}${TOOLCHAIN_DIR}/usr")


set(SWIFT_DARWIN_PLATFORMS
  OSX IOS IOS_SIMULATOR TVOS TVOS_SIMULATOR WATCHOS WATCHOS_SIMULATOR)

# Flags used to indicate we are building a standalone overlay.
# FIXME: We should cut this down to a single flag.
set(BUILD_STANDALONE TRUE)
set(SWIFT_BUILD_STANDALONE_OVERLAY TRUE)

set(SWIFT_STDLIB_LIBRARY_BUILD_TYPES "SHARED")
set(SWIFT_SDK_OVERLAY_LIBRARY_BUILD_TYPES "SHARED")

option(SWIFT_ENABLE_MACCATALYST
  "Build the overlays with macCatalyst support"
  FALSE)

# -----------------------------------------------------------------------------

include(SwiftToolchainUtils)
if(NOT SWIFT_LIPO)
  find_toolchain_tool(SWIFT_LIPO "${SWIFT_DARWIN_XCRUN_TOOLCHAIN}" lipo)
endif()

include(AddLLVM)
include(SwiftSharedCMakeConfig)
include(AddSwift)
include(SwiftHandleGybSources)
include(SwiftConfigureSDK)
include(SwiftComponents)
include(DarwinSDKs)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Without this line, installing components is broken. This needs refactoring.
swift_configure_components()


list_subtract(
  "${SWIFT_SDKS}"
  "${SWIFT_CONFIGURED_SDKS}"
  unknown_sdks)

precondition(unknown_sdks NEGATE
  MESSAGE
    "Unknown SDKs: ${unknown_sdks}")


# Some overlays include the runtime's headers, and some of those headers are
# generated at build time.
add_subdirectory("${SWIFT_SOURCE_DIR}/include" "${SWIFT_SOURCE_DIR}/include")
add_subdirectory("${SWIFT_SOURCE_DIR}/apinotes" "${SWIFT_SOURCE_DIR}/apinotes")
