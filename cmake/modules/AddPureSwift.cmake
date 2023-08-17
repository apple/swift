include(macCatalystUtils)

# Workaround a cmake bug, see the corresponding function in swift-syntax
function(force_target_link_libraries TARGET)
  cmake_parse_arguments(ARGS "" "" "PUBLIC" ${ARGN})

  foreach(DEPENDENCY ${ARGS_PUBLIC})
    target_link_libraries(${TARGET} PRIVATE
      ${DEPENDENCY}
    )
    add_dependencies(${TARGET} ${DEPENDENCY})

    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/forced-${DEPENDENCY}-dep.swift
      COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/forced-${DEPENDENCY}-dep.swift
      DEPENDS ${DEPENDENCY}
      )
    target_sources(${TARGET} PRIVATE
      ${CMAKE_CURRENT_BINARY_DIR}/forced-${DEPENDENCY}-dep.swift
    )
  endforeach()
endfunction()

# Add compile options shared between libraries and executables.
function(_add_host_swift_compile_options name)
  # Avoid introducing an implicit dependency on the string-processing library.
  if(SWIFT_SUPPORTS_DISABLE_IMPLICIT_STRING_PROCESSING_MODULE_IMPORT)
    target_compile_options(${name} PRIVATE
      "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-Xfrontend -disable-implicit-string-processing-module-import>")
  endif()

  # Same for backtracing
  if (SWIFT_SUPPORTS_DISABLE_IMPLICIT_BACKTRACING_MODULE_IMPORT)
    target_compile_options(${name} PRIVATE
      "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-Xfrontend -disable-implicit-backtracing-module-import>")
  endif()

  # The compat56 library is not available in current toolchains. The stage-0
  # compiler will build fine since the builder compiler is not aware of the 56
  # compat library, but the stage-1 and subsequent stage compilers will fail as
  # the stage-0 compiler is aware and will attempt to include the appropriate
  # compatibility library. We should turn this back on once we are building the
  # compiler correctly.
  # Note: This is safe at the moment because the 5.6 compat library only
  #       contains concurrency runtime fixes, and the compiler frontend does not
  #       use concurrency at the moment.
  target_compile_options(${name} PRIVATE
    $<$<COMPILE_LANGUAGE:Swift>:-runtime-compatibility-version>
    $<$<COMPILE_LANGUAGE:Swift>:none>)

  # Set the appropriate target triple.
  # FIXME: This should be set by CMake.
  if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_DARWIN_PLATFORMS)
    set(DEPLOYMENT_VERSION "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_DEPLOYMENT_VERSION}")
  endif()

  if(SWIFT_HOST_VARIANT_SDK STREQUAL "ANDROID")
    set(DEPLOYMENT_VERSION ${SWIFT_ANDROID_API_LEVEL})
  endif()

  get_target_triple(target target_variant "${SWIFT_HOST_VARIANT_SDK}" "${SWIFT_HOST_VARIANT_ARCH}"
    MACCATALYST_BUILD_FLAVOR ""
    DEPLOYMENT_VERSION "${DEPLOYMENT_VERSION}")

  target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:Swift>:-target;${target}>)
  _add_host_variant_swift_sanitizer_flags(${name})
endfunction()

# Add a new "pure" Swift host library.
#
# "Pure" Swift host libraries can only contain Swift code, and will be built
# with the host compiler. They are always expected to be part of the built
# compiler, without bootstrapping.
#
# All of these libraries depend on the swift-syntax stack, since they are
# meant to be part of the compiler.
#
# Usage:
#   add_pure_swift_host_library(name
#     [SHARED]
#     [STATIC]
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
# EMIT_MODULE
#   Emit '.swiftmodule' to
#
# DEPENDENCIES
#   Target names to pass target_link_library
#
# SWIFT_DEPENDENCIES
#   Target names to pass force_target_link_library.
#   TODO: Remove this and use DEPENDENCIES when CMake is fixed
#
# source1 ...
#   Sources to add into this library.
function(add_pure_swift_host_library name)
  if (NOT SWIFT_SWIFT_PARSER)
    message(STATUS "Not building ${name} because swift-syntax is not available")
    return()
  endif()

  # Option handling
  set(options
        SHARED
        STATIC
        EMIT_MODULE)
  set(single_parameter_options)
  set(multiple_parameter_options
        DEPENDENCIES
        SWIFT_DEPENDENCIES)

  cmake_parse_arguments(APSHL
                        "${options}"
                        "${single_parameter_options}"
                        "${multiple_parameter_options}"
                        ${ARGN})
  set(APSHL_SOURCES ${APSHL_UNPARSED_ARGUMENTS})

  translate_flags(APSHL "${options}")

  # Determine what kind of library we're building.
  if(APSHL_SHARED)
    set(libkind SHARED)
  elseif(APSHL_STATIC)
    set(libkind STATIC)
  endif()

  # Create the library.
  add_library(${name} ${libkind} ${APSHL_SOURCES})
  _add_host_swift_compile_options(${name})

  set_property(TARGET ${name}
    PROPERTY BUILD_WITH_INSTALL_RPATH YES)

  if(APSHL_SHARED AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # Allow install_name_tool to update paths (for rdar://109473564)
    set_property(TARGET ${name} APPEND_STRING PROPERTY
                 LINK_FLAGS " -Xlinker -headerpad_max_install_names")
  endif()

  # Respect LLVM_COMMON_DEPENDS if it is set.
  #
  # LLVM_COMMON_DEPENDS if a global variable set in ./lib that provides targets
  # such as swift-syntax or tblgen that all LLVM/Swift based tools depend on. If
  # we don't have it defined, then do not add the dependency since some parts of
  # swift host tools do not interact with LLVM/Swift tools and do not define
  # LLVM_COMMON_DEPENDS.
  if (LLVM_COMMON_DEPENDS)
    add_dependencies(${name} ${LLVM_COMMON_DEPENDS})
  endif()

  # Workaround to touch the library and its objects so that we don't
  # continually rebuild (again, see corresponding change in swift-syntax).
  add_custom_command(
      TARGET ${name}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E touch_nocreate $<TARGET_FILE:${name}> $<TARGET_OBJECTS:${name}> "${SWIFT_HOST_LIBRARIES_DEST_DIR}/${name}.swiftmodule" "${CMAKE_CURRENT_BINARY_DIR}/${name}.swiftmodule"
      COMMAND_EXPAND_LISTS
      COMMENT "Update mtime of library outputs workaround")

  # Link against dependencies.
  target_link_libraries(${name} PUBLIC
    ${APSHL_DEPENDENCIES}
  )
  # TODO: Change to target_link_libraries when cmake is fixed
  force_target_link_libraries(${name} PUBLIC
    ${APSHL_SWIFT_DEPENDENCIES}
  )

  # Make sure we can use the host libraries.
  target_include_directories(${name} PUBLIC
    ${SWIFT_HOST_LIBRARIES_DEST_DIR})

  if(APSHL_EMIT_MODULE)
    # Determine where Swift modules will be built and installed.

    set(module_triple ${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_ARCH_${SWIFT_HOST_VARIANT_ARCH}_MODULE})
    set(module_dir ${SWIFT_HOST_LIBRARIES_DEST_DIR})
    set(module_base "${module_dir}/${name}.swiftmodule")
    set(module_file "${module_base}/${module_triple}.swiftmodule")
    set(module_interface_file "${module_base}/${module_triple}.swiftinterface")
    set(module_sourceinfo_file "${module_base}/${module_triple}.swiftsourceinfo")

    set_target_properties(${name} PROPERTIES
        # Set the default module name to the target name.
        Swift_MODULE_NAME ${name}
        # Install the Swift module into the appropriate location.
        Swift_MODULE_DIRECTORY ${module_dir}
        # NOTE: workaround for CMake not setting up include flags.
        INTERFACE_INCLUDE_DIRECTORIES ${module_dir})

    # Create the module directory.
    add_custom_command(
        TARGET ${name}
        PRE_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory ${module_base}
        COMMENT "Generating module directory for ${name}")

    # Configure the emission of the Swift module files.
    target_compile_options("${name}" PRIVATE
        $<$<COMPILE_LANGUAGE:Swift>:
        -module-name;$<TARGET_PROPERTY:${name},Swift_MODULE_NAME>;
        -enable-library-evolution;
        -emit-module-path;${module_file};
        -emit-module-source-info-path;${module_sourceinfo_file};
        -emit-module-interface-path;${module_interface_file}
        >)
  endif()

  # Export this target.
  set_property(GLOBAL APPEND PROPERTY SWIFT_EXPORTS ${name})
endfunction()

# Add a new "pure" Swift host tool.
#
# "Pure" Swift host tools can only contain Swift code, and will be built
# with the host compiler. 
#
# Usage:
#   add_pure_swift_host_tool(name
#     [DEPENDENCIES dep1 ...]
#     [SWIFT_DEPENDENCIES swiftdep1 ...]
#     source1 [source2 source3 ...])
#
# name
#   Name of the tool (e.g., swift-frontend).
#
# DEPENDENCIES
#   Target names to pass target_link_library
#
# SWIFT_DEPENDENCIES
#   Target names to pass force_target_link_library.
#   TODO: Remove this and use DEPENDENCIES when CMake is fixed
#
# source1 ...
#   Sources to add into this tool.
function(add_pure_swift_host_tool name)
  if (NOT SWIFT_SWIFT_PARSER)
    message(STATUS "Not building ${name} because swift-syntax is not available")
    return()
  endif()

  # Option handling
  set(options)
  set(single_parameter_options)
  set(multiple_parameter_options
        DEPENDENCIES
        SWIFT_DEPENDENCIES)

  cmake_parse_arguments(APSHT
                        "${options}"
                        "${single_parameter_options}"
                        "${multiple_parameter_options}"
                        ${ARGN})
  set(APSHT_SOURCES ${APSHT_UNPARSED_ARGUMENTS})

  # Create the library.
  add_executable(${name} ${APSHT_SOURCES})
  _add_host_swift_compile_options(${name})

  if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_DARWIN_PLATFORMS)
    set_property(TARGET ${name}
      APPEND PROPERTY INSTALL_RPATH
        "@executable_path/../lib/swift/host")
  else()
    set_property(TARGET ${name}
      APPEND PROPERTY INSTALL_RPATH
        "$ORIGIN/../lib/swift/host")
  endif()

  set_property(TARGET ${name}
    PROPERTY BUILD_WITH_INSTALL_RPATH YES)

  # Respect LLVM_COMMON_DEPENDS if it is set.
  #
  # LLVM_COMMON_DEPENDS if a global variable set in ./lib that provides targets
  # such as swift-syntax or tblgen that all LLVM/Swift based tools depend on. If
  # we don't have it defined, then do not add the dependency since some parts of
  # swift host tools do not interact with LLVM/Swift tools and do not define
  # LLVM_COMMON_DEPENDS.
  if (LLVM_COMMON_DEPENDS)
    add_dependencies(${name} ${LLVM_COMMON_DEPENDS})
  endif()

  # Link against dependencies.
  target_link_libraries(${name} PUBLIC
    ${APSHT_DEPENDENCIES}
  )
  # TODO: Change to target_link_libraries when cmake is fixed
  force_target_link_libraries(${name} PUBLIC
    ${APSHT_SWIFT_DEPENDENCIES}
  )

  # Make sure we can use the host libraries.
  target_include_directories(${name} PUBLIC
    ${SWIFT_HOST_LIBRARIES_DEST_DIR})

  # Workaround to touch the library and its objects so that we don't
  # continually rebuild (again, see corresponding change in swift-syntax).
  add_custom_command(
      TARGET ${name}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E touch_nocreate $<TARGET_FILE:${name}> $<TARGET_OBJECTS:${name}>
      COMMAND_EXPAND_LISTS
      COMMENT "Update mtime of executable outputs workaround")

  # Even worse hack - ${name}.swiftmodule is added as an output, even though
  # this is an executable target. Just touch it all the time to avoid having
  # to rebuild it every time.
  add_custom_command(
      TARGET ${name}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/${name}.swiftmodule"
      COMMAND_EXPAND_LISTS
      COMMENT "Update mtime of executable outputs workaround")
  # Export this target.
  set_property(GLOBAL APPEND PROPERTY SWIFT_EXPORTS ${name})
endfunction()
