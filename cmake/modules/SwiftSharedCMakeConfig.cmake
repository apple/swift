include(CMakeParseArguments)
include(SwiftXcodeSupport)

include(CheckCXXCompilerFlag)

macro(swift_common_standalone_build_config_llvm product)
  option(LLVM_ENABLE_WARNINGS "Enable compiler warnings." ON)

  # If we already have a cached value for LLVM_ENABLE_ASSERTIONS, save the value.
  if(DEFINED LLVM_ENABLE_ASSERTIONS)
    set(LLVM_ENABLE_ASSERTIONS_saved "${LLVM_ENABLE_ASSERTIONS}")
  endif()

  # Then we import LLVMConfig. This is going to override whatever cached value
  # we have for LLVM_ENABLE_ASSERTIONS.
  find_package(LLVM CONFIG REQUIRED NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")

  set(LLVM_MAIN_SRC_DIR "${LLVM_BUILD_MAIN_SRC_DIR}"
    CACHE PATH "Path to LLVM source tree")
  set(LLVM_MAIN_INCLUDE_DIR "${LLVM_BUILD_MAIN_INCLUDE_DIR}"
    CACHE PATH "Path to llvm/include")

  # If we did not have a cached value for LLVM_ENABLE_ASSERTIONS, set
  # LLVM_ENABLE_ASSERTIONS_saved to be the ENABLE_ASSERTIONS value from LLVM so
  # we follow LLVMConfig.cmake's value by default if nothing is provided.
  if(NOT DEFINED LLVM_ENABLE_ASSERTIONS_saved)
    set(LLVM_ENABLE_ASSERTIONS_saved "${LLVM_ENABLE_ASSERTIONS}")
  endif()

  # Then set LLVM_ENABLE_ASSERTIONS with a default value of
  # LLVM_ENABLE_ASSERTIONS_saved.
  #
  # The effect of this is that if LLVM_ENABLE_ASSERTION did not have a cached
  # value, then LLVM_ENABLE_ASSERTIONS_saved is set to LLVM's value, so we get a
  # default value from LLVM.
  set(LLVM_ENABLE_ASSERTIONS "${LLVM_ENABLE_ASSERTIONS_saved}"
    CACHE BOOL "Enable assertions")
  mark_as_advanced(LLVM_ENABLE_ASSERTIONS)

  precondition(LLVM_TOOLS_BINARY_DIR)
  escape_path_for_xcode("${LLVM_BUILD_TYPE}" "${LLVM_TOOLS_BINARY_DIR}" LLVM_TOOLS_BINARY_DIR)

  precondition_translate_flag(LLVM_BUILD_LIBRARY_DIR LLVM_LIBRARY_DIR)
  escape_path_for_xcode("${LLVM_BUILD_TYPE}" "${LLVM_LIBRARY_DIR}" LLVM_LIBRARY_DIR)

  precondition(LLVM_LIBRARY_DIRS)
  escape_path_for_xcode("${LLVM_BUILD_TYPE}" "${LLVM_LIBRARY_DIRS}" LLVM_LIBRARY_DIRS)

  # This could be computed using ${CMAKE_CFG_INTDIR} if we want to link Swift
  # against a matching LLVM build configuration.  However, we usually want to be
  # flexible and allow linking a debug Swift against optimized LLVM.
  set(LLVM_RUNTIME_OUTPUT_INTDIR "${LLVM_BINARY_DIR}")
  set(LLVM_BINARY_OUTPUT_INTDIR "${LLVM_TOOLS_BINARY_DIR}")
  set(LLVM_LIBRARY_OUTPUT_INTDIR "${LLVM_LIBRARY_DIR}")

  if(XCODE)
    fix_imported_targets_for_xcode("${LLVM_EXPORTED_TARGETS}")
  endif()

  if(NOT CMAKE_CROSSCOMPILING)
    set(${product}_NATIVE_LLVM_TOOLS_PATH "${LLVM_TOOLS_BINARY_DIR}")
  endif()

  if(SWIFT_INCLUDE_TOOLS)
    if(LLVM_TABLEGEN)
      set(LLVM_TABLEGEN_EXE ${LLVM_TABLEGEN})
    else()
      if(CMAKE_CROSSCOMPILING)
        set(LLVM_NATIVE_BUILD_DIR "${LLVM_BINARY_DIR}/NATIVE")
        if(NOT EXISTS "${LLVM_NATIVE_BUILD_DIR}")
          message(FATAL_ERROR
            "Attempting to cross-compile swift standalone but no native LLVM build
            found.  Please cross-compile LLVM as well.")
        endif()

        if(CMAKE_HOST_SYSTEM_NAME MATCHES Windows)
          set(HOST_EXECUTABLE_SUFFIX ".exe")
        endif()

        if(NOT CMAKE_CONFIGURATION_TYPES)
          set(LLVM_TABLEGEN_EXE
            "${LLVM_NATIVE_BUILD_DIR}/bin/llvm-tblgen${HOST_EXECUTABLE_SUFFIX}")
        else()
          # NOTE: LLVM NATIVE build is always built Release, as is specified in
          # CrossCompile.cmake
          set(LLVM_TABLEGEN_EXE
            "${LLVM_NATIVE_BUILD_DIR}/Release/bin/llvm-tblgen${HOST_EXECUTABLE_SUFFIX}")
        endif()
      else()
        find_program(LLVM_TABLEGEN_EXE "llvm-tblgen" HINTS ${LLVM_TOOLS_BINARY_DIR}
          NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
        if(LLVM_TABLEGEN_EXE STREQUAL "LLVM_TABLEGEN_EXE-NOTFOUND")
          message(FATAL_ERROR "Failed to find tablegen in ${LLVM_TOOLS_BINARY_DIR}")
        endif()
      endif()
    endif()
  endif()

  if(LLVM_ENABLE_ZLIB)
    find_package(ZLIB REQUIRED)
  endif()

  # Work around a bug in the swift-driver that causes the swift-driver to not be
  # able to accept .tbd files when linking without passing in the .tbd file with
  # a -Xlinker flag.
  #
  # Both clang and swiftc can accept an -Xlinker flag so we use that to pass the
  # value.
  if (APPLE)
    get_target_property(LLVMSUPPORT_INTERFACE_LINK_LIBRARIES LLVMSupport INTERFACE_LINK_LIBRARIES)
    set(new_libraries)
    foreach(lib ${LLVMSUPPORT_INTERFACE_LINK_LIBRARIES})
      if (TARGET ${lib})
        list(APPEND new_libraries "-Xlinker;$<TARGET_FILE:${lib}>")
        continue()
      endif()

      get_filename_component(LIB_FILENAME_COMPONENT ${lib} LAST_EXT)
      if ("${LIB_FILENAME_COMPONENT}" STREQUAL ".tbd")
        # This needs to be an actual string argument since if we use a list,
        # cmake seems to swallow the -Xlinker. But we need that -Xlinker for our
        # work around!
        list(APPEND new_libraries "-Xlinker ${lib}")
        continue()
      endif()

      list(APPEND new_libraries "${lib}")
    endforeach()

    set_target_properties(LLVMSupport PROPERTIES INTERFACE_LINK_LIBRARIES "${new_libraries}")
  endif()

  include(AddLLVM)
  include(AddSwiftTableGen) # This imports TableGen from LLVM.
  include(HandleLLVMOptions)

  # HACK: Not all targets support -z,defs as a linker flag. 
  #
  # Normally, LLVM would only add it as an option for known ELF targets;
  # however, due to the custom scheme Swift uses for cross-compilation, the 
  # CMAKE_SHARED_LINKER_FLAGS are determined based on the host system and 
  # then applied to all targets. This causes issues in cross-compiling to
  # Windows from a Linux host.
  # 
  # To work around this, we unconditionally remove the flag here and then
  # selectively add it to the per-target link flags; this is currently done in
  # add_swift_host_library and add_swift_target_library within AddSwift.cmake.
  string(REGEX REPLACE "-Wl,-z,defs" "" CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
  string(REGEX REPLACE "-Wl,-z,nodelete" "" CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
  # Android build on macOS cross-compile host don't support `-Wl,-headerpad_max_install_names` and `-dynamiclib` as a linker flags.
  string(REGEX REPLACE "-Wl,-headerpad_max_install_names" "" CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS "${CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS}")
  string(REGEX REPLACE "-dynamiclib" "" CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS "${CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS}")

  set(PACKAGE_VERSION "${LLVM_PACKAGE_VERSION}")
  string(REGEX REPLACE "([0-9]+)\\.[0-9]+(\\.[0-9]+)?" "\\1" PACKAGE_VERSION_MAJOR
    ${PACKAGE_VERSION})
  string(REGEX REPLACE "[0-9]+\\.([0-9]+)(\\.[0-9]+)?" "\\1" PACKAGE_VERSION_MINOR
    ${PACKAGE_VERSION})

  set(SWIFT_LIBCLANG_LIBRARY_VERSION
    "${PACKAGE_VERSION_MAJOR}.${PACKAGE_VERSION_MINOR}" CACHE STRING
    "Version number that will be placed into the libclang library , in the form XX.YY")

  foreach(INCLUDE_DIR ${LLVM_INCLUDE_DIRS})
    escape_path_for_xcode("${LLVM_BUILD_TYPE}" "${INCLUDE_DIR}" INCLUDE_DIR)
    include_directories(${INCLUDE_DIR})
  endforeach ()

  # *NOTE* if we want to support separate Clang builds as well as separate LLVM
  # builds, the clang build directory needs to be added here.
  link_directories("${LLVM_LIBRARY_DIR}")

  set(LIT_ARGS_DEFAULT "-sv")
  if(XCODE)
    set(LIT_ARGS_DEFAULT "${LIT_ARGS_DEFAULT} --no-progress-bar")
  endif()
  set(LLVM_LIT_ARGS "${LIT_ARGS_DEFAULT}" CACHE STRING "Default options for lit")

  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

  set(LLVM_INCLUDE_TESTS TRUE)
  set(LLVM_INCLUDE_DOCS TRUE)

  option(LLVM_ENABLE_DOXYGEN "Enable doxygen support" FALSE)
  if (LLVM_ENABLE_DOXYGEN)
    find_package(Doxygen REQUIRED)
  endif()
endmacro()

macro(swift_common_standalone_build_config_clang product)
  find_package(Clang CONFIG REQUIRED NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

  if (NOT CMAKE_CROSSCOMPILING AND NOT SWIFT_PREBUILT_CLANG)
    set(${product}_NATIVE_CLANG_TOOLS_PATH "${LLVM_TOOLS_BINARY_DIR}")
  endif()

  if(XCODE)
    fix_imported_targets_for_xcode("${CLANG_EXPORTED_TARGETS}")
  endif()

  include_directories(${CLANG_INCLUDE_DIRS})
endmacro()

macro(swift_common_standalone_build_config_cmark product)
  set(${product}_PATH_TO_CMARK_SOURCE "${${product}_PATH_TO_CMARK_SOURCE}"
    CACHE PATH "Path to CMark source code.")
  set(${product}_PATH_TO_CMARK_BUILD "${${product}_PATH_TO_CMARK_BUILD}"
    CACHE PATH "Path to the directory where CMark was built.")
  set(${product}_CMARK_LIBRARY_DIR "${${product}_CMARK_LIBRARY_DIR}" CACHE PATH
    "Path to the directory where CMark was installed.")
  get_filename_component(PATH_TO_CMARK_BUILD "${${product}_PATH_TO_CMARK_BUILD}"
    ABSOLUTE)
  get_filename_component(CMARK_MAIN_SRC_DIR "${${product}_PATH_TO_CMARK_SOURCE}"
    ABSOLUTE)
  get_filename_component(CMARK_LIBRARY_DIR "${${product}_CMARK_LIBRARY_DIR}"
    ABSOLUTE)

  set(CMARK_MAIN_INCLUDE_DIR "${CMARK_MAIN_SRC_DIR}/src")
  set(CMARK_BUILD_INCLUDE_DIR "${PATH_TO_CMARK_BUILD}/src")

  file(TO_CMAKE_PATH "${CMARK_MAIN_INCLUDE_DIR}" CMARK_MAIN_INCLUDE_DIR)
  file(TO_CMAKE_PATH "${CMARK_BUILD_INCLUDE_DIR}" CMARK_BUILD_INCLUDE_DIR)

  include_directories("${CMARK_MAIN_INCLUDE_DIR}"
                      "${CMARK_BUILD_INCLUDE_DIR}")

  include(${PATH_TO_CMARK_BUILD}/src/cmarkTargets.cmake)
  add_definitions(-DCMARK_STATIC_DEFINE)
endmacro()

# Common cmake project config for standalone builds.
#
# Parameters:
#   product
#     The product name, e.g. Swift or SourceKit. Used as prefix for some
#     cmake variables.
macro(swift_common_standalone_build_config product)
  swift_common_standalone_build_config_llvm(${product})
  if(SWIFT_INCLUDE_TOOLS)
    swift_common_standalone_build_config_clang(${product})
    swift_common_standalone_build_config_cmark(${product})
  endif()

  # Enable groups for IDE generators (Xcode and MSVC).
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endmacro()

# Common cmake project config for unified builds.
#
# Parameters:
#   product
#     The product name, e.g. Swift or SourceKit. Used as prefix for some
#     cmake variables.
macro(swift_common_unified_build_config product)
  set(${product}_PATH_TO_CLANG_BUILD "${CMAKE_BINARY_DIR}")
  set(CLANG_MAIN_INCLUDE_DIR "${LLVM_EXTERNAL_CLANG_SOURCE_DIR}/include")
  set(CLANG_BUILD_INCLUDE_DIR "${CMAKE_BINARY_DIR}/tools/clang/include")
  set(${product}_NATIVE_LLVM_TOOLS_PATH "${CMAKE_BINARY_DIR}/bin")
  set(${product}_NATIVE_CLANG_TOOLS_PATH "${CMAKE_BINARY_DIR}/bin")
  set(LLVM_PACKAGE_VERSION ${PACKAGE_VERSION})
  set(LLVM_CMAKE_DIR "${CMAKE_SOURCE_DIR}/cmake/modules")

  # If cmark was checked out into tools/cmark, expect to build it as
  # part of the unified build.
  if(EXISTS "${LLVM_EXTERNAL_CMARK_SOURCE_DIR}")
    set(${product}_PATH_TO_CMARK_SOURCE "${LLVM_EXTERNAL_CMARK_SOURCE_DIR}")
    set(${product}_PATH_TO_CMARK_BUILD "${CMAKE_BINARY_DIR}/tools/cmark")
    set(${product}_CMARK_LIBRARY_DIR "${CMAKE_BINARY_DIR}/lib")

    get_filename_component(CMARK_MAIN_SRC_DIR "${${product}_PATH_TO_CMARK_SOURCE}"
      ABSOLUTE)
    get_filename_component(PATH_TO_CMARK_BUILD "${${product}_PATH_TO_CMARK_BUILD}"
      ABSOLUTE)
    get_filename_component(CMARK_LIBRARY_DIR "${${product}_CMARK_LIBRARY_DIR}"
      ABSOLUTE)

    set(CMARK_BUILD_INCLUDE_DIR "${PATH_TO_CMARK_BUILD}/src")
    set(CMARK_MAIN_INCLUDE_DIR "${CMARK_MAIN_SRC_DIR}/src")
  endif()

  include_directories(
      "${CLANG_BUILD_INCLUDE_DIR}"
      "${CLANG_MAIN_INCLUDE_DIR}"
      "${CMARK_MAIN_INCLUDE_DIR}"
      "${CMARK_BUILD_INCLUDE_DIR}")

  include(AddSwiftTableGen) # This imports TableGen from LLVM.

  check_cxx_compiler_flag("-Werror -Wnested-anon-types" CXX_SUPPORTS_NO_NESTED_ANON_TYPES_FLAG)
  if(CXX_SUPPORTS_NO_NESTED_ANON_TYPES_FLAG)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-nested-anon-types")
  endif()
endmacro()

# Common cmake project config for additional warnings.
#
macro(swift_common_cxx_warnings)
  # Make unhandled switch cases be an error in assert builds
  if(DEFINED LLVM_ENABLE_ASSERTIONS)
    check_cxx_compiler_flag("-Werror=switch" CXX_SUPPORTS_WERROR_SWITCH_FLAG)
    append_if(CXX_SUPPORTS_WERROR_SWITCH_FLAG "-Werror=switch" CMAKE_CXX_FLAGS)

    check_cxx_compiler_flag("/we4062" CXX_SUPPORTS_WE4062)
    append_if(CXX_SUPPORTS_WE4062 "/we4062" CMAKE_CXX_FLAGS)
  endif()

  check_cxx_compiler_flag("-Werror -Wdocumentation" CXX_SUPPORTS_DOCUMENTATION_FLAG)
  append_if(CXX_SUPPORTS_DOCUMENTATION_FLAG "-Wdocumentation" CMAKE_CXX_FLAGS)

  check_cxx_compiler_flag("-Werror -Wimplicit-fallthrough" CXX_SUPPORTS_IMPLICIT_FALLTHROUGH_FLAG)
  append_if(CXX_SUPPORTS_IMPLICIT_FALLTHROUGH_FLAG "-Wimplicit-fallthrough" CMAKE_CXX_FLAGS)

  # Check for -Wunreachable-code-aggressive instead of -Wunreachable-code, as that indicates
  # that we have the newer -Wunreachable-code implementation.
  check_cxx_compiler_flag("-Werror -Wunreachable-code-aggressive" CXX_SUPPORTS_UNREACHABLE_CODE_FLAG)
  append_if(CXX_SUPPORTS_UNREACHABLE_CODE_FLAG "-Wunreachable-code" CMAKE_CXX_FLAGS)

  check_cxx_compiler_flag("-Werror -Woverloaded-virtual" CXX_SUPPORTS_OVERLOADED_VIRTUAL)
  append_if(CXX_SUPPORTS_OVERLOADED_VIRTUAL "-Woverloaded-virtual" CMAKE_CXX_FLAGS)

  # Check for '-fapplication-extension'.  On OS X/iOS we wish to link all
  # dynamic libraries with this flag.
  check_cxx_compiler_flag("-fapplication-extension" CXX_SUPPORTS_FAPPLICATION_EXTENSION)

  # Disable C4068: unknown pragma. This means that MSVC doesn't report hundreds of warnings across
  # the repository for IDE features such as #pragma mark "Title".
  if("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4068")
    check_cxx_compiler_flag("/permissive-" CXX_SUPPORTS_PERMISSIVE_FLAG)
    append_if(CXX_SUPPORTS_PERMISSIVE_FLAG "/permissive-" CMAKE_CXX_FLAGS)
  endif()

  # Disallow calls to objc_msgSend() with no function pointer cast.
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DOBJC_OLD_DISPATCH_PROTOTYPES=0")
endmacro()

# Like 'llvm_config()', but uses libraries from the selected build
# configuration in LLVM.  ('llvm_config()' selects the same build configuration
# in LLVM as we have for Swift.)
function(swift_common_llvm_config target)
  set(link_components ${ARGN})

  if((SWIFT_BUILT_STANDALONE OR SOURCEKIT_BUILT_STANDALONE) AND NOT "${CMAKE_CFG_INTDIR}" STREQUAL ".")
    llvm_map_components_to_libnames(libnames ${link_components})

    get_target_property(target_type "${target}" TYPE)
    if("${target_type}" STREQUAL "STATIC_LIBRARY")
      target_link_libraries("${target}" INTERFACE ${libnames})
    elseif("${target_type}" STREQUAL "SHARED_LIBRARY" OR
           "${target_type}" STREQUAL "MODULE_LIBRARY")
      target_link_libraries("${target}" PRIVATE ${libnames})
    else()
      # HACK: Otherwise (for example, for executables), use a plain signature,
      # because LLVM CMake does that already.
      target_link_libraries("${target}" PRIVATE ${libnames})
    endif()
  else()
    # If Swift was not built standalone, dispatch to 'llvm_config()'.
    llvm_config("${target}" ${ARGN})
  endif()
endfunction()
