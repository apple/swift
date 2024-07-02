
include(AddSwift)

add_custom_target(SwiftUnitTests)

set_target_properties(SwiftUnitTests PROPERTIES FOLDER "Tests")

# Add a new Swift unit test executable.
#
# Usage:
#   add_swift_unittest(name
#     [IS_TARGET_TEST]
#     source1 [source2 source3 ...])
#
# name
#   Name of the test (e.g., SwiftASTTest).
#
# IS_TARGET_TEST
#   Indicates this is a test for target libraries. Not host library.
#   This avoids linking with toolchains stdlib.
#
# source1 ...
#   Sources to add into this executable.
function(add_swift_unittest test_dirname)
  cmake_parse_arguments(ASU
                        "IS_TARGET_TEST"
                        ""
                        ""
                        ${ARGN})

  # *NOTE* Even though "add_unittest" does not have llvm in its name, it is a
  # function defined by AddLLVM.cmake.
  add_unittest(SwiftUnitTests ${test_dirname} ${ASU_UNPARSED_ARGUMENTS})

  set_target_properties(${test_dirname} PROPERTIES LINKER_LANGUAGE CXX)

  # TODO: _add_variant_c_compile_link_flags and these tests should share some
  # sort of logic.
  #
  # *NOTE* The unittests are never built for the target, so we always enable LTO
  # *if we are asked to.
  _compute_lto_flag("${SWIFT_TOOLS_ENABLE_LTO}" _lto_flag_out)
  if (_lto_flag_out)
    set_property(TARGET "${test_dirname}" APPEND_STRING PROPERTY COMPILE_FLAGS " ${_lto_flag_out} ")
    set_property(TARGET "${test_dirname}" APPEND_STRING PROPERTY LINK_FLAGS " ${_lto_flag_out} ")
  endif()

  if("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin")
    # Add an @rpath to the swift library directory
    # and one to the OS dylibs we require but
    # are not building ourselves (e.g Foundation overlay)
    set_target_properties(${test_dirname} PROPERTIES
      BUILD_RPATH "${SWIFT_LIBRARY_OUTPUT_INTDIR}/swift/macosx;${SWIFT_DARWIN_STDLIB_INSTALL_NAME_DIR}")
    # Force all the swift libraries to be found via rpath.
    add_custom_command(TARGET "${test_dirname}" POST_BUILD
      COMMAND "${SWIFT_SOURCE_DIR}/utils/swift-rpathize.py"
              "$<TARGET_FILE:${test_dirname}>")
  elseif("${SWIFT_HOST_VARIANT}" STREQUAL "android")
    set_property(TARGET "${test_dirname}" APPEND PROPERTY LINK_LIBRARIES "log")
  elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
      target_compile_options(${test_dirname} PRIVATE
        -march=core2)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "armv5|armv6|armv7|i686")
      set_property(TARGET "${test_dirname}" APPEND PROPERTY LINK_LIBRARIES
        "atomic")
    endif()
  elseif("${SWIFT_HOST_VARIANT}" STREQUAL "windows")
    target_compile_definitions("${test_dirname}" PRIVATE
      _ENABLE_EXTENDED_ALIGNED_STORAGE)
  endif()

  # some headers switch their inline implementations based on
  # SWIFT_STDLIB_SINGLE_THREADED_CONCURRENCY, SWIFT_STDLIB_HAS_DLSYM and
  # SWIFT_THREADING_PACKAGE definitions
  if(SWIFT_STDLIB_SINGLE_THREADED_CONCURRENCY)
    target_compile_definitions("${test_dirname}" PRIVATE
      SWIFT_STDLIB_SINGLE_THREADED_CONCURRENCY)
  endif()
  if(SWIFT_STDLIB_HAS_DLSYM)
    target_compile_definitions("${test_dirname}" PRIVATE
      "SWIFT_STDLIB_HAS_DLSYM=1")
  else()
    target_compile_definitions("${test_dirname}" PRIVATE
      "SWIFT_STDLIB_HAS_DLSYM=0")
  endif()

  string(TOUPPER "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_THREADING_PACKAGE}" _threading_package)
  target_compile_definitions("${test_dirname}" PRIVATE
    "SWIFT_THREADING_${_threading_package}"
    "SWIFT_THREADING_STATIC")

  if(NOT SWIFT_COMPILER_IS_MSVC_LIKE)
    if(SWIFT_USE_LINKER)
      target_link_options(${test_dirname} PRIVATE
        -fuse-ld=${SWIFT_USE_LINKER}$<$<STREQUAL:${CMAKE_HOST_SYSTEM_NAME},Windows>:.exe>)
    endif()
  endif()

  if(SWIFT_ANALYZE_CODE_COVERAGE)
    set_property(TARGET "${test_dirname}" APPEND_STRING PROPERTY
      LINK_FLAGS " -fprofile-instr-generate -fcoverage-mapping")
  endif()

  if(SWIFT_RUNTIME_USE_SANITIZERS)
    if("Thread" IN_LIST SWIFT_RUNTIME_USE_SANITIZERS)
      set_property(TARGET "${test_dirname}" APPEND_STRING PROPERTY COMPILE_FLAGS
        " -fsanitize=thread")
      set_property(TARGET "${test_dirname}" APPEND_STRING PROPERTY
        LINK_FLAGS " -fsanitize=thread")
    endif()
  endif()

  is_build_type_with_debuginfo("${CMAKE_BUILD_TYPE}" HAS_DEBUG_INFO)
  target_compile_options("${test_dirname}" PRIVATE $<$<BOOL:${HAS_DEBUG_INFO}>:-g>)

  file(RELATIVE_PATH relative_lib_path "${CMAKE_CURRENT_BINARY_DIR}" "${SWIFT_LIBRARY_OUTPUT_INTDIR}")

  if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_DARWIN_PLATFORMS)
    set_property(
      TARGET ${test_dirname}
      APPEND PROPERTY INSTALL_RPATH "@executable_path/${relative_lib_path}")
  elseif(SWIFT_HOST_VARIANT_SDK MATCHES "LINUX|ANDROID|OPENBSD|FREEBSD")
    set_property(
      TARGET ${test_dirname}
      APPEND PROPERTY INSTALL_RPATH "$ORIGIN/${relative_lib_path}")
  endif()

  if (SWIFT_BUILD_SWIFT_SYNTAX AND NOT ASU_IS_TARGET_TEST)
    # Link to stdlib the compiler uses.
    _add_swift_runtime_link_flags(${test_dirname} "${relative_lib_path}" "")

    if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_DARWIN_PLATFORMS)
      set_property(
        TARGET ${test_dirname}
        APPEND PROPERTY INSTALL_RPATH "@executable_path/${relative_lib_path}/swift/host/compiler")
    elseif(SWIFT_HOST_VARIANT_SDK MATCHES "LINUX|ANDROID|OPENBSD|FREEBSD")
      set_property(
        TARGET ${test_dirname}
        APPEND PROPERTY INSTALL_RPATH "$ORIGIN/${relative_lib_path}/swift/host/compiler")
    endif()
  endif()
endfunction()

