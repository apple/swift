
include(SwiftUtils)

function(swift_windows_arch_spelling arch var)
  if(arch STREQUAL "i686")
    set(${var} x86 PARENT_SCOPE)
  elseif(arch STREQUAL "x86_64")
    set(${var} x64 PARENT_SCOPE)
  elseif(arch STREQUAL "armv7")
    set(${var} arm PARENT_SCOPE)
  elseif(arch STREQUAL "aarch64")
    set(${var} arm64 PARENT_SCOPE)
  else()
    message(FATAL_ERROR "do not know MSVC spelling for ARCH: `${arch}`")
  endif()
endfunction()

function(swift_windows_include_for_arch arch var)
  set(paths
      "${VCToolsInstallDir}/include"
      "${UniversalCRTSdkDir}/Include/${UCRTVersion}/ucrt"
      "${UniversalCRTSdkDir}/Include/${UCRTVersion}/shared"
      "${UniversalCRTSdkDir}/Include/${UCRTVersion}/um")
  set(${var} ${paths} PARENT_SCOPE)
endfunction()

function(swift_windows_lib_for_arch arch var)
  swift_windows_arch_spelling(${arch} ARCH)

  set(paths)

  # NOTE(compnerd) provide compatibility with VS2015 which had the libraries in
  # a directory called "Lib" rather than VS2017 which normalizes the layout and
  # places them in a directory named "lib".
  if(IS_DIRECTORY "${VCToolsInstallDir}/Lib")
    if(ARCH STREQUAL "x86")
      list(APPEND paths "${VCToolsInstallDir}/Lib/")
    else()
      list(APPEND paths "${VCToolsInstallDir}/Lib/${ARCH}")
    endif()
  else()
    list(APPEND paths "${VCToolsInstallDir}/lib/${ARCH}")
  endif()

  list(APPEND paths
          "${UniversalCRTSdkDir}/Lib/${UCRTVersion}/ucrt/${ARCH}"
          "${UniversalCRTSdkDir}/Lib/${UCRTVersion}/um/${ARCH}")

  set(${var} ${paths} PARENT_SCOPE)
endfunction()

function(swift_windows_get_sdk_vfs_overlay overlay)
  get_filename_component(VCToolsInstallDir ${VCToolsInstallDir} ABSOLUTE)
  get_filename_component(UniversalCRTSdkDir ${UniversalCRTSdkDir} ABSOLUTE)
  set(UCRTVersion ${UCRTVersion})

  # TODO(compnerd) use a target to avoid re-creating this file all the time
  configure_file("${SWIFT_SOURCE_DIR}/utils/WindowsSDKVFSOverlay.yaml.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/windows-sdk-vfs-overlay.yaml"
                 @ONLY)
  set(${overlay} ${CMAKE_CURRENT_BINARY_DIR}/windows-sdk-vfs-overlay.yaml
    PARENT_SCOPE)
endfunction()

function(swift_verify_windows_VCVAR var)
  if (NOT DEFINED "${var}" AND NOT DEFINED "ENV{${var}}")
    message(FATAL_ERROR "${var} environment variable must be set")
  endif()
endfunction()

function(swift_windows_cache_VCVARS)
  swift_verify_windows_VCVAR(VCToolsInstallDir)
  swift_verify_windows_VCVAR(UniversalCRTSdkDir)
  swift_verify_windows_VCVAR(UCRTVersion)

  set(VCToolsInstallDir $ENV{VCToolsInstallDir} CACHE STRING "")
  set(UniversalCRTSdkDir $ENV{UniversalCRTSdkDir} CACHE STRING "")
  set(UCRTVersion $ENV{UCRTVersion} CACHE STRING "")
endfunction()

# NOTE(compnerd) we use a macro here as this modifies global variables
macro(swift_swap_compiler_if_needed target)
  if(NOT CMAKE_C_COMPILER_ID MATCHES Clang)
    if(CMAKE_SYSTEM_NAME STREQUAL CMAKE_HOST_SYSTEM_NAME AND CMAKE_SYSTEM_PROCESSOR STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
      if(SWIFT_BUILT_STANDALONE)
        get_target_property(SWIFT_CLANG_LOCATION clang LOCATION)
        get_filename_component(SWIFT_CLANG_LOCATION ${SWIFT_CLANG_LOCATION} DIRECTORY)
      else()
        set(SWIFT_CLANG_LOCATION ${LLVM_RUNTIME_OUTPUT_INTDIR})
      endif()
    endif()

    if (DEFINED SWIFT_CLANG_LOCATION)
      if("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC" OR "${CMAKE_C_SIMULATE_ID}" STREQUAL "MSVC")
        set(CMAKE_C_COMPILER ${SWIFT_CLANG_LOCATION}/clang-cl${CMAKE_EXECUTABLE_SUFFIX})
        set(CMAKE_CXX_COMPILER ${SWIFT_CLANG_LOCATION}/clang-cl${CMAKE_EXECUTABLE_SUFFIX})
        set(CMAKE_C_SIMULATE_ID MSVC)
        set(CMAKE_C_COMPILER_FRONTEND_VARIANT MSVC)
        set(CMAKE_CXX_SIMULATE_ID MSVC)
        set(CMAKE_CXX_COMPILER_FRONTEND_VARIANT MSVC)
      else()
        set(CMAKE_C_COMPILER ${SWIFT_CLANG_LOCATION}/clang${CMAKE_EXECUTABLE_SUFFIX})
        set(CMAKE_CXX_COMPILER ${SWIFT_CLANG_LOCATION}/clang++${CMAKE_EXECUTABLE_SUFFIX})
      endif()
      set(CMAKE_C_COMPILER_ID Clang)
      set(CMAKE_CXX_COMPILER_ID Clang)
      message(STATUS "C/C++ compiler for ${target} is set to: ${CMAKE_C_COMPILER}")
    else()
      message(SEND_ERROR "${target} requires a clang based compiler. Please set SWIFT_CLANG_LOCATION.")
    endif()
  endif()
endmacro()

