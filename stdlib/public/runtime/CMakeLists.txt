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
    Casting.cpp
    CygwinPort.cpp
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
    Heap.cpp
    HeapObject.cpp
    ImageInspectionCommon.cpp
    ImageInspectionMachO.cpp
    ImageInspectionELF.cpp
    ImageInspectionCOFF.cpp
    ImageInspectionStatic.cpp
    KeyPaths.cpp
    KnownMetadata.cpp
    Metadata.cpp
    MetadataLookup.cpp
    MutexPThread.cpp
    MutexWin32.cpp
    Numeric.cpp
    Once.cpp
    Portability.cpp
    ProtocolConformance.cpp
    RefCount.cpp
    ReflectionMirror.cpp
    RuntimeInvocationsTracking.cpp
    SwiftDtoa.cpp)

# Acknowledge that the following sources are known.
set(LLVM_OPTIONAL_SOURCES
    SwiftRT-COFF.cpp
    SwiftRT-ELF.cpp
    SwiftRT-WASM.cpp
    ${swift_runtime_sources}
    ${swift_runtime_objc_sources}
    ${swift_runtime_leaks_sources})

set(swift_runtime_library_compile_flags ${swift_runtime_compile_flags})
list(APPEND swift_runtime_library_compile_flags -DswiftCore_EXPORTS)
list(APPEND swift_runtime_library_compile_flags -I${SWIFT_SOURCE_DIR}/stdlib/include/llvm/Support -I${SWIFT_SOURCE_DIR}/include)

macro(add_image_inspection_shared sdk primary_arch inspection_file linkfile_src)
  set(static_binary_lnk_file_list)
  string(TOLOWER "${sdk}" lowercase_sdk)

  # Generate the static-executable-args.lnk file used for ELF systems (eg linux)
  set(linkfile "${lowercase_sdk}/static-executable-args.lnk")
  add_custom_command_target(swift_static_binary_${sdk}_args
    COMMAND
      "${CMAKE_COMMAND}" -E copy
      "${linkfile_src}"
      "${SWIFTSTATICLIB_DIR}/${linkfile}"
    OUTPUT
      "${SWIFTSTATICLIB_DIR}/${linkfile}"
    DEPENDS
      "${linkfile_src}")

  list(APPEND static_binary_lnk_file_list ${swift_static_binary_${sdk}_args})
  swift_install_in_component(FILES "${SWIFTSTATICLIB_DIR}/${linkfile}"
                             DESTINATION "lib/swift_static/${lowercase_sdk}"
                             COMPONENT stdlib)
endmacro()

set(is_image_inspection_required)
foreach(sdk IN LISTS SWIFT_SDKS)
  set(image_inspection_shared_sdk)
  set(primary_arch)
  set(image_inspection_shared_file)
  set(linkfile_src)

  if("${SWIFT_SDK_${sdk}_OBJECT_FORMAT}" STREQUAL "ELF")
    list(APPEND ELFISH_SDKS ${sdk})
    set(linkfile_src "${SWIFT_SOURCE_DIR}/utils/static-executable-args.lnk")
  elseif("${SWIFT_SDK_${sdk}_OBJECT_FORMAT}" STREQUAL "WASM")
    set(linkfile_src "${SWIFT_SOURCE_DIR}/utils/webassembly/static-executable-args.lnk")
  endif()

  if(SWIFT_BUILD_STATIC_STDLIB AND "${sdk}" STREQUAL "LINUX")
    set(image_inspection_shared_sdk "${sdk}")
    set(image_inspection_shared_file ImageInspectionELF.cpp)
  elseif(SWIFT_BUILD_STATIC_STDLIB AND "${sdk}" STREQUAL "WASI")
    set(image_inspection_shared_sdk "${sdk}")
    set(image_inspection_shared_file ImageInspectionELF.cpp)
    # Set default arch
    set(primary_arch "wasm32")
  endif()

  if("${sdk}" STREQUAL "${SWIFT_PRIMARY_VARIANT_SDK}")
    set(primary_arch ${SWIFT_PRIMARY_VARIANT_ARCH})
  endif()

  if(NOT "${image_inspection_shared_sdk}" STREQUAL "" AND NOT "${primary_arch}" STREQUAL "")
    set(is_image_inspection_required TRUE)
    add_image_inspection_shared(${image_inspection_shared_sdk} ${primary_arch} ${image_inspection_shared_file} ${linkfile_src})
  endif()
endforeach()

if(is_image_inspection_required)
  add_custom_target(static_binary_magic ALL DEPENDS ${static_binary_lnk_file_list})
  add_dependencies(stdlib static_binary_magic)
  add_dependencies(stdlib ${static_binary_lnk_file_list})
endif()

add_swift_target_library(swiftRuntime OBJECT_LIBRARY
  ${swift_runtime_sources}
  ${swift_runtime_objc_sources}
  ${swift_runtime_leaks_sources}
  C_COMPILE_FLAGS
    ${swift_runtime_library_compile_flags}
  LINK_FLAGS ${swift_runtime_linker_flags}
  SWIFT_COMPILE_FLAGS ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
  INSTALL_IN_COMPONENT never_install)


add_swift_target_library(swiftStandaloneRuntime STATIC IS_STDLIB
  INCORPORATE_OBJECT_LIBRARIES
    swiftRuntime swiftLLVMSupport swiftDemangling swiftStdlibStubs
  INSTALL_IN_COMPONENT
    stdlib)

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


add_swift_target_library(swiftImageRegistrationObjectELF
                  OBJECT_LIBRARY IS_STDLIB IS_STDLIB_CORE
                  SwiftRT-ELF.cpp
                  C_COMPILE_FLAGS ${SWIFT_RUNTIME_CORE_CXX_FLAGS}
                  LINK_FLAGS ${SWIFT_RUNTIME_CORE_LINK_FLAGS}
                  TARGET_SDKS ${ELFISH_SDKS}
                  SWIFT_COMPILE_FLAGS ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
                  INSTALL_IN_COMPONENT none)

add_swift_target_library(swiftImageRegistrationObjectWASM
                  OBJECT_LIBRARY IS_STDLIB IS_STDLIB_CORE
                  SwiftRT-WASM.cpp
                  C_COMPILE_FLAGS ${SWIFT_RUNTIME_CORE_CXX_FLAGS}
                  LINK_FLAGS ${SWIFT_RUNTIME_CORE_LINK_FLAGS}
                  TARGET_SDKS ${WASM_SDKS}
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
      set(swiftrtObject ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/swiftImageRegistrationObject${SWIFT_SDK_${sdk}_OBJECT_FORMAT}-${arch_suffix}.dir/SwiftRT-${SWIFT_SDK_${sdk}_OBJECT_FORMAT}.cpp${CMAKE_C_OUTPUT_EXTENSION})

      if(sdk STREQUAL WINDOWS)
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

    string(TOLOWER "${sdk}" lowercase_sdk)
    # Generate the static-stdlib-args.lnk file used by -static-stdlib option for
    # 'GenericUnix' (eg linux)
    if(${SWIFT_SDK_${sdk}_OBJECT_FORMAT} STREQUAL ELF)
      if(SWIFT_${SWIFT_HOST_VARIANT_SDK}_${SWIFT_HOST_VARIANT_ARCH}_ICU_STATICLIB)
        set(libicu_i18n_a -licui18nswift)
        set(libicu_uc_a -licuucswift)
        set(libicu_data_a -licudataswift)
      else()
        find_package(ICU REQUIRED COMPONENTS uc i18n)
        get_filename_component(ICU_UC_LIBDIR "${ICU_UC_LIBRARIES}" DIRECTORY)
        get_filename_component(ICU_I18N_LIBDIR "${ICU_I18N_LIBRARIES}" DIRECTORY)

        set(libicu_i18n_a -licui18n)
        set(libicu_uc_a -licuuc)
        set(libicu_data_a)
        if(EXISTS ${ICU_I18N_LIBDIR}/libicui18n.a AND
            EXISTS ${ICU_UC_LIBDIR}/libicuuc.a AND
            EXISTS ${ICU_UC_LIBDIR}/libicudata.a)
          set(libicu_i18n_a ${ICU_I18N_LIBDIR}/libicui18n.a)
          set(libicu_uc_a ${ICU_UC_LIBDIR}/libicuuc.a)
          set(libicu_data_a ${ICU_UC_LIBDIR}/libicudata.a)
        endif()
      endif()
      set(libpthread -lpthread)
      set(android_libraries)
      if(sdk STREQUAL ANDROID)
        set(android_libraries -llog)
        set(libpthread)
      endif()

      set(linkfile ${lowercase_sdk}/static-stdlib-args.lnk)
      file(WRITE "${SWIFTSTATICLIB_DIR}/${linkfile}" "
-ldl
${libpthread}
${android_libraries}
-lswiftCore
${libicu_i18n_a}
${libicu_uc_a}
${libicu_data_a}
-lstdc++
-lm
-Xlinker -export-dynamic
-Xlinker --exclude-libs
-Xlinker ALL")

      swift_install_in_component(FILES "${SWIFTSTATICLIB_DIR}/${linkfile}"
                                 DESTINATION "lib/swift_static/${lowercase_sdk}"
                                 COMPONENT stdlib)
    elseif("${sdk}" STREQUAL "WASI")
      set(linkfile "${lowercase_sdk}/static-stdlib-args.lnk")
      file(WRITE "${SWIFTSTATICLIB_DIR}/${linkfile}" "
-ldl
-latomic
-lswiftWasiPthread
-lswiftCore
-licuuc
-licudata
-lstdc++
-lm
-Xlinker --exclude-libs
-Xlinker ALL")

      swift_install_in_component(FILES "${SWIFTSTATICLIB_DIR}/${linkfile}"
                                DESTINATION "lib/swift_static/${lowercase_sdk}"
                                COMPONENT stdlib)

      set(swift_icu_libs_wasi_list)
      set(icu_modules UC I18N DATA)
      foreach(module IN LISTS icu_modules)
        set(module_lib "${SWIFT_WASI_wasm32_ICU_${module}}")
        get_filename_component(module_lib_name ${module_lib} NAME)
        add_custom_command_target(swift_icu_${module}_${sdk}
          COMMAND
            "${CMAKE_COMMAND}" -E copy
            "${module_lib}"
            "${SWIFTSTATICLIB_DIR}/${lowercase_sdk}/${module_lib_name}"
          OUTPUT
            "${SWIFTSTATICLIB_DIR}/${lowercase_sdk}/${module_lib_name}"
          DEPENDS
            "${module_lib}")
        list(APPEND swift_icu_libs_wasi_list ${swift_icu_${module}_${sdk}})
        swift_install_in_component(FILES "${SWIFTSTATICLIB_DIR}/${lowercase_sdk}/${module_lib_name}"
                                  DESTINATION "lib/swift_static/${lowercase_sdk}"
                                  COMPONENT stdlib)
      endforeach()
      add_custom_target(swift_icu_libs_wasi ALL DEPENDS ${swift_icu_libs_wasi_list})
      add_dependencies(stdlib swift_icu_libs_wasi)
    endif()
  endforeach()
endforeach()
