# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See http://swift.org/LICENSE.txt for license information
# See http://swift.org/CONTRIBUTORS.txt for Swift project authors

# Following function are needed as a workaround until it's possible to compile
# swift code with cmake's builtin swift support.

# Add a swift compiler module
#
# Creates a target to compile a swift module.
# Adds the module name to the global property "swift_compiler_modules".
#
function(add_swift_compiler_module module)
  cmake_parse_arguments(ALSM
                        ""
                        ""
                        "DEPENDS;SOURCES"
                        ${ARGN})
  set(raw_sources ${ALSM_SOURCES} ${ALSM_UNPARSED_ARGUMENTS})

  set(target_name "SwiftModule${module}")

  # Add a target which depends on the actual compilation target, which
  # will be created in add_swift_compiler_modules_library.
  # This target is mainly used to add properties, like the list of source files.
  add_custom_target(
      ${target_name}
      COMMENT "swift compiler module ${module}")

  swift_compiler_sources(${module} ${raw_sources})

  set_property(TARGET ${target_name} PROPERTY module_name ${module})
  set_property(TARGET ${target_name} PROPERTY module_depends ${ALSM_DEPENDS})

  get_property(modules GLOBAL PROPERTY swift_compiler_modules)
  set_property(GLOBAL PROPERTY swift_compiler_modules ${modules} ${module})
endfunction()

# Add source files to a swift compiler module.
#
function(swift_compiler_sources module)
  cmake_parse_arguments(LSS
                        ""
                        ""
                        ""
                        ${ARGN})
  set(raw_sources ${LSS_UNPARSED_ARGUMENTS})
  set(sources)
  foreach(raw_source ${raw_sources})
    get_filename_component(
      raw_source "${raw_source}" REALPATH BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND sources "${raw_source}")
  endforeach()

  set(target_name "SwiftModule${module}")
  set_property(TARGET "SwiftModule${module}" APPEND PROPERTY SOURCES ${sources})
endfunction()

# Allow the override of the flags used to define the SDK used to compile the
# Swift compiler sources from the CMake configuration (command line or cache
# files). This allows supporting complicated sysroots and some cross-compilation
# scenarios.
set(SWIFT_COMPILER_SOURCES_SDK_FLAGS_default)
if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_DARWIN_PLATFORMS)
  set(sdk_path "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_ARCH_${SWIFT_HOST_VARIANT_ARCH}_PATH}")
  list(APPEND SWIFT_COMPILER_SOURCES_SDK_FLAGS_default "-sdk" "${sdk_path}")
  if(NOT EXISTS "${sdk_path}/usr/include/c++")
    # Darwin SDKs in Xcode 12 or older do not include libc++, which prevents clang from finding libc++ when invoked
    # from ClangImporter. This results in build errors. To workaround this, let's explicitly pass the path to libc++
    # to clang.
    message(WARNING "Building with an outdated Darwin SDK: libc++ missing from the ${SWIFT_HOST_VARIANT_SDK} SDK. Will use libc++ from the toolchain.")
    get_filename_component(absolute_libcxx_path "${CMAKE_C_COMPILER}/../../include/c++/v1" REALPATH)
    if (EXISTS "${absolute_libcxx_path}")
      list(APPEND SWIFT_COMPILER_SOURCES_SDK_FLAGS_default "-Xcc" "-isystem" "-Xcc" "${absolute_libcxx_path}")
    else()
      message(ERROR "libc++ not found in the toolchain.")
    endif()
  endif()
elseif(BOOTSTRAPPING_MODE STREQUAL "CROSSCOMPILE")
  list(APPEND SWIFT_COMPILER_SOURCES_SDK_FLAGS_default "-sdk" "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_ARCH_${SWIFT_HOST_VARIANT_ARCH}_PATH}")
endif()
set(SWIFT_COMPILER_SOURCES_SDK_FLAGS ${SWIFT_COMPILER_SOURCES_SDK_FLAGS_default}
  CACHE STRING "Swift flags used to compiler the Swift compiler sources")

# Add a library target for the swift compiler modules.
#
# Adds targets to compile all swift compiler modules and a target for the
# library itself.
#
function(add_swift_compiler_modules_library name)
  cmake_parse_arguments(ALS
                        ""
                        "BOOTSTRAPPING;SWIFT_EXEC"
                        "DEPENDS"
                        ${ARGN})

  set(swift_compile_options
      "-color-diagnostics"
      "-Xfrontend" "-validate-tbd-against-ir=none"
      "${cxx_interop_flag}"
      "-Xfrontend" "-disable-target-os-checking"
      "-Xcc" "-std=c++17"
      "-Xcc" "-DCOMPILED_WITH_SWIFT" "-Xcc" "-DSWIFT_TARGET"
      "-Xcc" "-UIBOutlet" "-Xcc" "-UIBAction" "-Xcc" "-UIBInspectable")

  # Prior to 5.9, we have to use the experimental flag for C++ interop.
  if (CMAKE_Swift_COMPILER_VERSION VERSION_LESS 5.9)
    list(APPEND swift_compile_options "-Xfrontend" "-enable-experimental-cxx-interop")
  else()
    list(APPEND swift_compile_options "-cxx-interoperability-mode=default")
  endif()

  if (NOT BOOTSTRAPPING_MODE STREQUAL "HOSTTOOLS")
    if(SWIFT_MIN_RUNTIME_VERSION)
      list(APPEND swift_compile_options
        "-Xfrontend" "-min-runtime-version"
        "-Xfrontend" "${SWIFT_MIN_RUNTIME_VERSION}")
    endif()
    list(APPEND swift_compile_options "-Xfrontend" "-disable-implicit-string-processing-module-import")

    # We cannot use Unsafe*Pointer when importing C++ move-only types until the
    # host libraries are updated to Swift 6.0, because that importing strategy
    # requires _Pointer have its Pointee: ~Copyable. (rdar://128013193)
    list(APPEND swift_compile_options "-Xfrontend" "-cxx-interop-use-opaque-pointer-for-moveonly")
  endif()

  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND swift_compile_options "-g")
  else()
    list(APPEND swift_compile_options "-O" "-cross-module-optimization")
  endif()

  if(LLVM_ENABLE_ASSERTIONS)
    list(APPEND swift_compile_options "-Xcc" "-UNDEBUG")
  else()
    list(APPEND swift_compile_options "-Xcc" "-DNDEBUG")
  endif()

  if("${BRIDGING_MODE}" STREQUAL "PURE")
    list(APPEND swift_compile_options "-Xcc" "-DPURE_BRIDGING_MODE")
  endif()

  if(NOT SWIFT_STDLIB_SUPPORT_BACK_DEPLOYMENT)
    list(APPEND swift_compile_options "-Xfrontend" "-disable-legacy-type-info")
  endif()

  get_bootstrapping_path(build_dir ${CMAKE_CURRENT_BINARY_DIR} "${ALS_BOOTSTRAPPING}")

  set(sdk_option ${SWIFT_COMPILER_SOURCES_SDK_FLAGS})

  if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_DARWIN_PLATFORMS)
    set(deployment_version "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_DEPLOYMENT_VERSION}")
    if(BOOTSTRAPPING_MODE STREQUAL "CROSSCOMPILE-WITH-HOSTLIBS")
      # Let the cross-compiled compile don't pick up the compiled stdlib by providing
      # an (almost) empty resource dir.
      # The compiler will instead pick up the stdlib from the SDK.
      get_filename_component(swift_exec_bin_dir ${ALS_SWIFT_EXEC} DIRECTORY)
      list(APPEND sdk_option "-resource-dir" "${swift_exec_bin_dir}/../bootstrapping0/lib/swift")
    endif()
  elseif(BOOTSTRAPPING_MODE STREQUAL "CROSSCOMPILE")
    # NOTE: prepending allows SWIFT_COMPILER_SOURCES_SDK_FLAGS to override the
    # resource directory if needed.
    list(PREPEND sdk_option "-resource-dir" "${SWIFTLIB_DIR}")
  endif()
  get_versioned_target_triple(target ${SWIFT_HOST_VARIANT_SDK}
      ${SWIFT_HOST_VARIANT_ARCH} "${deployment_version}")

  # Let Swift discover SwiftShims headers which are included by some headers
  # under `include/swift`. These are either located next to the compiler (in case of open source toolchains) or
  # in the SDK (in case a Swift compiler from Xcode)
  get_filename_component(swift_exec_bin_dir ${ALS_SWIFT_EXEC} DIRECTORY)

  if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND swift_compile_options "-static")
    list(APPEND sdk_option "-sdk" "${SWIFT_PATH_TO_SWIFT_SDK}")

    # Workaround a crash in the LoadableByAddress pass
    # https://github.com/apple/swift/issues/73254
    list(APPEND swift_compile_options "-Xllvm" "-sil-disable-pass=loadable-address")

    # The STL in VS 17.10 requires Clang 17 or higher, but bootstrapping generally uses toolchains with older versions
    # versions of Clang. Swift 6 toolchains are the first to include Clang 17, so if we are on Windows and using an
    # earlier toolchain, we need to relax to relax this requirement with ALLOW_COMPILER_AND_STL_VERSION_MISMATCH.
    if (CMAKE_Swift_COMPILER_VERSION VERSION_LESS 6.0)
      list(APPEND swift_compile_options "-Xcc" "-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH")
    endif()
  else()
    list(APPEND sdk_option "-I" "${swift_exec_bin_dir}/../lib" "-I" "${sdk_path}/usr/lib")
  endif()

  set(all_obj_files)
  set(all_module_targets)
  set(all_module_files)
  get_property(modules GLOBAL PROPERTY "swift_compiler_modules")
  foreach(module ${modules})

    set(module_target "SwiftModule${module}")
    get_target_property(module ${module_target} "module_name")
    get_target_property(sources ${module_target} SOURCES)
    get_target_property(dependencies ${module_target} "module_depends")
    set(deps, "")
    if (dependencies)
      foreach(dep_module ${dependencies})
        if (DEFINED "${dep_module}_dep_target")
          # We have to add the module target for the ordering dependency
          # and the output file for the file dependency (otherwise the dependent
          # module wouldn't be rebuilt if the current module changes)
          list(APPEND deps "${${dep_module}_dep_target}" "${build_dir}/${dep_module}.o")
        else()
          message(FATAL_ERROR "module dependency ${module} -> ${dep_module} not found. Make sure to add modules in dependency order")
        endif()
      endforeach()
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_DARWIN_PLATFORMS)
      # swift-driver when non-optimizing in not Mach-O  platforms adds an extra
      # job wrapmodule that uses the {module-name}.o as output, which clashes
      # with the name chosen here. Use a different name in those cases.
      set(module_obj_file "${build_dir}/${module}.object.o")
      set(output_obj_files "${module_obj_file}" "${build_dir}/${module}.o")
    else()
      set(module_obj_file "${build_dir}/${module}.o")
      set(output_obj_files "${module_obj_file}")
    endif()
    set(module_file "${build_dir}/${module}.swiftmodule")
    list(APPEND all_module_files ${module_file})

    list(APPEND all_obj_files ${output_obj_files})
    set(c_include_paths
      # LLVM modules and headers.
      "${LLVM_MAIN_INCLUDE_DIR}"
      # Generated LLVM headers.
      "${LLVM_INCLUDE_DIR}"
      # Clang modules and headers.
      ${CLANG_INCLUDE_DIRS}
      # Bridging modules and headers.
      "${SWIFT_MAIN_INCLUDE_DIR}"
      # Generated C headers.
      "${CMAKE_CURRENT_BINARY_DIR}/../include")
    set(c_include_paths_args)
    foreach(c_include_path ${c_include_paths})
      list(APPEND c_include_paths_args "-Xcc" "-I" "-Xcc" "${c_include_path}")
    endforeach()

    # Compile the module into an object file
    add_custom_command_target(dep_target
      COMMAND ${ALS_SWIFT_EXEC} "-c" "-o" ${module_obj_file}
        ${sdk_option}
        "-target" ${target}
        "-module-name" ${module} "-emit-module"
        "-emit-module-path" "${build_dir}/${module}.swiftmodule"
        "-parse-as-library" ${sources}
        "-wmo" ${swift_compile_options}
        ${c_include_paths_args}
        # Generated swift modules.
        "-I" "${build_dir}"
      OUTPUT ${output_obj_files}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      DEPENDS ${sources} ${deps} ${ALS_DEPENDS}
        importedHeaderDependencies
      COMMENT "Building swift module ${module}")

    if(BOOTSTRAPPING_MODE STREQUAL "CROSSCOMPILE")
      add_dependencies(${dep_target} swift-stdlib-${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR}-${SWIFT_HOST_VARIANT_ARCH})
    endif()
    set("${module}_dep_target" ${dep_target})
    set(all_module_targets ${all_module_targets} ${dep_target})
  endforeach()

  # Create a static library containing all module object files.
  if (XCODE)
    # Xcode does not compile libraries that contain only object files.
    # Therefore, it fails to create the static library. As a workaround,
    # we add an empty source file force_lib.c to the target.
    set(all_obj_files force_lib.c ${all_obj_files})
  endif()
  add_library(${name} STATIC ${all_obj_files})
  add_dependencies(${name} ${all_module_targets})
  set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_link_libraries(${name} PUBLIC swiftSwiftOnoneSupport)
  endif()

  # Downstream linking should include the swiftmodule in debug builds to allow lldb to
  # work correctly. Only do this on Darwin since neither gold (currently used by default
  # on Linux), nor the default Windows linker 'link' support '-add_ast_path'.
  is_build_type_with_debuginfo("${CMAKE_BUILD_TYPE}" debuginfo)
  if(debuginfo AND SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_DARWIN_PLATFORMS)
    set(public_link_flags)
    foreach(module_file ${all_module_files})
      list(APPEND public_link_flags "SHELL:-Xlinker -add_ast_path -Xlinker ${module_file}")
    endforeach()
    target_link_options(${name} PUBLIC ${public_link_flags})
  endif()

  set_property(GLOBAL APPEND PROPERTY SWIFT_BUILDTREE_EXPORTS ${name})
endfunction()


# A dummy library if swift in the compiler is disabled
add_swift_host_library(swiftCompilerStub OBJECT stubs.cpp)

if (NOT BOOTSTRAPPING_MODE)

  add_library(swiftCompilerModules ALIAS swiftCompilerStub)

else()
  # Note: "Swift" is not added intentionally here, because it would break
  # the bootstrapping build in case no swift toolchain is installed on the host.
  project(SwiftInTheCompiler LANGUAGES C CXX)

  add_subdirectory(Sources)


  if(BOOTSTRAPPING_MODE MATCHES "HOSTTOOLS|CROSSCOMPILE")

    if (NOT SWIFT_EXEC_FOR_SWIFT_MODULES)
      message(FATAL_ERROR "Need a swift toolchain building swift compiler sources")
    endif()

    if(BOOTSTRAPPING_MODE STREQUAL "HOSTTOOLS")
      if(NOT SWIFT_EXEC_FOR_SWIFT_MODULES STREQUAL CMAKE_Swift_COMPILER)
        message(FATAL_ERROR "The Swift compiler (${CMAKE_Swift_COMPILER}) differs from the Swift compiler in SWIFT_NATIVE_SWIFT_TOOLS_PATH (${SWIFT_NATIVE_SWIFT_TOOLS_PATH}/swiftc).")
      endif()

      set(min_supported_swift_version 5.8)
      if(CMAKE_Swift_COMPILER_VERSION VERSION_LESS "${min_supported_swift_version}")
        message(FATAL_ERROR
            "Outdated Swift compiler: building with host tools requires Swift ${min_supported_swift_version} or newer. "
            "Please update your Swift toolchain or switch BOOTSTRAPPING_MODE to BOOTSTRAPPING(-WITH-HOSTLIBS)? or OFF.")
      endif()
    endif()

    add_swift_compiler_modules_library(swiftCompilerModules
      SWIFT_EXEC "${SWIFT_EXEC_FOR_SWIFT_MODULES}")

  elseif(BOOTSTRAPPING_MODE MATCHES "BOOTSTRAPPING.*")

    set(b0_deps swift-frontend-bootstrapping0 symlink-headers-bootstrapping0 copy-legacy-layouts)
    set(b1_deps swift-frontend-bootstrapping1 symlink-headers-bootstrapping1)
    if(BOOTSTRAPPING_MODE STREQUAL "BOOTSTRAPPING")
      list(APPEND b0_deps swiftCore-bootstrapping0)
      list(APPEND b1_deps swiftCore-bootstrapping1)
      if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND b0_deps swiftSwiftOnoneSupport-bootstrapping0)
        list(APPEND b1_deps swiftSwiftOnoneSupport-bootstrapping1)
      endif()
      if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_DARWIN_PLATFORMS)
        list(APPEND b0_deps swiftDarwin-bootstrapping0)
        list(APPEND b1_deps swiftDarwin-bootstrapping1)
      endif()
      if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_LIBSTDCXX_PLATFORMS)
        list(APPEND b0_deps copy-libstdcxx-modulemap-bootstrapping0 copy-libstdcxx-header-bootstrapping0)
        list(APPEND b1_deps copy-libstdcxx-modulemap-bootstrapping1 copy-libstdcxx-header-bootstrapping1)
      endif()
    endif()
    if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_DARWIN_PLATFORMS AND SWIFT_STDLIB_SUPPORT_BACK_DEPLOYMENT)
      # We cannot specify directly HostCompatibilityLibs
      # because ultimately is used to specify a dependency for a
      # custom target and, unlike `target_link_libraries`, such dependency
      # would be lost at the generation of the build system.
      get_property(compatibility_libs
        TARGET HostCompatibilityLibs
        PROPERTY INTERFACE_LINK_LIBRARIES)
      list(APPEND b0_deps ${compatibility_libs})
      list(APPEND b1_deps ${compatibility_libs})
    endif()


    # Bootstrapping - stage 1, using the compiler from level 0

    add_swift_compiler_modules_library(swiftCompilerModules-bootstrapping1
      SWIFT_EXEC $<TARGET_FILE_DIR:swift-frontend-bootstrapping0>/swiftc${CMAKE_EXECUTABLE_SUFFIX}
      DEPENDS ${b0_deps}
      BOOTSTRAPPING 1)

    # The final build, using the compiler from stage 1

    add_swift_compiler_modules_library(swiftCompilerModules
        SWIFT_EXEC $<TARGET_FILE_DIR:swift-frontend-bootstrapping1>/swiftc${CMAKE_EXECUTABLE_SUFFIX}
        DEPENDS ${b1_deps})

    if(BOOTSTRAPPING_MODE STREQUAL "BOOTSTRAPPING-WITH-HOSTLIBS")
      file(GLOB module_dirs "${CMAKE_BINARY_DIR}/bootstrapping*/lib/swift/macosx/*.swiftmodule")
      foreach(module_dir ${module_dirs})
        message(WARNING "${module_dir} found from a previous 'bootstrapping' build: removing")
        file(REMOVE_RECURSE "${module_dir}")
      endforeach()
    endif()
  else()
    message(FATAL_ERROR "Unknown BOOTSTRAPPING_MODE '${BOOTSTRAPPING_MODE}'")
  endif()

endif()
