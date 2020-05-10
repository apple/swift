# Variable that tracks the set of configured SDKs.
#
# Each element in this list is an SDK for which the various
# SWIFT_SDK_${name}_* variables are defined. Swift libraries will be
# built for each variant.
set(SWIFT_CONFIGURED_SDKS)

include(SwiftWindowsSupport)
include(SwiftAndroidSupport)

# Report the given SDK to the user.
function(_report_sdk prefix)
  message(STATUS "${SWIFT_SDK_${prefix}_NAME} SDK:")
  message(STATUS "  Object File Format: ${SWIFT_SDK_${prefix}_OBJECT_FORMAT}")
  message(STATUS "  Swift Standard Library Path: ${SWIFT_SDK_${prefix}_LIB_SUBDIR}")

  if("${prefix}" STREQUAL "WINDOWS")
    message(STATUS "  UCRT Version: ${UCRTVersion}")
    message(STATUS "  UCRT SDK Path: ${UniversalCRTSdkDir}")
    message(STATUS "  VC Path: ${VCToolsInstallDir}")
    if("${CMAKE_BUILD_TYPE}" STREQUAL "DEBUG")
      message(STATUS "  ${CMAKE_BUILD_TYPE} VC++ CRT: MDd")
    else()
      message(STATUS "  ${CMAKE_BUILD_TYPE} VC++ CRT: MD")
    endif()
  endif()
  if(prefix IN_LIST SWIFT_APPLE_PLATFORMS)
    message(STATUS "  Version: ${SWIFT_SDK_${prefix}_VERSION}")
    message(STATUS "  Build number: ${SWIFT_SDK_${prefix}_BUILD_NUMBER}")
    message(STATUS "  Deployment version: ${SWIFT_SDK_${prefix}_DEPLOYMENT_VERSION}")
    message(STATUS "  Version min name: ${SWIFT_SDK_${prefix}_VERSION_MIN_NAME}")
    message(STATUS "  Triple name: ${SWIFT_SDK_${prefix}_TRIPLE_NAME}")
    message(STATUS "  Simulator: ${SWIFT_SDK_${prefix}_IS_SIMULATOR}")
  endif()
  if(SWIFT_SDK_${prefix}_MODULE_ARCHITECTURES)
    message(STATUS "  Module Architectures: ${SWIFT_SDK_${prefix}_MODULE_ARCHITECTURES}")
  endif()

  message(STATUS "  Architectures: ${SWIFT_SDK_${prefix}_ARCHITECTURES}")
  foreach(arch ${SWIFT_SDK_${prefix}_ARCHITECTURES})
    message(STATUS "  ${arch} triple: ${SWIFT_SDK_${prefix}_ARCH_${arch}_TRIPLE}")
    message(STATUS "  Module triple: ${SWIFT_SDK_${prefix}_ARCH_${arch}_MODULE}")
  endforeach()
  if("${prefix}" STREQUAL "WINDOWS")
    foreach(arch ${SWIFT_SDK_${prefix}_ARCHITECTURES})
      swift_windows_include_for_arch(${arch} ${arch}_INCLUDE)
      swift_windows_lib_for_arch(${arch} ${arch}_LIB)
      message(STATUS "  ${arch} INCLUDE: ${${arch}_INCLUDE}")
      message(STATUS "  ${arch} LIB: ${${arch}_LIB}")
    endforeach()
  elseif("${prefix}" STREQUAL "ANDROID")
    if(NOT "${SWIFT_ANDROID_NDK_PATH}" STREQUAL "")
      message(STATUS " NDK: $ENV{SWIFT_ANDROID_NDK_PATH}")
    endif()
    if(NOT "${SWIFT_ANDROID_NATIVE_SYSROOT}" STREQUAL "")
      message(STATUS " Sysroot: ${SWIFT_ANDROID_NATIVE_SYSROOT}")
    endif()
    foreach(arch ${SWIFT_SDK_${prefix}_ARCHITECTURES})
      swift_android_include_for_arch(${arch} ${arch}_INCLUDE)
      swift_android_lib_for_arch(${arch} ${arch}_LIB)
      message(STATUS "  ${arch} INCLUDE: ${${arch}_INCLUDE}")
      message(STATUS "  ${arch} LIB: ${${arch}_LIB}")
    endforeach()
  else()
    foreach(arch ${SWIFT_SDK_${prefix}_ARCHITECTURES})
      message(STATUS "  ${arch} Path: ${SWIFT_SDK_${prefix}_ARCH_${arch}_PATH}")
    endforeach()
  endif()

  if(NOT prefix IN_LIST SWIFT_APPLE_PLATFORMS)
    foreach(arch ${SWIFT_SDK_${prefix}_ARCHITECTURES})
      message(STATUS "  ${arch} libc header path: ${SWIFT_SDK_${prefix}_ARCH_${arch}_LIBC_INCLUDE_DIRECTORY}")
      message(STATUS "  ${arch} libc architecture specific header path: ${SWIFT_SDK_${prefix}_ARCH_${arch}_LIBC_ARCHITECTURE_INCLUDE_DIRECTORY}")
    endforeach()
    if(SWIFT_BUILD_STDLIB)
      foreach(arch ${SWIFT_SDK_${prefix}_ARCHITECTURES})
        message(STATUS "  ${arch} ICU i18n INCLUDE: ${SWIFT_${prefix}_${arch}_ICU_I18N_INCLUDE}")
        message(STATUS "  ${arch} ICU i18n LIB: ${SWIFT_${prefix}_${arch}_ICU_I18N}")
        message(STATUS "  ${arch} ICU unicode INCLUDE: ${SWIFT_${prefix}_${arch}_ICU_UC_INCLUDE}")
        message(STATUS "  ${arch} ICU unicode LIB: ${SWIFT_${prefix}_${arch}_ICU_UC}")
      endforeach()
    endif()
  endif()

  message(STATUS "")
endfunction()

# Configure an SDK
#
# Usage:
#   configure_sdk_darwin(
#     prefix             # Prefix to use for SDK variables (e.g., OSX)
#     name               # Display name for this SDK
#     deployment_version # Deployment version
#     xcrun_name         # SDK name to use with xcrun
#     version_min_name   # The name used in the -mOS-version-min flag
#     triple_name        # The name used in Swift's -triple
#     architectures      # A list of architectures this SDK supports
#   )
#
# Sadly there are three OS naming conventions.
# xcrun SDK name:   macosx iphoneos iphonesimulator (+ version)
# -mOS-version-min: macosx ios      ios-simulator
# swift -triple:    macosx ios      ios
#
# This macro attempts to configure a given SDK. When successful, it
# defines a number of variables:
#
#   SWIFT_SDK_${prefix}_NAME                Display name for the SDK
#   SWIFT_SDK_${prefix}_VERSION             SDK version number (e.g., 10.9, 7.0)
#   SWIFT_SDK_${prefix}_BUILD_NUMBER        SDK build number (e.g., 14A389a)
#   SWIFT_SDK_${prefix}_DEPLOYMENT_VERSION  Deployment version (e.g., 10.9, 7.0)
#   SWIFT_SDK_${prefix}_LIB_SUBDIR          Library subdir for this SDK
#   SWIFT_SDK_${prefix}_VERSION_MIN_NAME    Version min name for this SDK
#   SWIFT_SDK_${prefix}_TRIPLE_NAME         Triple name for this SDK
#   SWIFT_SDK_${prefix}_ARCHITECTURES       Architectures (as a list)
#   SWIFT_SDK_${prefix}_IS_SIMULATOR        Whether this is a simulator target.
#   SWIFT_SDK_${prefix}_ARCH_${ARCH}_TRIPLE Triple name
#   SWIFT_SDK_${prefix}_ARCH_${ARCH}_MODULE Module triple name for this SDK
macro(configure_sdk_darwin
    prefix name deployment_version xcrun_name
    version_min_name triple_name module_name architectures)
  # Note: this has to be implemented as a macro because it sets global
  # variables.

  # Find the SDK
  set(SWIFT_SDK_${prefix}_PATH "" CACHE PATH "Path to the ${name} SDK")

  if(NOT SWIFT_SDK_${prefix}_PATH)
    execute_process(
        COMMAND "xcrun" "--sdk" "${xcrun_name}" "--show-sdk-path"
        OUTPUT_VARIABLE SWIFT_SDK_${prefix}_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()

  if(NOT EXISTS "${SWIFT_SDK_${prefix}_PATH}/SDKSettings.plist")
    message(FATAL_ERROR "${name} SDK not found at SWIFT_SDK_${prefix}_PATH.")
  endif()

  # Determine the SDK version we found.
  execute_process(
    COMMAND "defaults" "read" "${SWIFT_SDK_${prefix}_PATH}/SDKSettings.plist" "Version"
      OUTPUT_VARIABLE SWIFT_SDK_${prefix}_VERSION
      OUTPUT_STRIP_TRAILING_WHITESPACE)

  execute_process(
    COMMAND "xcodebuild" "-sdk" "${SWIFT_SDK_${prefix}_PATH}" "-version" "ProductBuildVersion"
      OUTPUT_VARIABLE SWIFT_SDK_${prefix}_BUILD_NUMBER
      OUTPUT_STRIP_TRAILING_WHITESPACE)

  # Set other variables.
  set(SWIFT_SDK_${prefix}_NAME "${name}")
  set(SWIFT_SDK_${prefix}_DEPLOYMENT_VERSION "${deployment_version}")
  set(SWIFT_SDK_${prefix}_LIB_SUBDIR "${xcrun_name}")
  set(SWIFT_SDK_${prefix}_VERSION_MIN_NAME "${version_min_name}")
  set(SWIFT_SDK_${prefix}_TRIPLE_NAME "${triple_name}")
  set(SWIFT_SDK_${prefix}_OBJECT_FORMAT "MACHO")

  set(SWIFT_SDK_${prefix}_ARCHITECTURES ${architectures})
  if(SWIFT_DARWIN_SUPPORTED_ARCHS)
    list_intersect(
      "${architectures}"                  # lhs
      "${SWIFT_DARWIN_SUPPORTED_ARCHS}"   # rhs
      SWIFT_SDK_${prefix}_ARCHITECTURES)  # result
  endif()

  list_intersect(
    "${SWIFT_DARWIN_MODULE_ARCHS}"            # lhs
    "${architectures}"                        # rhs
    SWIFT_SDK_${prefix}_MODULE_ARCHITECTURES) # result

  # Ensure the architectures and module-only architectures lists are mutually
  # exclusive.
  list_subtract(
    "${SWIFT_SDK_${prefix}_MODULE_ARCHITECTURES}" # lhs
    "${SWIFT_SDK_${prefix}_ARCHITECTURES}"        # rhs
    SWIFT_SDK_${prefix}_MODULE_ARCHITECTURES)     # result

  # Determine whether this is a simulator SDK.
  if ( ${xcrun_name} MATCHES "simulator" )
    set(SWIFT_SDK_${prefix}_IS_SIMULATOR TRUE)
  else()
    set(SWIFT_SDK_${prefix}_IS_SIMULATOR FALSE)
  endif()

  # Configure variables for _all_ architectures even if we aren't "building"
  # them because they aren't supported.
  foreach(arch ${architectures})
    # On Darwin, all archs share the same SDK path.
    set(SWIFT_SDK_${prefix}_ARCH_${arch}_PATH "${SWIFT_SDK_${prefix}_PATH}")

    set(SWIFT_SDK_${prefix}_ARCH_${arch}_TRIPLE
        "${arch}-apple-${SWIFT_SDK_${prefix}_TRIPLE_NAME}")

    set(SWIFT_SDK_${prefix}_ARCH_${arch}_MODULE
        "${arch}-apple-${module_name}")

    # If this is a simulator target, append -simulator.
    if (SWIFT_SDK_${prefix}_IS_SIMULATOR)
      set(SWIFT_SDK_${prefix}_ARCH_${arch}_TRIPLE
          "${SWIFT_SDK_${prefix}_ARCH_${arch}_TRIPLE}-simulator")
    endif ()

    if(SWIFT_ENABLE_MACCATALYST AND "${prefix}" STREQUAL "OSX")
      # For macCatalyst append the '-macabi' environment to the target triple.
      set(SWIFT_SDK_MACCATALYST_ARCH_${arch}_TRIPLE "${arch}-apple-ios-macabi")
      set(SWIFT_SDK_MACCATALYST_ARCH_${arch}_MODULE "${arch}-apple-ios-macabi")

      # For macCatalyst, the xcrun_name is "macosx" since it uses that sdk.
      # Hard code the library subdirectory to "maccatalyst" in that case.
      set(SWIFT_SDK_MACCATALYST_LIB_SUBDIR "maccatalyst")
    endif()
  endforeach()

  # Add this to the list of known SDKs.
  list(APPEND SWIFT_CONFIGURED_SDKS "${prefix}")

  _report_sdk("${prefix}")
endmacro()

macro(configure_sdk_unix name architectures)
  # Note: this has to be implemented as a macro because it sets global
  # variables.

  string(TOUPPER ${name} prefix)
  string(TOLOWER ${name} platform)

  set(SWIFT_SDK_${prefix}_NAME "${name}")
  set(SWIFT_SDK_${prefix}_LIB_SUBDIR "${platform}")
  set(SWIFT_SDK_${prefix}_ARCHITECTURES "${architectures}")
  if("${prefix}" STREQUAL "CYGWIN")
    set(SWIFT_SDK_${prefix}_OBJECT_FORMAT "COFF")
  elseif("${prefix}" STREQUAL "WASI")
    set(SWIFT_SDK_${prefix}_OBJECT_FORMAT "WASM")
  else()
    set(SWIFT_SDK_${prefix}_OBJECT_FORMAT "ELF")
  endif()

  foreach(arch ${architectures})
    if("${prefix}" STREQUAL "ANDROID")
      if(NOT "${SWIFT_ANDROID_NDK_PATH}" STREQUAL "")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_LIBC_INCLUDE_DIRECTORY "${SWIFT_ANDROID_NDK_PATH}/sysroot/usr/include" CACHE STRING "Path to C library headers")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_LIBC_ARCHITECTURE_INCLUDE_DIRECTORY "${SWIFT_ANDROID_NDK_PATH}/sysroot/usr/include" CACHE STRING "Path to C library architecture headers")
      elseif(NOT "${SWIFT_ANDROID_NATIVE_SYSROOT}" STREQUAL "")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_LIBC_INCLUDE_DIRECTORY "${SWIFT_ANDROID_NATIVE_SYSROOT}/usr/include" CACHE STRING "Path to C library headers")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_LIBC_ARCHITECTURE_INCLUDE_DIRECTORY "${SWIFT_ANDROID_NATIVE_SYSROOT}/usr/include" CACHE STRING "Path to C library architecture headers")
      else()
        message(SEND_ERROR "Couldn't find LIBC_INCLUDE_DIRECTORY for Android")
      endif()

      if("${arch}" STREQUAL "armv7")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_NDK_TRIPLE "arm-linux-androideabi")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_ALT_SPELLING "arm")
        if(NOT "${SWIFT_ANDROID_NDK_PATH}" STREQUAL "")
          set(SWIFT_SDK_ANDROID_ARCH_${arch}_PATH "${SWIFT_ANDROID_NDK_PATH}/platforms/android-${SWIFT_ANDROID_API_LEVEL}/arch-arm")
        elseif(NOT "${SWIFT_ANDROID_NATIVE_SYSROOT}" STREQUAL "")
          set(SWIFT_SDK_ANDROID_ARCH_${arch}_PATH "${SWIFT_ANDROID_NATIVE_SYSROOT}")
        else()
          message(SEND_ERROR "Couldn't find SWIFT_SDK_ANDROID_ARCH_armv7_PATH")
        endif()
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_TRIPLE "armv7-none-linux-androideabi")
        # The Android ABI isn't part of the module triple.
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_MODULE "armv7-none-linux-android")
      elseif("${arch}" STREQUAL "aarch64")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_NDK_TRIPLE "aarch64-linux-android")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_ALT_SPELLING "aarch64")
        if(NOT "${SWIFT_ANDROID_NDK_PATH}" STREQUAL "")
          set(SWIFT_SDK_ANDROID_ARCH_${arch}_PATH "${SWIFT_ANDROID_NDK_PATH}/platforms/android-${SWIFT_ANDROID_API_LEVEL}/arch-arm64")
        elseif(NOT "${SWIFT_ANDROID_NATIVE_SYSROOT}" STREQUAL "")
          set(SWIFT_SDK_ANDROID_ARCH_${arch}_PATH "${SWIFT_ANDROID_NATIVE_SYSROOT}")
        else()
          message(SEND_ERROR "Couldn't find SWIFT_SDK_ANDROID_ARCH_aarch64_PATH")
        endif()
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_TRIPLE "aarch64-unknown-linux-android")
      elseif("${arch}" STREQUAL "i686")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_NDK_TRIPLE "i686-linux-android")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_ALT_SPELLING "i686")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_PATH "${SWIFT_ANDROID_NDK_PATH}/platforms/android-${SWIFT_ANDROID_API_LEVEL}/arch-x86")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_TRIPLE "i686-unknown-linux-android")
      elseif("${arch}" STREQUAL "x86_64")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_NDK_TRIPLE "x86_64-linux-android")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_ALT_SPELLING "x86_64")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_PATH "${SWIFT_ANDROID_NDK_PATH}/platforms/android-${SWIFT_ANDROID_API_LEVEL}/arch-x86_64")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_TRIPLE "x86_64-unknown-linux-android")
      else()
        message(FATAL_ERROR "unknown arch for android SDK: ${arch}")
      endif()

      # Get the prebuilt suffix to create the correct toolchain path when using the NDK
      if(CMAKE_HOST_SYSTEM_NAME STREQUAL Darwin)
        set(_swift_android_prebuilt_build darwin-x86_64)
      elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL Linux)
        set(_swift_android_prebuilt_build linux-x86_64)
      elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL Windows)
        set(_swift_android_prebuilt_build Windows-x86_64)
      elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL Android)
        # When building natively on an Android host, there's no NDK or prebuilt suffix.
      else()
        message(SEND_ERROR "cannot cross-compile to android from ${CMAKE_HOST_SYSTEM_NAME}")
      endif()
      if("${arch}" STREQUAL "i686")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_NDK_PREBUILT_PATH
          "${SWIFT_ANDROID_NDK_PATH}/toolchains/x86-${SWIFT_ANDROID_NDK_GCC_VERSION}/prebuilt/${_swift_android_prebuilt_build}")
      elseif("${arch}" STREQUAL "x86_64")
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_NDK_PREBUILT_PATH
          "${SWIFT_ANDROID_NDK_PATH}/toolchains/x86_64-${SWIFT_ANDROID_NDK_GCC_VERSION}/prebuilt/${_swift_android_prebuilt_build}")
      else()
        set(SWIFT_SDK_ANDROID_ARCH_${arch}_NDK_PREBUILT_PATH
          "${SWIFT_ANDROID_NDK_PATH}/toolchains/${SWIFT_SDK_ANDROID_ARCH_${arch}_NDK_TRIPLE}-${SWIFT_ANDROID_NDK_GCC_VERSION}/prebuilt/${_swift_android_prebuilt_build}")
      endif()
    else()
      set(SWIFT_SDK_${prefix}_ARCH_${arch}_PATH "/" CACHE STRING "CMAKE_SYSROOT for ${prefix} ${arch}")

      if("${prefix}" STREQUAL "HAIKU")
        set(SWIFT_SDK_HAIKU_ARCH_${arch}_LIBC_INCLUDE_DIRECTORY "/system/develop/headers/posix" CACHE STRING "Path to C library headers")
        set(SWIFT_SDK_HAIKU_ARCH_${arch}_LIBC_ARCHITECTURE_INCLUDE_DIRECTORY "/system/develop/headers" CACHE STRING "Path to C library architecture headers")
      else()
        set(SWIFT_SDK_${prefix}_ARCH_${arch}_LIBC_INCLUDE_DIRECTORY "/usr/include" CACHE STRING "Path to C library headers")
        set(SWIFT_SDK_${prefix}_ARCH_${arch}_LIBC_ARCHITECTURE_INCLUDE_DIRECTORY "${SWIFT_SDK_${prefix}_ARCH_${arch}_LIBC_INCLUDE_DIRECTORY}/${CMAKE_LIBRARY_ARCHITECTURE}" CACHE STRING "Path to C library architecture headers")
      endif()

      if("${prefix}" STREQUAL "LINUX")
        if(arch MATCHES "(armv6|armv7)")
          set(SWIFT_SDK_LINUX_ARCH_${arch}_TRIPLE "${arch}-unknown-linux-gnueabihf")
        elseif(arch MATCHES "(aarch64|i686|powerpc64|powerpc64le|s390x|x86_64)")
          set(SWIFT_SDK_LINUX_ARCH_${arch}_TRIPLE "${arch}-unknown-linux-gnu")
        else()
          message(FATAL_ERROR "unknown arch for ${prefix}: ${arch}")
        endif()
      elseif("${prefix}" STREQUAL "FREEBSD")
        if(NOT arch STREQUAL x86_64)
          message(FATAL_ERROR "unsupported arch for FreeBSD: ${arch}")
        endif()

        if(CMAKE_HOST_SYSTEM_NAME NOT STREQUAL FreeBSD)
          message(WARNING "CMAKE_SYSTEM_VERSION will not match target")
        endif()

        string(REPLACE "[-].*" "" freebsd_system_version ${CMAKE_SYSTEM_VERSION})
        message(STATUS "FreeBSD Version: ${freebsd_system_version}")

        set(SWIFT_SDK_FREEBSD_ARCH_x86_64_TRIPLE "x86_64-unknown-freebsd${freebsd_system_version}")
      elseif("${prefix}" STREQUAL "OPENBSD")
        if(NOT arch STREQUAL amd64)
          message(FATAL_ERROR "unsupported arch for OpenBSD: ${arch}")
        endif()

        string(REPLACE "[-].*" "" openbsd_system_version ${CMAKE_SYSTEM_VERSION})
        message(STATUS "OpenBSD Version: ${openbsd_system_version}")

        set(SWIFT_SDK_OPENBSD_ARCH_amd64_TRIPLE "amd64-unknown-openbsd${openbsd_system_version}")
      elseif("${prefix}" STREQUAL "CYGWIN")
        if(NOT arch STREQUAL x86_64)
          message(FATAL_ERROR "unsupported arch for cygwin: ${arch}")
        endif()
        set(SWIFT_SDK_CYGWIN_ARCH_x86_64_TRIPLE "x86_64-unknown-windows-cygnus")
      elseif("${prefix}" STREQUAL "HAIKU")
        if(NOT arch STREQUAL x86_64)
          message(FATAL_ERROR "unsupported arch for Haiku: ${arch}")
        endif()
        set(SWIFT_SDK_HAIKU_ARCH_x86_64_TRIPLE "x86_64-unknown-haiku")
      elseif("${prefix}" STREQUAL "WASI")
        if(NOT arch STREQUAL wasm32)
          message(FATAL_ERROR "unsupported arch for WASI: ${arch}")
        endif()
        set(SWIFT_SDK_WASI_ARCH_wasm32_PATH "${SWIFT_WASI_SYSROOT}")
        set(SWIFT_SDK_WASI_ARCH_wasm32_TRIPLE "wasm32-unknown-wasi")
        set(SWIFT_SDK_WASI_ARCH_wasm32_LIBC_INCLUDE_DIRECTORY "${SWIFT_WASI_SYSROOT}/include")
        set(SWIFT_SDK_WASI_ARCH_wasm32_LIBC_ARCHITECTURE_INCLUDE_DIRECTORY "${SWIFT_WASI_SYSROOT}/include")
      else()
        message(FATAL_ERROR "unknown Unix OS: ${prefix}")
      endif()
    endif()

    # If the module triple wasn't set explicitly, it's the same as the triple.
    if(NOT SWIFT_SDK_${prefix}_ARCH_${arch}_MODULE)
      set(SWIFT_SDK_${prefix}_ARCH_${arch}_MODULE "${SWIFT_SDK_${prefix}_ARCH_${arch}_MODULE}")
    endif()
  endforeach()

  # Add this to the list of known SDKs.
  list(APPEND SWIFT_CONFIGURED_SDKS "${prefix}")

  _report_sdk("${prefix}")
endmacro()

macro(configure_sdk_windows name environment architectures)
  # Note: this has to be implemented as a macro because it sets global
  # variables.

  swift_windows_cache_VCVARS()

  string(TOUPPER ${name} prefix)
  string(TOLOWER ${name} platform)

  set(SWIFT_SDK_${prefix}_NAME "${name}")
  set(SWIFT_SDK_${prefix}_LIB_SUBDIR "windows")
  set(SWIFT_SDK_${prefix}_ARCHITECTURES "${architectures}")
  set(SWIFT_SDK_${prefix}_OBJECT_FORMAT "COFF")

  foreach(arch ${architectures})
    if(arch STREQUAL armv7)
      set(SWIFT_SDK_${prefix}_ARCH_${arch}_TRIPLE
          "thumbv7-unknown-windows-${environment}")
    else()
      set(SWIFT_SDK_${prefix}_ARCH_${arch}_TRIPLE
          "${arch}-unknown-windows-${environment}")
    endif()

    set(SWIFT_SDK_${prefix}_ARCH_${arch}_MODULE "${SWIFT_SDK_${prefix}_ARCH_${arch}_MODULE}")

    # NOTE: set the path to / to avoid a spurious `--sysroot` from being passed
    # to the driver -- rely on the `INCLUDE` AND `LIB` environment variables
    # instead.
    set(SWIFT_SDK_${prefix}_ARCH_${arch}_PATH "/")

    # NOTE(compnerd) workaround incorrectly extensioned import libraries from
    # the Windows SDK on case sensitive file systems.
    swift_windows_arch_spelling(${arch} WinSDKArchitecture)
    set(WinSDK${arch}UMDir "${UniversalCRTSdkDir}/Lib/${UCRTVersion}/um/${WinSDKArchitecture}")
    set(OverlayDirectory "${CMAKE_BINARY_DIR}/winsdk_lib_${arch}_symlinks")

    if(NOT EXISTS "${UniversalCRTSdkDir}/Include/${UCRTVersion}/um/WINDOWS.H")
      file(MAKE_DIRECTORY ${OverlayDirectory})

      file(GLOB libraries RELATIVE "${WinSDK${arch}UMDir}" "${WinSDK${arch}UMDir}/*")
      foreach(library ${libraries})
        get_filename_component(name_we "${library}" NAME_WE)
        get_filename_component(ext "${library}" EXT)
        string(TOLOWER "${ext}" lowercase_ext)
        set(lowercase_ext_symlink_name "${name_we}${lowercase_ext}")
        if(NOT library STREQUAL lowercase_ext_symlink_name)
          execute_process(COMMAND
                          "${CMAKE_COMMAND}" -E create_symlink "${WinSDK${arch}UMDir}/${library}" "${OverlayDirectory}/${lowercase_ext_symlink_name}")
        endif()
      endforeach()
    endif()
  endforeach()

  # Add this to the list of known SDKs.
  list(APPEND SWIFT_CONFIGURED_SDKS "${prefix}")

  _report_sdk("${prefix}")
endmacro()

# Configure a variant of a certain SDK
#
# In addition to the SDK and architecture, a variant determines build settings.
#
# FIXME: this is not wired up with anything yet.
function(configure_target_variant prefix name sdk build_config lib_subdir)
  set(SWIFT_VARIANT_${prefix}_NAME               ${name})
  set(SWIFT_VARIANT_${prefix}_SDK_PATH           ${SWIFT_SDK_${sdk}_PATH})
  set(SWIFT_VARIANT_${prefix}_VERSION            ${SWIFT_SDK_${sdk}_VERSION})
  set(SWIFT_VARIANT_${prefix}_BUILD_NUMBER       ${SWIFT_SDK_${sdk}_BUILD_NUMBER})
  set(SWIFT_VARIANT_${prefix}_DEPLOYMENT_VERSION ${SWIFT_SDK_${sdk}_DEPLOYMENT_VERSION})
  set(SWIFT_VARIANT_${prefix}_LIB_SUBDIR         "${lib_subdir}/${SWIFT_SDK_${sdk}_LIB_SUBDIR}")
  set(SWIFT_VARIANT_${prefix}_VERSION_MIN_NAME   ${SWIFT_SDK_${sdk}_VERSION_MIN_NAME})
  set(SWIFT_VARIANT_${prefix}_TRIPLE_NAME        ${SWIFT_SDK_${sdk}_TRIPLE_NAME})
  set(SWIFT_VARIANT_${prefix}_ARCHITECTURES      ${SWIFT_SDK_${sdk}_ARCHITECTURES})
endfunction()

