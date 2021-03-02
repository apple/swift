set(swift_platform_sources
    Platform.swift
    TiocConstants.swift)
set(swift_platform_gyb_sources
    tgmath.swift.gyb)

set(darwin_depends)
if(NOT BUILD_STANDALONE)
  # This is overly conservative, but we have so few API notes files that
  # haven't migrated to the Swift repo that it's probably fine in practice.
  list(APPEND darwin_depends copy_apinotes)
endif()

set(swiftDarwin_target_sdks ALL_APPLE_PLATFORMS)
if(SWIFT_FREESTANDING_FLAVOR STREQUAL "apple")
  set(swiftDarwin_target_sdks ALL_APPLE_PLATFORMS FREESTANDING)
endif()
add_swift_target_library(swiftDarwin ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_SDK_OVERLAY
    ${swift_platform_sources}
    POSIXError.swift
    MachError.swift

    "${SWIFT_SOURCE_DIR}/stdlib/linker-support/magic-symbols-for-install-name.c"

    GYB_SOURCES
      ${swift_platform_gyb_sources}
      Darwin.swift.gyb

    SWIFT_COMPILE_FLAGS
      ${SWIFT_RUNTIME_SWIFT_COMPILE_FLAGS}
      ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
      -Xfrontend -disable-objc-attr-requires-foundation-module
    LINK_FLAGS "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"
    TARGET_SDKS "${swiftDarwin_target_sdks}"
    INSTALL_IN_COMPONENT sdk-overlay

    DEPENDS ${darwin_depends})

set(swiftGlibc_target_sdks ANDROID CYGWIN FREEBSD OPENBSD LINUX HAIKU)
if(SWIFT_FREESTANDING_FLAVOR STREQUAL "linux")
  set(swiftGlibc_target_sdks ANDROID CYGWIN FREEBSD OPENBSD LINUX HAIKU FREESTANDING)
endif()
add_swift_target_library(swiftGlibc ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_SDK_OVERLAY
    ${swift_platform_sources}
    POSIXError.swift

    GYB_SOURCES
      ${swift_platform_gyb_sources}
      Glibc.swift.gyb

    SWIFT_COMPILE_FLAGS
      ${SWIFT_RUNTIME_SWIFT_COMPILE_FLAGS}
      ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
    LINK_FLAGS "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"
    TARGET_SDKS "${swiftGlibc_target_sdks}"
    INSTALL_IN_COMPONENT sdk-overlay
    DEPENDS glibc_modulemap)

add_swift_target_library(swiftCRT ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_SDK_OVERLAY
    ucrt.swift
    ${swift_platform_sources}
    POSIXError.swift

    GYB_SOURCES
      ${swift_platform_gyb_sources}

    SWIFT_COMPILE_FLAGS
      ${SWIFT_RUNTIME_SWIFT_COMPILE_FLAGS}
      ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
      -Xcc -D_USE_MATH_DEFINES
    LINK_FLAGS "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"
    TARGET_SDKS WINDOWS
    INSTALL_IN_COMPONENT sdk-overlay)

set(glibc_modulemap_target_list)
foreach(sdk ${SWIFT_SDKS})
  if(NOT "${sdk}" STREQUAL "LINUX" AND
     NOT "${sdk}" STREQUAL "FREEBSD" AND
     NOT "${sdk}" STREQUAL "OPENBSD" AND
     NOT "${sdk}" STREQUAL "ANDROID" AND
     NOT "${sdk}" STREQUAL "CYGWIN" AND
     NOT "${sdk}" STREQUAL "HAIKU")
    continue()
  endif()

  foreach(arch ${SWIFT_SDK_${sdk}_ARCHITECTURES})
    set(arch_subdir "${SWIFT_SDK_${sdk}_LIB_SUBDIR}/${arch}")
    set(module_dir "${SWIFTLIB_DIR}/${arch_subdir}")
    set(module_dir_static "${SWIFTSTATICLIB_DIR}/${arch_subdir}")

    if(${sdk} STREQUAL ANDROID)
      set(glibc_modulemap_source "bionic.modulemap.gyb")
    elseif(${sdk} STREQUAL OPENBSD)
      set(glibc_modulemap_source "libc-openbsd.modulemap.gyb")
    else()
      set(glibc_modulemap_source "glibc.modulemap.gyb")
    endif()
    set(glibc_modulemap_out "${module_dir}/glibc.modulemap")
    set(glibc_modulemap_out_static "${module_dir_static}/glibc.modulemap")

    # Configure the module map based on the target. Each platform needs to
    # reference different headers, based on what's available in their glibc.
    # This is the 'glibc.modulemap' in the 'resource-dir', so
    # it's the one we'll look at during the build process.
    handle_gyb_source_single(glibc_modulemap_target
        SOURCE "${glibc_modulemap_source}"
        OUTPUT "${glibc_modulemap_out}"
        FLAGS
            "-DCMAKE_SDK=${sdk}"
            "-DGLIBC_INCLUDE_PATH=${SWIFT_SDK_${sdk}_ARCH_${arch}_LIBC_INCLUDE_DIRECTORY}"
            "-DGLIBC_ARCH_INCLUDE_PATH=${SWIFT_SDK_${sdk}_ARCH_${arch}_LIBC_ARCHITECTURE_INCLUDE_DIRECTORY}")

    list(APPEND glibc_modulemap_target_list ${glibc_modulemap_target})

    if(SWIFT_BUILD_STATIC_STDLIB)
      add_custom_command_target(
        copy_glibc_modulemap_static
        COMMAND
          "${CMAKE_COMMAND}" "-E" "make_directory" ${module_dir_static}
        COMMAND
          "${CMAKE_COMMAND}" "-E" "copy" ${glibc_modulemap_out} ${glibc_modulemap_out_static}
        OUTPUT ${glibc_modulemap_out_static}
        DEPENDS
          "${glibc_modulemap_target}"
        COMMENT "Copying Glibc modulemap to static resources")

      list(APPEND glibc_modulemap_target_list ${copy_glibc_modulemap_static})
    endif()

    set(glibc_header_out "${module_dir}/SwiftGlibc.h")
    handle_gyb_source_single(glibc_header_target
        SOURCE "SwiftGlibc.h.gyb"
        OUTPUT "${glibc_header_out}"
        FLAGS "-DCMAKE_SDK=${sdk}")
    list(APPEND glibc_modulemap_target_list ${glibc_header_target})

    # If this SDK is a target for a non-native host, except if it's for Android
    # with its own native sysroot, create a native modulemap without a sysroot
    # prefix. This is the one we'll install instead.
    if(NOT "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_ARCH_${arch}_PATH}" STREQUAL "/" AND
       NOT (${sdk} STREQUAL ANDROID AND NOT "${SWIFT_ANDROID_NATIVE_SYSROOT}" STREQUAL ""))
      set(glibc_sysroot_relative_modulemap_out "${module_dir}/sysroot-relative-modulemaps/glibc.modulemap")

      string(REPLACE "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_ARCH_${arch}_PATH}"
        "" absolute_libc_include_path "${SWIFT_SDK_${sdk}_ARCH_${arch}_LIBC_INCLUDE_DIRECTORY}")
      string(REPLACE "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_ARCH_${arch}_PATH}"
        "" absolute_libc_arch_include_path ${SWIFT_SDK_${sdk}_ARCH_${arch}_LIBC_ARCHITECTURE_INCLUDE_DIRECTORY})

      handle_gyb_source_single(glibc_modulemap_native_target
        SOURCE "${glibc_modulemap_source}"
        OUTPUT "${glibc_sysroot_relative_modulemap_out}"
        FLAGS "-DCMAKE_SDK=${sdk}"
              "-DGLIBC_INCLUDE_PATH=${absolute_libc_include_path}"
              "-DGLIBC_ARCH_INCLUDE_PATH=${absolute_libc_arch_include_path}")

      list(APPEND glibc_modulemap_target_list ${glibc_modulemap_native_target})
      set(glibc_modulemap_out ${glibc_sysroot_relative_modulemap_out})
    endif()

    # FIXME: When SDK is a cross-compile target (SDK != Host), the generated
    #        modulemap will be relative to the Host, with hardcoded paths.
    #        It is not relocatable to the target platform itself.
    #        This affects any cross-comipled targets that use glibc.modulemap.

    swift_install_in_component(FILES "${glibc_modulemap_out}"
                               DESTINATION "lib/swift/${arch_subdir}"
                               COMPONENT sdk-overlay)
    swift_install_in_component(FILES "${glibc_header_out}"
                               DESTINATION "lib/swift/${arch_subdir}"
                               COMPONENT sdk-overlay)

    if(SWIFT_BUILD_STATIC_STDLIB)
      swift_install_in_component(FILES "${glibc_modulemap_out}"
                                 DESTINATION "lib/swift_static/${arch_subdir}"
                                 COMPONENT sdk-overlay)
      swift_install_in_component(FILES "${glibc_header_out}"
                                 DESTINATION "lib/swift_static/${arch_subdir}"
                                 COMPONENT sdk-overlay)
    endif()
  endforeach()
endforeach()
add_custom_target(glibc_modulemap DEPENDS ${glibc_modulemap_target_list})
set_property(TARGET glibc_modulemap PROPERTY FOLDER "Miscellaneous")
add_dependencies(sdk-overlay glibc_modulemap)
