include(macCatalystUtils)
include(SwiftList)
include(SwiftXcodeSupport)
include(SwiftWindowsSupport)
include(SwiftAndroidSupport)

function(_swift_gyb_target_sources target scope)
  file(GLOB GYB_UNICODE_DATA ${SWIFT_SOURCE_DIR}/utils/UnicodeData/*)
  file(GLOB GYB_STDLIB_SUPPORT ${SWIFT_SOURCE_DIR}/utils/gyb_stdlib_support.py)
  file(GLOB GYB_SYNTAX_SUPPORT ${SWIFT_SOURCE_DIR}/utils/gyb_syntax_support/*)
  file(GLOB GYB_SOURCEKIT_SUPPORT ${SWIFT_SOURCE_DIR}/utils/gyb_sourcekit_support/*)
  set(GYB_SOURCES
    ${SWIFT_SOURCE_DIR}/utils/GYBUnicodeDataUtils.py
    ${SWIFT_SOURCE_DIR}/utils/SwiftIntTypes.py
    ${GYB_UNICODE_DATA}
    ${GYB_STDLIB_SUPPORT}
    ${GYB_SYNTAX_SUPPORT}
    ${GYB_SOURCEKIT_SUPPORT})

  foreach(source ${ARGN})
    get_filename_component(generated ${source} NAME_WLE)
    get_filename_component(absolute ${source} REALPATH)

    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${generated}
      COMMAND
        $<TARGET_FILE:Python3::Interpreter> ${SWIFT_SOURCE_DIR}/utils/gyb -D CMAKE_SIZEOF_VOID_P=${CMAKE_SIZEOF_VOID_P} ${SWIFT_GYB_FLAGS} -o ${CMAKE_CURRENT_BINARY_DIR}/${generated}.tmp ${absolute}
      COMMAND
        ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_BINARY_DIR}/${generated}.tmp ${CMAKE_CURRENT_BINARY_DIR}/${generated}
      COMMAND
        ${CMAKE_COMMAND} -E remove ${CMAKE_CURRENT_BINARY_DIR}/${generated}.tmp
      DEPENDS
        ${GYB_SOURCES}
        ${absolute})
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/${generated} PROPERTIES
      GENERATED TRUE)
    target_sources(${target} ${scope}
      ${CMAKE_CURRENT_BINARY_DIR}/${generated})
  endforeach()
endfunction()

# SWIFTLIB_DIR is the directory in the build tree where Swift resource files
# should be placed.  Note that $CMAKE_CFG_INTDIR expands to "." for
# single-configuration builds.
set(SWIFTLIB_DIR
    "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/lib/swift")
set(SWIFTSTATICLIB_DIR
    "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/lib/swift_static")

function(_compute_lto_flag option out_var)
  string(TOLOWER "${option}" lowercase_option)
  if (lowercase_option STREQUAL "full")
    set(${out_var} "-flto=full" PARENT_SCOPE)
  elseif (lowercase_option STREQUAL "thin")
    set(${out_var} "-flto=thin" PARENT_SCOPE)
  endif()
endfunction()

function(_set_target_prefix_and_suffix target kind sdk)
  precondition(target MESSAGE "target is required")
  precondition(kind MESSAGE "kind is required")
  precondition(sdk MESSAGE "sdk is required")

  if(${sdk} STREQUAL ANDROID)
    if(${kind} STREQUAL STATIC)
      set_target_properties(${target} PROPERTIES PREFIX "lib" SUFFIX ".a")
    elseif(${kind} STREQUAL SHARED)
      set_target_properties(${target} PROPERTIES PREFIX "lib" SUFFIX ".so")
    endif()
  elseif(${sdk} STREQUAL WINDOWS)
    if(${kind} STREQUAL STATIC)
      set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".lib")
    elseif(${kind} STREQUAL SHARED)
      set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".dll")
    endif()
  endif()
endfunction()

# Usage:
# _add_host_variant_c_compile_link_flags(name)
function(_add_host_variant_c_compile_link_flags name)
  if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_APPLE_PLATFORMS)
    set(DEPLOYMENT_VERSION "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_DEPLOYMENT_VERSION}")
  endif()

  if(SWIFT_HOST_VARIANT_SDK STREQUAL ANDROID)
    set(DEPLOYMENT_VERSION ${SWIFT_ANDROID_API_LEVEL})
  endif()

  # MSVC, clang-cl, gcc don't understand -target.
  if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND NOT SWIFT_COMPILER_IS_MSVC_LIKE)
    get_target_triple(target target_variant "${SWIFT_HOST_VARIANT_SDK}" "${SWIFT_HOST_VARIANT_ARCH}"
      MACCATALYST_BUILD_FLAVOR ""
      DEPLOYMENT_VERSION "${DEPLOYMENT_VERSION}")
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-target;${target}>)
    target_link_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-target;${target}>)
  endif()

  if (CMAKE_Swift_COMPILER)
    get_target_triple(target target_variant "${SWIFT_HOST_VARIANT_SDK}" "${SWIFT_HOST_VARIANT_ARCH}"
      MACCATALYST_BUILD_FLAVOR ""
      DEPLOYMENT_VERSION "${DEPLOYMENT_VERSION}")
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:Swift>:-target;${target}>)
  endif()

  set(_sysroot
    "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_ARCH_${SWIFT_HOST_VARIANT_ARCH}_PATH}")
  if(SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_USE_ISYSROOT)
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-isysroot;${_sysroot}>)
  elseif(NOT SWIFT_COMPILER_IS_MSVC_LIKE AND NOT "${_sysroot}" STREQUAL "/")
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:--sysroot=${_sysroot}>)
  endif()

  if(SWIFT_HOST_VARIANT_SDK STREQUAL ANDROID)
    # lld can handle targeting the android build.  However, if lld is not
    # enabled, then fallback to the linker included in the android NDK.
    if(NOT SWIFT_USE_LINKER STREQUAL "lld")
      swift_android_tools_path(${SWIFT_HOST_VARIANT_ARCH} tools_path)
      target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-B${tools_path}>)
    endif()
  endif()

  if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_APPLE_PLATFORMS)
    # We collate -F with the framework path to avoid unwanted deduplication
    # of options by target_compile_options -- this way no undesired
    # side effects are introduced should a new search path be added.
    target_compile_options(${name} PRIVATE
      $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:
      -arch ${SWIFT_HOST_VARIANT_ARCH}
      "-F${SWIFT_SDK_${SWIFT_HOST_VARIANT_ARCH}_PATH}/../../../Developer/Library/Frameworks"
    >)
  endif()

  _compute_lto_flag("${SWIFT_TOOLS_ENABLE_LTO}" _lto_flag_out)
  if (_lto_flag_out)
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:${_lto_flag_out}>)
    target_link_options(${name} PRIVATE ${_lto_flag_out})
  endif()
endfunction()


function(_add_host_variant_c_compile_flags target)
  _add_host_variant_c_compile_link_flags(${target})

  is_build_type_optimized("${CMAKE_BUILD_TYPE}" optimized)
  if(optimized)
    if("${CMAKE_BUILD_TYPE}" STREQUAL "MinSizeRel")
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-Os>)
    else()
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-O2>)
    endif()

    # Omit leaf frame pointers on x86 production builds (optimized, no debug
    # info, and no asserts).
    is_build_type_with_debuginfo("${CMAKE_BUILD_TYPE}" debug)
    if(NOT debug AND NOT LLVM_ENABLE_ASSERTIONS)
      if(SWIFT_HOST_VARIANT_ARCH MATCHES "i?86")
        if(NOT SWIFT_COMPILER_IS_MSVC_LIKE)
          target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-momit-leaf-frame-pointer>)
        else()
          target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:/Oy>)
        endif()
      endif()
    endif()
  else()
    if(NOT SWIFT_COMPILER_IS_MSVC_LIKE)
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-O0>)
    else()
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:/Od>)
    endif()
  endif()

  # CMake automatically adds the flags for debug info if we use MSVC/clang-cl.
  if(NOT SWIFT_COMPILER_IS_MSVC_LIKE)
    is_build_type_with_debuginfo("${CMAKE_BUILD_TYPE}" debuginfo)
    if(debuginfo)
      _compute_lto_flag("${SWIFT_TOOLS_ENABLE_LTO}" _lto_flag_out)
      if(_lto_flag_out)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-gline-tables-only>)
      else()
        target_compile_options(${target} PRIVATE -g)
      endif()
    else()
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-g0>)
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:Swift>:-gnone>)
    endif()
  endif()

  if(SWIFT_HOST_VARIANT_SDK STREQUAL WINDOWS)
    # MSVC/clang-cl don't support -fno-pic or -fms-compatibility-version.
    if(NOT SWIFT_COMPILER_IS_MSVC_LIKE)
      target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-fms-compatibility-version=1900 -fno-pic>)
    endif()

    target_compile_definitions(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:LLVM_ON_WIN32 _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS>)
    if(NOT "${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
      target_compile_definitions(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:_CRT_USE_BUILTIN_OFFSETOF>)
    endif()
    # TODO(compnerd) permit building for different families
    target_compile_definitions(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:_CRT_USE_WINAPI_FAMILY_DESKTOP_APP>)
    if(SWIFT_HOST_VARIANT_ARCH MATCHES arm)
      target_compile_definitions(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE>)
    endif()
    target_compile_definitions(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:
      # TODO(compnerd) handle /MT
      _MD
      _DLL
      # NOTE: We assume that we are using VS 2015 U2+
      _ENABLE_ATOMIC_ALIGNMENT_FIX
      # NOTE: We use over-aligned values for the RefCount side-table
      # (see revision d913eefcc93f8c80d6d1a6de4ea898a2838d8b6f)
      # This is required to build with VS2017 15.8+
      _ENABLE_EXTENDED_ALIGNED_STORAGE=1>)

    # msvcprt's std::function requires RTTI, but we do not want RTTI data.
    # Emulate /GR-.
    # TODO(compnerd) when moving up to VS 2017 15.3 and newer, we can disable
    # RTTI again
    if(SWIFT_COMPILER_IS_MSVC_LIKE)
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:/GR->)
    else()
      target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-frtti>
        $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>"SHELL:-Xclang -fno-rtti-data">)
    endif()

    # NOTE: VS 2017 15.3 introduced this to disable the static components of
    # RTTI as well.  This requires a newer SDK though and we do not have
    # guarantees on the SDK version currently.
    target_compile_definitions(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:_HAS_STATIC_RTTI=0>)

    # NOTE(compnerd) workaround LLVM invoking `add_definitions(-D_DEBUG)` which
    # causes failures for the runtime library when cross-compiling due to
    # undefined symbols from the standard library.
    if(NOT CMAKE_BUILD_TYPE STREQUAL Debug)
      target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-U_DEBUG>)
    endif()
  endif()

  if(SWIFT_HOST_VARIANT_SDK STREQUAL ANDROID)
    if(SWIFT_HOST_VARIANT_ARCH STREQUAL x86_64)
      # NOTE(compnerd) Android NDK 21 or lower will generate library calls to
      # `__sync_val_compare_and_swap_16` rather than lowering to the CPU's
      # `cmpxchg16b` instruction as the `cx16` feature is disabled due to a bug
      # in Clang.  This is being fixed in the current master Clang and will
      # hopefully make it into Clang 9.0.  In the mean time, workaround this in
      # the build.
      if(CMAKE_C_COMPILER_ID MATCHES Clang AND CMAKE_C_COMPILER_VERSION
          VERSION_LESS 9.0.0)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-mcx16>)
      endif()
    endif()
  endif()

  if(LLVM_ENABLE_ASSERTIONS)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-UNDEBUG>)
  else()
    target_compile_definitions(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:NDEBUG>)
  endif()

  if(SWIFT_ENABLE_RUNTIME_FUNCTION_COUNTERS)
    target_compile_definitions(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:SWIFT_ENABLE_RUNTIME_FUNCTION_COUNTERS>)
  endif()

  if(SWIFT_ANALYZE_CODE_COVERAGE)
    target_compile_options(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-fprofile-instr-generate -fcoverage-mapping>)
  endif()

  if((SWIFT_HOST_VARIANT_ARCH STREQUAL armv7 OR
      SWIFT_HOST_VARIANT_ARCH STREQUAL aarch64) AND
     (SWIFT_HOST_VARIANT_SDK STREQUAL LINUX OR
      SWIFT_HOST_VARIANT_SDK STREQUAL ANDROID))
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-funwind-tables>)
  endif()

  if(SWIFT_HOST_VARIANT_SDK STREQUAL "LINUX")
    if(SWIFT_HOST_VARIANT_ARCH STREQUAL x86_64)
      # this is the minimum architecture that supports 16 byte CAS, which is
      # necessary to avoid a dependency to libatomic
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-march=core2>)
    endif()
  endif()

  # The LLVM backend is built with these defines on most 32-bit platforms,
  # llvm/llvm-project@66395c9, which can cause incompatibilities with the Swift
  # frontend if not built the same way.
  if("${SWIFT_HOST_VARIANT_ARCH}" MATCHES "armv6|armv7|i686" AND
     NOT (SWIFT_HOST_VARIANT_SDK STREQUAL ANDROID AND SWIFT_ANDROID_API_LEVEL LESS 24))
    target_compile_definitions(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:_LARGEFILE_SOURCE _FILE_OFFSET_BITS=64>)
  endif()
endfunction()

function(_add_host_variant_link_flags target)
  _add_host_variant_c_compile_link_flags(${target})

  if(SWIFT_HOST_VARIANT_SDK STREQUAL LINUX)
    target_link_libraries(${target} PRIVATE
      pthread
      dl)
    if("${SWIFT_HOST_VARIANT_ARCH}" MATCHES "armv6|armv7|i686")
      target_link_libraries(${target} PRIVATE atomic)
    endif()
  elseif(SWIFT_HOST_VARIANT_SDK STREQUAL FREEBSD)
    target_link_libraries(${target} PRIVATE
      pthread)
  elseif(SWIFT_HOST_VARIANT_SDK STREQUAL CYGWIN)
    # No extra libraries required.
  elseif(SWIFT_HOST_VARIANT_SDK STREQUAL WINDOWS)
    # We don't need to add -nostdlib using MSVC or clang-cl, as MSVC and
    # clang-cl rely on auto-linking entirely.
    if(NOT SWIFT_COMPILER_IS_MSVC_LIKE)
      # NOTE: we do not use "/MD" or "/MDd" and select the runtime via linker
      # options. This causes conflicts.
      target_link_options(${target} PRIVATE
        -nostdlib)
    endif()
    swift_windows_lib_for_arch(${SWIFT_HOST_VARIANT_ARCH}
      ${SWIFT_HOST_VARIANT_ARCH}_LIB)
    target_link_directories(${target} PRIVATE
      ${${SWIFT_HOST_VARIANT_ARCH}_LIB})

    # NOTE(compnerd) workaround incorrectly extensioned import libraries from
    # the Windows SDK on case sensitive file systems.
    target_link_directories(${target} PRIVATE
      ${CMAKE_BINARY_DIR}/winsdk_lib_${SWIFT_HOST_VARIANT_ARCH}_symlinks)
  elseif(SWIFT_HOST_VARIANT_SDK STREQUAL HAIKU)
    target_link_libraries(${target} PRIVATE
      bsd)
    target_link_options(${target} PRIVATE
      "SHELL:-Xlinker -Bsymbolic")
  elseif(SWIFT_HOST_VARIANT_SDK STREQUAL ANDROID)
    target_link_libraries(${target} PRIVATE
      dl
      log
      # We need to add the math library, which is linked implicitly by libc++
      m)

    # link against the custom C++ library
    swift_android_cxx_libraries_for_arch(${SWIFT_HOST_VARIANT_ARCH}
      cxx_link_libraries)
    target_link_libraries(${target} PRIVATE
      ${cxx_link_libraries})

    swift_android_libgcc_for_arch_cross_compile(${SWIFT_HOST_VARIANT_ARCH}
      ${SWIFT_HOST_VARIANT_ARCH}_LIB)
    target_link_directories(${target} PRIVATE
      ${${SWIFT_HOST_VARIANT_ARCH}_LIB})
  else()
    # If lto is enabled, we need to add the object path flag so that the LTO code
    # generator leaves the intermediate object file in a place where it will not
    # be touched. The reason why this must be done is that on OS X, debug info is
    # left in object files. So if the object file is removed when we go to
    # generate a dsym, the debug info is gone.
    if (SWIFT_TOOLS_ENABLE_LTO)
      target_link_options(${target} PRIVATE
        "SHELL:-Xlinker -object_path_lto"
        "SHELL:-Xlinker ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/${target}-${SWIFT_HOST_VARIANT_SDK}-${SWIFT_HOST_VARIANT_ARCH}-lto${CMAKE_C_OUTPUT_EXTENSION}")
    endif()
  endif()

  if(NOT SWIFT_COMPILER_IS_MSVC_LIKE)
    if(SWIFT_USE_LINKER)
      target_link_options(${target} PRIVATE
        $<$<LINK_LANGUAGE:CXX>:-fuse-ld=${SWIFT_USE_LINKER}$<$<STREQUAL:${CMAKE_HOST_SYSTEM_NAME},Windows>:.exe>>)
      if (CMAKE_Swift_COMPILER)
        target_link_options(${target} PRIVATE
          $<$<LINK_LANGUAGE:Swift>:-use-ld=${SWIFT_USE_LINKER}$<$<STREQUAL:${CMAKE_HOST_SYSTEM_NAME},Windows>:.exe>>)
      endif()
    endif()
  endif()

  # Enable dead stripping. Portions of this logic were copied from llvm's
  # `add_link_opts` function (which, perhaps, should have been used here in the
  # first place, but at this point it's hard to say whether that's feasible).
  #
  # TODO: Evaluate/enable -f{function,data}-sections --gc-sections for bfd,
  # gold, and lld.
  if(NOT CMAKE_BUILD_TYPE STREQUAL Debug AND CMAKE_SYSTEM_NAME MATCHES Darwin)
    if (NOT SWIFT_DISABLE_DEAD_STRIPPING)
      # See rdar://48283130: This gives 6MB+ size reductions for swift and
      # SourceKitService, and much larger size reductions for sil-opt etc.
      target_link_options(${target} PRIVATE
        "SHELL:-Xlinker -dead_strip")
    endif()
  endif()
endfunction()

# Add a new Swift host library.
#
# Usage:
#   add_swift_host_library(name
#     [SHARED]
#     [STATIC]
#     [OBJECT]
#     [PURE_SWIFT]
#     [LLVM_LINK_COMPONENTS comp1 ...]
#     source1 [source2 source3 ...])
#
# name
#   Name of the library (e.g., swiftParse).
#
# SHARED
#   Build a shared library.
#
# STATIC
#   Build a static library.
#
# OBJECT
#   Build an object library
#
# LLVM_LINK_COMPONENTS
#   LLVM components this library depends on.
#
# PURE_SWIFT
#   This has two effects if set: we do not use llvm_update_compile_flags to
#   generate cflags/etc and we leave the linking mode of the library as swift.
#
# source1 ...
#   Sources to add into this library.
function(add_swift_host_library name)
  set(options
        SHARED
        STATIC
        OBJECT
        PURE_SWIFT)
  set(single_parameter_options)
  set(multiple_parameter_options
        LLVM_LINK_COMPONENTS)

  cmake_parse_arguments(ASHL
                        "${options}"
                        "${single_parameter_options}"
                        "${multiple_parameter_options}"
                        ${ARGN})
  set(ASHL_SOURCES ${ASHL_UNPARSED_ARGUMENTS})

  translate_flags(ASHL "${options}")

  if(NOT ASHL_SHARED AND NOT ASHL_STATIC AND NOT ASHL_OBJECT)
    message(FATAL_ERROR "One of SHARED/STATIC/OBJECT must be specified")
  endif()

  if(XCODE)
    get_filename_component(base_dir ${CMAKE_CURRENT_SOURCE_DIR} NAME)
  
    file(GLOB_RECURSE ASHL_HEADERS
      ${SWIFT_SOURCE_DIR}/include/swift/${base_dir}/*.h
      ${SWIFT_SOURCE_DIR}/include/swift/${base_dir}/*.def
      ${CMAKE_CURRENT_SOURCE_DIR}/*.h
      ${CMAKE_CURRENT_SOURCE_DIR}/*.def)
    file(GLOB_RECURSE ASHL_TDS
      ${SWIFT_SOURCE_DIR}/include/swift${base_dir}/*.td)

    set_source_files_properties(${ASHL_HEADERS} ${ASHL_TDS} PROPERTIES
      HEADER_FILE_ONLY true)
    source_group("TableGen descriptions" FILES ${ASHL_TDS})

    set(ASHL_SOURCES ${ASHL_SOURCES} ${ASHL_HEADERS} ${ASHL_TDS})
  endif()

  if(ASHL_SHARED)
    set(libkind SHARED)
  elseif(ASHL_STATIC)
    set(libkind STATIC)
  elseif(ASHL_OBJECT)
    set(libkind OBJECT)
  endif()

  add_library(${name} ${libkind} ${ASHL_SOURCES})
  if (LLVM_COMMON_DEPENDS)
    add_dependencies(${name} ${LLVM_COMMON_DEPENDS})
  endif()
  if (NOT ASHL_PURE_SWIFT)
    llvm_update_compile_flags(${name})
  endif()
  swift_common_llvm_config(${name} ${ASHL_LLVM_LINK_COMPONENTS})
  set_output_directory(${name}
      BINARY_DIR ${SWIFT_RUNTIME_OUTPUT_INTDIR}
      LIBRARY_DIR ${SWIFT_LIBRARY_OUTPUT_INTDIR})

  if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_APPLE_PLATFORMS)
    set_target_properties(${name} PROPERTIES
      INSTALL_NAME_DIR "@rpath")
  elseif(SWIFT_HOST_VARIANT_SDK STREQUAL LINUX)
    set_target_properties(${name} PROPERTIES
      INSTALL_RPATH "$ORIGIN")
  elseif(SWIFT_HOST_VARIANT_SDK STREQUAL CYGWIN)
    set_target_properties(${name} PROPERTIES
      INSTALL_RPATH "$ORIGIN:/usr/lib/swift/cygwin")
  elseif(SWIFT_HOST_VARIANT_SDK STREQUAL "ANDROID")
    set_target_properties(${name} PROPERTIES
      INSTALL_RPATH "$ORIGIN")
  endif()

  set_target_properties(${name} PROPERTIES
    BUILD_WITH_INSTALL_RPATH YES
    FOLDER "Swift libraries")

  _add_host_variant_c_compile_flags(${name})
  _add_host_variant_link_flags(${name})
  _add_host_variant_c_compile_link_flags(${name})
  _set_target_prefix_and_suffix(${name} "${libkind}" "${SWIFT_HOST_VARIANT_SDK}")

  # Set compilation and link flags.
  if(SWIFT_HOST_VARIANT_SDK STREQUAL WINDOWS)
    swift_windows_include_for_arch(${SWIFT_HOST_VARIANT_ARCH}
      ${SWIFT_HOST_VARIANT_ARCH}_INCLUDE)
    target_include_directories(${name} SYSTEM PRIVATE
      ${${SWIFT_HOST_VARIANT_ARCH}_INCLUDE})

    if(libkind STREQUAL SHARED)
      target_compile_definitions(${name} PRIVATE
        _WINDLL)
    endif()

    if(NOT ${CMAKE_C_COMPILER_ID} STREQUAL MSVC)
      swift_windows_get_sdk_vfs_overlay(ASHL_VFS_OVERLAY)
      target_compile_options(${name} PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:"SHELL:-Xclang -ivfsoverlay -Xclang ${ASHL_VFS_OVERLAY}">)

      # MSVC doesn't support -Xclang. We don't need to manually specify
      # the dependent libraries as `cl` does so.
      target_compile_options(${name} PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:"SHELL:-Xclang --dependent-lib=oldnames">
        # TODO(compnerd) handle /MT, /MTd
        $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:"SHELL:-Xclang --dependent-lib=msvcrt$<$<CONFIG:Debug>:d>">
        )
    endif()

    set_target_properties(${name} PROPERTIES
      NO_SONAME YES)
  endif()

  # Always link as CXX even if we have swift content unless we only contain
  # swift content signaled via us being marked "PURE_SWIFT".
  if (NOT ASHL_PURE_SWIFT)
    set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)
  endif()

  if(${SWIFT_HOST_VARIANT_SDK} IN_LIST SWIFT_APPLE_PLATFORMS)
    target_link_options(${name} PRIVATE
      "LINKER:-compatibility_version,1")
    if(SWIFT_COMPILER_VERSION)
      target_link_options(${name} PRIVATE
        "LINKER:-current_version,${SWIFT_COMPILER_VERSION}")
    endif()

    # If we found a swift compiler and are going to use swift code in swift
    # host side tools but link with clang, add the appropriate -L paths so we
    # find all of the necessary swift libraries on Darwin.
    if (NOT ASHL_PURE_SWIFT)
      if (CMAKE_Swift_COMPILER)
        # Add in the SDK directory for the host platform and add an rpath.
        target_link_directories(${name} PRIVATE
          ${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_ARCH_${SWIFT_HOST_VARIANT_ARCH}_PATH}/usr/lib/swift)
        # Add in the toolchain directory so we can grab compatibility libraries
        get_filename_component(TOOLCHAIN_BIN_DIR ${CMAKE_Swift_COMPILER} DIRECTORY)
        get_filename_component(TOOLCHAIN_LIB_DIR "${TOOLCHAIN_BIN_DIR}/../lib/swift/macosx" ABSOLUTE)
        target_link_directories(${name} PUBLIC ${TOOLCHAIN_LIB_DIR})
      endif()
    endif()

    # For now turn off on Darwin swift targets, debug info if we are compiling a static
    # library and set up an rpath so that if someone works around this by using
    # shared libraries that in the short term we can find shared libraries.
    if (ASHL_STATIC)
      target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:Swift>:-gnone>)
    endif()
  endif()

  # If we are compiling in release or release with deb info, compile swift code
  # with -cross-module-optimization enabled.
  target_compile_options(${name} PRIVATE
    $<$<AND:$<COMPILE_LANGUAGE:Swift>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>>:-cross-module-optimization>)

  add_dependencies(dev ${name})
  if(NOT LLVM_INSTALL_TOOLCHAIN_ONLY)
    swift_install_in_component(TARGETS ${name}
      ARCHIVE DESTINATION lib${LLVM_LIBDIR_SUFFIX} COMPONENT dev
      LIBRARY DESTINATION lib${LLVM_LIBDIR_SUFFIX} COMPONENT dev
      RUNTIME DESTINATION bin COMPONENT dev)
  endif()

  swift_is_installing_component(dev is_installing)
  if(NOT is_installing)
    set_property(GLOBAL APPEND PROPERTY SWIFT_BUILDTREE_EXPORTS ${name})
  else()
    set_property(GLOBAL APPEND PROPERTY SWIFT_EXPORTS ${name})
  endif()
endfunction()

# Add a module of libswift
#
# Creates a target to compile a module which is part of libswift.
# Adds the module name to the global property "libswift_modules".
function(add_libswift_module module)
  cmake_parse_arguments(ALSM
                        ""
                        "DEPENDS"
                        ""
                        ${ARGN})
  set(sources ${ALSM_UNPARSED_ARGUMENTS})
  list(TRANSFORM sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

  set(target_name "LibSwift${module}")

  # Add a target which depends on the actual compilation target, which
  # will be created in add_libswift.
  # This target is mainly used to add properties, like the list of source files.
  add_custom_target(
      ${target_name}
      SOURCES ${sources}
      COMMENT "libswift module ${module}")

  set_property(TARGET ${target_name} PROPERTY "module_name" ${module})
  set_property(TARGET ${target_name} PROPERTY "module_depends" ${ALSM_DEPENDS})

  get_property(modules GLOBAL PROPERTY "libswift_modules")
  set_property(GLOBAL PROPERTY "libswift_modules" ${modules} ${module})
endfunction()
 
# Add source files to a libswift module.
function(libswift_sources module)
  cmake_parse_arguments(LSS
                        ""
                        ""
                        ""
                        ${ARGN})
  set(sources ${LSS_UNPARSED_ARGUMENTS})
  list(TRANSFORM sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

  set(target_name "LibSwift${module}")
  set_property(TARGET "LibSwift${module}" APPEND PROPERTY SOURCES ${sources})
endfunction()
 
# Add the libswift library.
#
# Adds targets to compile all modules of libswift and a target for the
# libswift library itself.
function(add_libswift name)
  if(CMAKE_BUILD_TYPE STREQUAL Debug)
    set(libswift_compile_options "-g")
  else()
    set(libswift_compile_options "-O" "-cross-module-optimization")
  endif()

  set(build_dir ${CMAKE_CURRENT_BINARY_DIR})

  if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_APPLE_PLATFORMS)
    set(deployment_version "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_DEPLOYMENT_VERSION}")
  endif()
  get_versioned_target_triple(target ${SWIFT_HOST_VARIANT_SDK}
      ${SWIFT_HOST_VARIANT_ARCH} "${deployment_version}")

  get_property(modules GLOBAL PROPERTY "libswift_modules")
  foreach(module ${modules})

    set(module_target "LibSwift${module}")
    get_target_property(module ${module_target} "module_name")
    get_target_property(sources ${module_target} SOURCES)
    get_target_property(dependencies ${module_target} "module_depends")
    if(dependencies)
      list(TRANSFORM dependencies PREPEND "LibSwift")
    else()
      set(dependencies "")
    endif()

    set(module_obj_file "${build_dir}/${module}.o")
    set(module_file "${build_dir}/${module}.swiftmodule")
    set_property(TARGET ${module_target} PROPERTY "module_file" "${module_file}")

    set(all_obj_files ${all_obj_files} ${module_obj_file})

    # Compile the libswift module into an object file
    add_custom_command_target(dep_target OUTPUT ${module_obj_file}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      DEPENDS ${sources} ${dependencies}
      COMMAND ${CMAKE_Swift_COMPILER} "-c" "-o" ${module_obj_file}
              "-sdk" "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_ARCH_${SWIFT_HOST_VARIANT_ARCH}_PATH}"
              "-target" ${target}
              "-module-name" ${module} "-emit-module"
              "-emit-module-path" "${build_dir}/${module}.swiftmodule"
              "-parse-as-library" ${sources}
              "-wmo" ${libswift_compile_options}
              "-I" "${CMAKE_SOURCE_DIR}/include/swift"
              "-I" "${build_dir}"
      COMMENT "Building libswift module ${module}")

    add_dependencies(${module_target} ${dep_target})

  endforeach()

  # Create a static libswift library containing all module object files.
  add_library(${name} STATIC ${all_obj_files})
  set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)
endfunction()

macro(add_swift_tool_subdirectory name)
  add_llvm_subdirectory(SWIFT TOOL ${name})
endmacro()

macro(add_swift_lib_subdirectory name)
  add_llvm_subdirectory(SWIFT LIB ${name})
endmacro()

function(add_swift_host_tool executable)
  set(options HAS_LIBSWIFT)
  set(single_parameter_options SWIFT_COMPONENT)
  set(multiple_parameter_options LLVM_LINK_COMPONENTS)

  cmake_parse_arguments(ASHT
    "${options}"
    "${single_parameter_options}"
    "${multiple_parameter_options}"
    ${ARGN})

  precondition(ASHT_SWIFT_COMPONENT
               MESSAGE "Swift Component is required to add a host tool")


  add_executable(${executable} ${ASHT_UNPARSED_ARGUMENTS})
  _add_host_variant_c_compile_flags(${executable})
  _add_host_variant_link_flags(${executable})
  _add_host_variant_c_compile_link_flags(${executable})
  target_link_directories(${executable} PRIVATE
    ${SWIFTLIB_DIR}/${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR})
  # Force executables linker language to be CXX so that we do not link using the
  # host toolchain swiftc.
  set_target_properties(${executable} PROPERTIES LINKER_LANGUAGE CXX)
  add_dependencies(${executable} ${LLVM_COMMON_DEPENDS})

  set_target_properties(${executable} PROPERTIES
    FOLDER "Swift executables")
  if(SWIFT_PARALLEL_LINK_JOBS)
    set_target_properties(${executable} PROPERTIES
      JOB_POOL_LINK swift_link_job_pool)
  endif()
  if(${SWIFT_HOST_VARIANT_SDK} IN_LIST SWIFT_APPLE_PLATFORMS)
    # If we found a swift compiler and are going to use swift code in swift
    # host side tools but link with clang, add the appropriate -L paths so we
    # find all of the necessary swift libraries on Darwin.
    if (CMAKE_Swift_COMPILER)
      # Add in the SDK directory for the host platform and add an rpath.
      target_link_directories(${executable} PRIVATE
        ${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_ARCH_${SWIFT_HOST_VARIANT_ARCH}_PATH}/usr/lib/swift)
      # Add in the toolchain directory so we can grab compatibility libraries
      get_filename_component(TOOLCHAIN_BIN_DIR ${CMAKE_Swift_COMPILER} DIRECTORY)
      get_filename_component(TOOLCHAIN_LIB_DIR "${TOOLCHAIN_BIN_DIR}/../lib/swift/macosx" ABSOLUTE)
      target_link_directories(${executable} PUBLIC ${TOOLCHAIN_LIB_DIR})

      if (ASHT_HAS_LIBSWIFT AND SWIFT_TOOLS_ENABLE_LIBSWIFT)
        # Workaround to make lldb happy: we have to explicitly add all libswift modules
        # to the linker command line.
        set(libswift_ast_path_flags "-Wl")
        get_property(modules GLOBAL PROPERTY "libswift_modules")
        foreach(module ${modules})
          get_target_property(module_file "LibSwift${module}" "module_file")
          string(APPEND libswift_ast_path_flags ",-add_ast_path,${module_file}")
        endforeach()

        set_property(TARGET ${executable} APPEND_STRING PROPERTY
                     LINK_FLAGS ${libswift_ast_path_flags})

        # Workaround for a linker crash related to autolinking: rdar://77839981
        set_property(TARGET ${executable} APPEND_STRING PROPERTY
                     LINK_FLAGS " -lobjc ")
      endif()
    endif()

    # Lists of rpaths that we are going to add to our executables.
    #
    # Please add each rpath separately below to the list, explaining why you are
    # adding it.
    set(RPATH_LIST)

    # We also want to be able to find libraries from the base toolchain
    # directory. This is so swiftc can rely on its own host side dylibs that may
    # contain swift content.
    list(APPEND RPATH_LIST "@executable_path/../lib")

    # Also include the abi stable system stdlib in our rpath.
    list(APPEND RPATH_LIST "/usr/lib/swift")

    set_target_properties(${executable} PROPERTIES
      BUILD_WITH_INSTALL_RPATH YES
      INSTALL_RPATH "${RPATH_LIST}")

  elseif(SWIFT_HOST_VARIANT_SDK STREQUAL "LINUX")
    if (ASHT_HAS_LIBSWIFT AND SWIFT_TOOLS_ENABLE_LIBSWIFT)
      # At build time and and run time, link against the swift libraries in the
      # installed host toolchain.
      get_filename_component(swift_bin_dir ${CMAKE_Swift_COMPILER} DIRECTORY)
      get_filename_component(swift_dir ${swift_bin_dir} DIRECTORY)
      set(host_lib_dir "${swift_dir}/lib/swift/linux")

      target_link_libraries(${executable} PRIVATE "swiftCore")

      target_link_directories(${executable} PRIVATE ${host_lib_dir})
      set_target_properties(${executable} PROPERTIES
        BUILD_WITH_INSTALL_RPATH YES
        INSTALL_RPATH  "${host_lib_dir}")
    endif()
  endif()

  llvm_update_compile_flags(${executable})
  swift_common_llvm_config(${executable} ${ASHT_LLVM_LINK_COMPONENTS})
  set_output_directory(${executable}
    BINARY_DIR ${SWIFT_RUNTIME_OUTPUT_INTDIR}
    LIBRARY_DIR ${SWIFT_LIBRARY_OUTPUT_INTDIR})

  if(SWIFT_HOST_VARIANT_SDK STREQUAL WINDOWS)
    swift_windows_include_for_arch(${SWIFT_HOST_VARIANT_ARCH}
      ${SWIFT_HOST_VARIANT_ARCH}_INCLUDE)
    target_include_directories(${executable} SYSTEM PRIVATE
      ${${SWIFT_HOST_VARIANT_ARCH}_INCLUDE})

    if(NOT ${CMAKE_C_COMPILER_ID} STREQUAL MSVC)
      # MSVC doesn't support -Xclang. We don't need to manually specify
      # the dependent libraries as `cl` does so.
      target_compile_options(${executable} PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:"SHELL:-Xclang --dependent-lib=oldnames">
        # TODO(compnerd) handle /MT, /MTd
        $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:"SHELL:-Xclang --dependent-lib=msvcrt$<$<CONFIG:Debug>:d>">
        )
    endif()
  endif()

  add_dependencies(${ASHT_SWIFT_COMPONENT} ${executable})
  swift_install_in_component(TARGETS ${executable}
                             RUNTIME
                               DESTINATION bin
                               COMPONENT ${ASHT_SWIFT_COMPONENT})

  swift_is_installing_component(${ASHT_SWIFT_COMPONENT} is_installing)

  if(NOT is_installing)
    set_property(GLOBAL APPEND PROPERTY SWIFT_BUILDTREE_EXPORTS ${executable})
  else()
    set_property(GLOBAL APPEND PROPERTY SWIFT_EXPORTS ${executable})
  endif()
endfunction()

# This declares a swift host tool that links with libfuzzer.
function(add_swift_fuzzer_host_tool executable)
  # First create our target. We do not actually parse the argument since we do
  # not care about the arguments, we just pass them all through to
  # add_swift_host_tool.
  add_swift_host_tool(${executable} ${ARGN})

  # Then make sure that we pass the -fsanitize=fuzzer flag both on the cflags
  # and cxx flags line.
  target_compile_options(${executable} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:"-fsanitize=fuzzer">)
  target_link_libraries(${executable} PRIVATE "-fsanitize=fuzzer")
endfunction()

macro(add_swift_tool_symlink name dest component)
  add_llvm_tool_symlink(${name} ${dest} ALWAYS_GENERATE)
  llvm_install_symlink(${name} ${dest} ALWAYS_GENERATE COMPONENT ${component})
endmacro()

# Declare that files in this library are built with LLVM's support
# libraries available.
function(set_swift_llvm_is_available name)
  target_compile_definitions(${name} PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:SWIFT_LLVM_SUPPORT_IS_AVAILABLE>)
endfunction()
