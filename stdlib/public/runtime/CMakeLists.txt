set(swift_runtime_compile_flags ${SWIFT_RUNTIME_CORE_CXX_FLAGS})
set(swift_runtime_linker_flags ${SWIFT_RUNTIME_CORE_LINK_FLAGS})

if(SWIFT_RUNTIME_CLOBBER_FREED_OBJECTS)
  list(APPEND swift_runtime_compile_flags
      "-DSWIFT_RUNTIME_CLOBBER_FREED_OBJECTS=1")
endif()

if(SWIFT_RUNTIME_CRASH_REPORTER_CLIENT)
  list(APPEND swift_runtime_compile_flags
      "-DSWIFT_HAVE_CRASHREPORTERCLIENT=1")
endif()

set(swift_runtime_leaks_sources)
if(SWIFT_RUNTIME_ENABLE_LEAK_CHECKER)
  list(APPEND swift_runtime_compile_flags
       "-DSWIFT_RUNTIME_ENABLE_LEAK_CHECKER=1")
  set(swift_runtime_leaks_sources Leaks.mm)
endif()

if(SWIFT_RUNTIME_FIXED_BACKTRACER_PATH)
  list(APPEND swift_runtime_compile_flags
    "-DSWIFT_RUNTIME_FIXED_BACKTRACER_PATH=\"${SWIFT_RUNTIME_FIXED_BACKTRACER_PATH}\"")
endif()

set(swift_runtime_objc_sources
    ErrorObject.mm
    SwiftObject.mm
    SwiftValue.mm
    ReflectionMirrorObjC.mm
    ObjCRuntimeGetImageNameFromClass.mm)

set(swift_runtime_sources
    ../CompatibilityOverride/CompatibilityOverride.cpp
    AnyHashableSupport.cpp
    Array.cpp
    AutoDiffSupport.cpp
    Bincompat.cpp
    BytecodeLayouts.cpp
    Casting.cpp
    CrashReporter.cpp
    Demangle.cpp
    DynamicCast.cpp
    Enum.cpp
    EnvironmentVariables.cpp
    ErrorObjectCommon.cpp
    ErrorObjectNative.cpp
    Errors.cpp
    ErrorDefaultImpls.cpp
    Exclusivity.cpp
    ExistentialContainer.cpp
    Float16Support.cpp
    FoundationSupport.cpp
    FunctionReplacement.cpp
    Heap.cpp
    HeapObject.cpp
    ImageInspectionCommon.cpp
    ImageInspectionMachO.cpp
    ImageInspectionELF.cpp
    ImageInspectionCOFF.cpp
    ImageInspectionStatic.cpp
    ImageInspectionWasm.cpp
    SymbolInfo.cpp
    KeyPaths.cpp
    KnownMetadata.cpp
    Metadata.cpp
    MetadataLookup.cpp
    Numeric.cpp
    Once.cpp
    Paths.cpp
    Portability.cpp
    ProtocolConformance.cpp
    RefCount.cpp
    ReflectionMirror.cpp
    RuntimeInvocationsTracking.cpp
    SwiftDtoa.cpp
    SwiftTLSContext.cpp
    ThreadingError.cpp
    Tracing.cpp
    AccessibleFunction.cpp
    Win32.cpp)

# We pull this in separately here because other dylibs will need it, but only
# will have the __tsan_on_initialize called, and on Darwin, RTLD_NEXT can't be
# used to call subsequence dylibs' copies of that.
set(swift_runtime_threading_sources
  ${SWIFT_SOURCE_DIR}/lib/Threading/ThreadSanitizer.cpp)

set(swift_runtime_backtracing_sources
  Backtrace.cpp
  CrashHandlerMacOS.cpp
  CrashHandlerLinux.cpp)

# Acknowledge that the following sources are known.
set(LLVM_OPTIONAL_SOURCES
    SwiftRT-COFF.cpp
    SwiftRT-ELF-WASM.cpp
    ${swift_runtime_sources}
    ${swift_runtime_objc_sources}
    ${swift_runtime_leaks_sources}
    ${swift_runtime_backtracing_sources})

if(SWIFT_ENABLE_BACKTRACING)
  list(APPEND swift_runtime_sources ${swift_runtime_backtracing_sources})
endif()

set(swift_runtime_library_compile_flags ${swift_runtime_compile_flags})
list(APPEND swift_runtime_library_compile_flags -DswiftCore_EXPORTS)
list(APPEND swift_runtime_library_compile_flags -I${SWIFT_SOURCE_DIR}/stdlib/include/llvm/Support -I${SWIFT_SOURCE_DIR}/include)

if(SWIFT_BUILD_STATIC_STDLIB)
  set(static_binary_lnk_file_list)

  foreach(sdk ${SWIFT_SDKS})
    if(NOT "${sdk}" STREQUAL "LINUX" AND NOT "${sdk}" STREQUAL "WASI")
      continue()
    endif()

    string(TOLOWER "${sdk}" lowercase_sdk)
    set(static_binary_lnk_src "${SWIFT_SOURCE_DIR}/stdlib/public/Resources/${lowercase_sdk}/static-executable-args.lnk")

    # Generate the static-executable-args.lnk file used for ELF systems (eg linux)
    set(linkfile "${lowercase_sdk}/static-executable-args.lnk")
    add_custom_command_target(swift_static_binary_${sdk}_args
      COMMAND
        "${CMAKE_COMMAND}" -E copy
        "${static_binary_lnk_src}"
        "${SWIFTSTATICLIB_DIR}/${linkfile}"
      OUTPUT
        "${SWIFTSTATICLIB_DIR}/${linkfile}"
      DEPENDS
        "${static_binary_lnk_src}")

    list(APPEND static_binary_lnk_file_list ${swift_static_binary_${sdk}_args})
    swift_install_in_component(FILES "${SWIFTSTATICLIB_DIR}/${linkfile}"
                               DESTINATION "lib/swift_static/${lowercase_sdk}"
                               COMPONENT stdlib)
  endforeach()
  if(static_binary_lnk_file_list)
    add_dependencies(stdlib ${static_binary_lnk_file_list})
    add_custom_target(static_binary_magic ALL DEPENDS ${static_binary_lnk_file_list})
  endif()
endif()

add_swift_target_library(swiftRuntime OBJECT_LIBRARY
  ${swift_runtime_sources}
  ${swift_runtime_objc_sources}
  ${swift_runtime_leaks_sources}
  ${swift_runtime_threading_sources}
  C_COMPILE_FLAGS
    ${swift_runtime_library_compile_flags}
  LINK_FLAGS ${swift_runtime_linker_flags}
  SWIFT_COMPILE_FLAGS ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
  INSTALL_IN_COMPONENT never_install)

set(ELFISH_SDKS)
set(COFF_SDKS)
set(WASM_SDKS)
foreach(sdk ${SWIFT_SDKS})
  if("${SWIFT_SDK_${sdk}_OBJECT_FORMAT}" STREQUAL "ELF")
    list(APPEND ELFISH_SDKS ${sdk})
  elseif("${SWIFT_SDK_${sdk}_OBJECT_FORMAT}" STREQUAL "COFF")
    list(APPEND COFF_SDKS ${sdk})
  elseif("${SWIFT_SDK_${sdk}_OBJECT_FORMAT}" STREQUAL "WASM")
    list(APPEND WASM_SDKS ${sdk})
  endif()
endforeach()


# Gold LTO is unsupported. To prevent tests from failing when building
# with LTO, force swift runtime to compile without LTO for Linux.
add_swift_target_library(swiftImageRegistrationObjectELF
                  OBJECT_LIBRARY IS_STDLIB IS_STDLIB_CORE
                  SwiftRT-ELF-WASM.cpp
                  C_COMPILE_FLAGS ${SWIFT_RUNTIME_CORE_CXX_FLAGS}
                  C_COMPILE_FLAGS_LINUX -fno-lto
                  LINK_FLAGS ${SWIFT_RUNTIME_CORE_LINK_FLAGS}
                  TARGET_SDKS ${ELFISH_SDKS}
                  SWIFT_COMPILE_FLAGS ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
                  INSTALL_IN_COMPONENT none)
# FIXME(compnerd) this should be compiled twice, once for static and once for
# shared.  The static version should be used for building the standard library.
add_swift_target_library(swiftImageRegistrationObjectCOFF
                  OBJECT_LIBRARY IS_STDLIB IS_STDLIB_CORE
                  SwiftRT-COFF.cpp
                  C_COMPILE_FLAGS ${SWIFT_RUNTIME_CORE_CXX_FLAGS}
                  LINK_FLAGS ${SWIFT_RUNTIME_CORE_LINK_FLAGS}
                  TARGET_SDKS ${COFF_SDKS}
                  SWIFT_COMPILE_FLAGS ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
                  INSTALL_IN_COMPONENT none)

add_swift_target_library(swiftImageRegistrationObjectWASM
                  OBJECT_LIBRARY IS_STDLIB IS_STDLIB_CORE
                  SwiftRT-ELF-WASM.cpp
                  C_COMPILE_FLAGS ${SWIFT_RUNTIME_CORE_CXX_FLAGS}
                  LINK_FLAGS ${SWIFT_RUNTIME_CORE_LINK_FLAGS}
                  TARGET_SDKS ${WASM_SDKS}
                  SWIFT_COMPILE_FLAGS ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
                  INSTALL_IN_COMPONENT none)

foreach(sdk ${SWIFT_SDKS})
  foreach(arch ${SWIFT_SDK_${sdk}_ARCHITECTURES})
    set(arch_subdir "${SWIFT_SDK_${sdk}_LIB_SUBDIR}/${arch}")
    set(arch_suffix "${SWIFT_SDK_${sdk}_LIB_SUBDIR}-${arch}")

    if("${SWIFT_SDK_${sdk}_OBJECT_FORMAT}" STREQUAL "ELF" OR
       "${SWIFT_SDK_${sdk}_OBJECT_FORMAT}" STREQUAL "COFF" OR
       "${SWIFT_SDK_${sdk}_OBJECT_FORMAT}" STREQUAL "WASM")
      # TODO(compnerd) switch to the generator expression when cmake is upgraded
      # to a version which supports it.
      # set(swiftrtObject "$<TARGET_OBJECTS:swiftImageRegistrationObject${SWIFT_SDK_${sdk}_OBJECT_FORMAT}-${arch_suffix}>")
      set(swiftrtSourceName SwiftRT-${SWIFT_SDK_${sdk}_OBJECT_FORMAT}.cpp)
      if("${SWIFT_SDK_${sdk}_OBJECT_FORMAT}" STREQUAL "ELF" OR
         "${SWIFT_SDK_${sdk}_OBJECT_FORMAT}" STREQUAL "WASM")
       set(swiftrtSourceName SwiftRT-ELF-WASM.cpp)
      endif()
      set(swiftrtObject ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/swiftImageRegistrationObject${SWIFT_SDK_${sdk}_OBJECT_FORMAT}-${arch_suffix}.dir/${swiftrtSourceName}${CMAKE_C_OUTPUT_EXTENSION})

      if(sdk STREQUAL "WINDOWS")
        set(extension .obj)
      else()
        set(extension .o)
      endif()

      set(shared_runtime_registrar "${SWIFTLIB_DIR}/${arch_subdir}/swiftrt${extension}")
      set(static_runtime_registrar "${SWIFTSTATICLIB_DIR}/${arch_subdir}/swiftrt${extension}")

      add_custom_command_target(swiftImageRegistration-${arch_suffix}
                                COMMAND
                                  "${CMAKE_COMMAND}" -E copy "${swiftrtObject}" "${shared_runtime_registrar}"
                                COMMAND
                                  "${CMAKE_COMMAND}" -E copy "${swiftrtObject}" "${static_runtime_registrar}"
                                OUTPUT
                                  "${shared_runtime_registrar}"
                                  "${static_runtime_registrar}"
                                DEPENDS
                                  "${swiftrtObject}")
      if(SWIFT_BUILD_DYNAMIC_STDLIB)
        swift_install_in_component(FILES
                                     "${shared_runtime_registrar}"
                                   DESTINATION
                                     "lib/swift/${arch_subdir}"
                                   COMPONENT
                                     stdlib)
      endif()
      if(SWIFT_BUILD_STATIC_STDLIB)
        swift_install_in_component(FILES
                                     "${static_runtime_registrar}"
                                   DESTINATION
                                     "lib/swift_static/${arch_subdir}"
                                   COMPONENT
                                     stdlib)
      endif()

      add_dependencies(swift-stdlib-${arch_suffix} ${swiftImageRegistration-${arch_suffix}})

      add_custom_target(swiftImageRegistration-${arch_suffix}
                        ALL DEPENDS
                          ${swiftImageRegistration-${arch_suffix}})
      add_dependencies(stdlib swift-stdlib-${arch_suffix} swiftImageRegistration-${arch_suffix})
    endif()

    # Generate the static-stdlib-args.lnk file used by -static-stdlib option for
    # 'GenericUnix' (eg linux)
    if(SWIFT_SDK_${sdk}_OBJECT_FORMAT STREQUAL "ELF")
      string(TOLOWER "${sdk}" lowercase_sdk)
      set(libpthread -lpthread)
      set(concurrency_libs)
      set(android_libraries)
      if(sdk STREQUAL "ANDROID")
        set(android_libraries -llog)
        set(libpthread)
      elseif(SWIFT_CONCURRENCY_USES_DISPATCH)
        set(concurrency_libs "-ldispatch -lBlocksRuntime")
      endif()

      set(linkfile ${lowercase_sdk}/static-stdlib-args.lnk)
      file(WRITE "${SWIFTSTATICLIB_DIR}/${linkfile}" "
-ldl
${libpthread}
${android_libraries}
-lswiftCore
${concurrency_libs}
-lstdc++
-lm
-Xlinker -export-dynamic
-Xlinker --exclude-libs
-Xlinker ALL")

      swift_install_in_component(FILES "${SWIFTSTATICLIB_DIR}/${linkfile}"
                                 DESTINATION "lib/swift_static/${lowercase_sdk}"
                                 COMPONENT stdlib)
    endif()
  endforeach()
endforeach()
