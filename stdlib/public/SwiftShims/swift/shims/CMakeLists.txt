set(sources
  AssertionReporting.h
  CoreFoundationShims.h
  FoundationShims.h
  GlobalObjects.h
  HeapObject.h
  KeyPath.h
  LibcOverlayShims.h
  LibcShims.h
  MetadataSections.h
  Random.h
  RefCount.h
  Reflection.h
  RuntimeShims.h
  RuntimeStubs.h
  SwiftStdbool.h
  SwiftStddef.h
  SwiftStdint.h
  System.h
  Target.h
  ThreadLocalStorage.h
  UnicodeData.h
  Visibility.h
  _SwiftConcurrency.h
  _SwiftDistributed.h

  module.modulemap
  )
set(output_dir "${SWIFTLIB_DIR}/shims")
set(output_dir_static "${SWIFTSTATICLIB_DIR}/shims")

add_custom_command(
    OUTPUT "${output_dir}"
    COMMAND ${CMAKE_COMMAND} "-E" "make_directory" "${output_dir}")

if(SWIFT_BUILD_STATIC_STDLIB)
  add_custom_command(
      OUTPUT "${output_dir_static}"
      COMMAND ${CMAKE_COMMAND} "-E" "make_directory" "${output_dir_static}")
endif()

set(outputs)
foreach(input ${sources})
  add_custom_command(
      OUTPUT "${output_dir}/${input}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${input}"
      COMMAND
        "${CMAKE_COMMAND}" "-E" "copy_if_different"
        "${CMAKE_CURRENT_SOURCE_DIR}/${input}"
        "${output_dir}/${input}"
      COMMENT "Copying ${input} to ${output_dir}")
  list(APPEND outputs "${output_dir}/${input}")

  if(SWIFT_BUILD_STATIC_STDLIB)
    add_custom_command(
        OUTPUT "${output_dir_static}/${input}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${input}"
        COMMAND
          "${CMAKE_COMMAND}" "-E" "copy_if_different"
          "${CMAKE_CURRENT_SOURCE_DIR}/${input}"
          "${output_dir_static}/${input}"
        COMMENT "Copying ${input} to ${output_dir_static}")
    list(APPEND outputs "${output_dir_static}/${input}")
  endif()
endforeach()
# Put the output dir itself last so that it isn't considered the primary output.
list(APPEND outputs "${output_dir}")

add_custom_target("copy_shim_headers" ALL
    DEPENDS "${outputs}"
    COMMENT "Copying SwiftShims module to ${output_dir}")

if ("${LLVM_PACKAGE_VERSION}" STREQUAL "")
  message(FATAL_ERROR
          "LLVM_PACKAGE_VERSION must be set before including subdirectories")
endif()

# Symlink in the Clang headers.
# First extract the "version" used for Clang's resource directory.
string(REGEX MATCH "[0-9]+\\.[0-9]+(\\.[0-9]+)?" CLANG_VERSION
  "${LLVM_PACKAGE_VERSION}")
if(NOT SWIFT_INCLUDE_TOOLS AND
   (SWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER OR SWIFT_PREBUILT_CLANG))
  if(SWIFT_COMPILER_IS_MSVC_LIKE)
    execute_process(COMMAND ${CMAKE_C_COMPILER} /clang:-print-resource-dir
      OUTPUT_VARIABLE clang_headers_location
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
  else()
    execute_process(COMMAND ${CMAKE_C_COMPILER} -print-resource-dir
      OUTPUT_VARIABLE clang_headers_location
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
  endif()
  message(STATUS "Using clang Resource Directory: ${clang_headers_location}")
else()
  set(clang_headers_location "${LLVM_LIBRARY_OUTPUT_INTDIR}/clang/${CLANG_VERSION}")
endif()

if("${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
  set(cmake_symlink_option "copy_directory")
else()
  set(cmake_symlink_option "create_symlink")
endif()

add_custom_command_target(unused_var
    COMMAND
      "${CMAKE_COMMAND}" "-E" "make_directory" "${SWIFTLIB_DIR}"
    COMMAND
      "${CMAKE_COMMAND}" "-E" "${cmake_symlink_option}"
      "${clang_headers_location}"
      "${SWIFTLIB_DIR}/clang"

    CUSTOM_TARGET_NAME "symlink_clang_headers"
    OUTPUT "${SWIFTLIB_DIR}/clang"
    COMMENT "Symlinking Clang resource headers into ${SWIFTLIB_DIR}/clang")

add_dependencies(copy_shim_headers symlink_clang_headers)

if(SWIFT_BUILD_STATIC_STDLIB)
  add_custom_command_target(unused_var
      COMMAND
        "${CMAKE_COMMAND}" "-E" "make_directory" "${SWIFTSTATICLIB_DIR}"
      COMMAND
        "${CMAKE_COMMAND}" "-E" "${cmake_symlink_option}"
        "${clang_headers_location}"
        "${SWIFTSTATICLIB_DIR}/clang"

      CUSTOM_TARGET_NAME "symlink_clang_headers_static"
      OUTPUT "${SWIFTSTATICLIB_DIR}/clang"
      COMMENT "Symlinking Clang resource headers into ${SWIFTSTATICLIB_DIR}/clang")

      add_dependencies(copy_shim_headers symlink_clang_headers_static)
endif()

if(NOT SWIFT_BUILT_STANDALONE)
  if(TARGET clang-resource-headers) # LLVM > 8
    set(clang_resource_headers clang-resource-headers)
  elseif(TARGET clang-headers) # LLVM <= 8
    set(clang_resource_headers clang-headers)
  else()
    message(SEND_ERROR
      "Unable to determine clang resource headers target in unified build")
  endif()

  foreach(target
      symlink_clang_headers
      clang-builtin-headers
      clang-resource-dir-symlink
      clang-builtin-headers-in-clang-resource-dir)
    add_dependencies(${target} ${clang_resource_headers})
  endforeach()
endif()

if(BOOTSTRAPPING_MODE MATCHES "BOOTSTRAPPING.*")
  foreach(bootstrapping "0" "1")
    get_bootstrapping_path(outdir ${SWIFTLIB_DIR} ${bootstrapping})
    set(target_name "symlink-headers-bootstrapping${bootstrapping}")

    add_custom_command_target(unused_var
        COMMAND
          "${CMAKE_COMMAND}" "-E" "make_directory" "${outdir}"
        COMMAND
          "${CMAKE_COMMAND}" "-E" ${cmake_symlink_option}
          ${output_dir} "${outdir}/shims"
        COMMAND
          "${CMAKE_COMMAND}" "-E" "${cmake_symlink_option}"
          "${clang_headers_location}"
          "${outdir}/clang"

        CUSTOM_TARGET_NAME ${target_name}
        OUTPUT "${outdir}/shims" "${outdir}/clang"
        COMMENT "Symlinking headers for bootstrapping0")

    add_dependencies(${target_name} copy_shim_headers)
  endforeach()
endif()

swift_install_in_component(FILES ${sources}
                           DESTINATION "lib/swift/shims"
                           COMPONENT stdlib)

if(SWIFT_BUILD_STATIC_STDLIB)
  swift_install_in_component(FILES ${sources}
                             DESTINATION "lib/swift_static/shims"
                             COMPONENT stdlib)
endif()

# Install Clang headers under the Swift library so that an installed Swift's
# module importer can find the compiler headers corresponding to its Clang.
swift_install_in_component(DIRECTORY "${clang_headers_location}/"
                           DESTINATION "lib/swift/clang"
                           COMPONENT clang-builtin-headers
                           PATTERN "*.h")

if(SWIFT_BUILD_STATIC_STDLIB)
  swift_install_in_component(DIRECTORY "${clang_headers_location}/"
                             DESTINATION "lib/swift_static/clang"
                             COMPONENT clang-builtin-headers
                             PATTERN "*.h")
endif()

if(SWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER OR SWIFT_PREBUILT_CLANG)
  # This will still link against the Swift-forked clang headers if the Swift
  # toolchain was built with SWIFT_INCLUDE_TOOLS.
  set(SWIFT_CLANG_RESOURCE_DIR_SYMLINK_INSTALL_TARGET_default ${clang_headers_location})
else()
  set(SWIFT_CLANG_RESOURCE_DIR_SYMLINK_INSTALL_TARGET_default "../clang/${CLANG_VERSION}")
endif()

set(SWIFT_CLANG_RESOURCE_DIR_SYMLINK_INSTALL_TARGET
  ${SWIFT_CLANG_RESOURCE_DIR_SYMLINK_INSTALL_TARGET_default} CACHE STRING "The target of the installed symlink at lib/swift/clang")

swift_install_symlink_component(clang-resource-dir-symlink
  LINK_NAME clang
  TARGET ${SWIFT_CLANG_RESOURCE_DIR_SYMLINK_INSTALL_TARGET}
  DESTINATION "lib/swift")

if(SWIFT_BUILD_STATIC_STDLIB)
  swift_install_symlink_component(clang-resource-dir-symlink
    LINK_NAME clang
    TARGET ${SWIFT_CLANG_RESOURCE_DIR_SYMLINK_INSTALL_TARGET}
    DESTINATION "lib/swift_static")
endif()

# Possibly install Clang headers under Clang's resource directory in case we
# need to use a different version of the headers than the installed Clang. This
# should be used in conjunction with clang-resource-dir-symlink.
file(TO_CMAKE_PATH "${LLVM_LIBRARY_OUTPUT_INTDIR}"
  _SWIFT_SHIMS_PATH_TO_CLANG_LIB_BUILD)
swift_install_in_component(DIRECTORY "${_SWIFT_SHIMS_PATH_TO_CLANG_LIB_BUILD}/clang"
                           DESTINATION "lib"
                           COMPONENT clang-builtin-headers-in-clang-resource-dir
                           PATTERN "*.h")
