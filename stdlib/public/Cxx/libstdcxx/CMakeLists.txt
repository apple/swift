set(libstdcxx_modulemap_target_list)
foreach(sdk ${SWIFT_SDKS})
  if(NOT sdk IN_LIST SWIFT_LIBSTDCXX_PLATFORMS)
    continue()
  endif()

  set(module_dir "${SWIFTLIB_DIR}/${SWIFT_SDK_${sdk}_LIB_SUBDIR}")
  set(module_dir_static "${SWIFTSTATICLIB_DIR}/${SWIFT_SDK_${sdk}_LIB_SUBDIR}")

  set(libstdcxx_header "libstdcxx.h")
  set(libstdcxx_header_out "${module_dir}/libstdcxx.h")
  set(libstdcxx_header_out_static "${module_dir_static}/libstdcxx.h")
  set(libstdcxx_modulemap "libstdcxx.modulemap")
  set(libstdcxx_modulemap_out "${module_dir}/libstdcxx.modulemap")
  set(libstdcxx_modulemap_out_static "${module_dir_static}/libstdcxx.modulemap")

  add_custom_command_target(
      copy_libstdcxx_modulemap
      COMMAND
      "${CMAKE_COMMAND}" "-E" "make_directory" ${module_dir}
      COMMAND
      "${CMAKE_COMMAND}" "-E" "copy_if_different" "${CMAKE_CURRENT_SOURCE_DIR}/${libstdcxx_modulemap}" "${libstdcxx_modulemap_out}"
      OUTPUT ${libstdcxx_modulemap_out}
      DEPENDS ${libstdcxx_modulemap}
      COMMENT "Copying libstdcxx modulemap to resources")
  list(APPEND libstdcxx_modulemap_target_list ${copy_libstdcxx_modulemap})
  add_dependencies(swift-stdlib-${SWIFT_SDK_${sdk}_LIB_SUBDIR} ${copy_libstdcxx_modulemap})

  add_custom_command_target(
      copy_libstdcxx_header
      COMMAND
      "${CMAKE_COMMAND}" "-E" "make_directory" ${module_dir}
      COMMAND
      "${CMAKE_COMMAND}" "-E" "copy_if_different" "${CMAKE_CURRENT_SOURCE_DIR}/${libstdcxx_header}" "${libstdcxx_header_out}"
      OUTPUT ${libstdcxx_header_out}
      DEPENDS ${libstdcxx_header}
      COMMENT "Copying libstdcxx header to resources")
  list(APPEND libstdcxx_modulemap_target_list ${copy_libstdcxx_header})
  add_dependencies(swift-stdlib-${SWIFT_SDK_${sdk}_LIB_SUBDIR} ${copy_libstdcxx_header})

  if(SWIFT_BUILD_STATIC_STDLIB)
    add_custom_command_target(
        copy_libstdcxx_modulemap_static
        COMMAND
        "${CMAKE_COMMAND}" "-E" "make_directory" ${module_dir_static}
        COMMAND
        "${CMAKE_COMMAND}" "-E" "copy_if_different"
        "${libstdcxx_modulemap_out}" "${libstdcxx_modulemap_out_static}"
        OUTPUT ${libstdcxx_modulemap_out_static}
        DEPENDS ${copy_libstdcxx_modulemap}
        COMMENT "Copying libstdcxx modulemap to static resources")
    list(APPEND libstdcxx_modulemap_target_list ${copy_libstdcxx_modulemap_static})
    add_dependencies(swift-stdlib-${SWIFT_SDK_${sdk}_LIB_SUBDIR} ${copy_libstdcxx_modulemap_static})

    add_custom_command_target(
        copy_libstdcxx_header_static
        COMMAND
        "${CMAKE_COMMAND}" "-E" "make_directory" ${module_dir_static}
        COMMAND
        "${CMAKE_COMMAND}" "-E" "copy_if_different"
        "${libstdcxx_header_out}" "${libstdcxx_header_out_static}"
        OUTPUT ${libstdcxx_header_out_static}
        DEPENDS ${copy_libstdcxx_header}
        COMMENT "Copying libstdcxx header to static resources")
    list(APPEND libstdcxx_modulemap_target_list ${copy_libstdcxx_header_static})
    add_dependencies(swift-stdlib-${SWIFT_SDK_${sdk}_LIB_SUBDIR} ${copy_libstdcxx_header_static})
  endif()

  swift_install_in_component(FILES "${libstdcxx_modulemap_out}"
      DESTINATION "lib/swift/${SWIFT_SDK_${sdk}_LIB_SUBDIR}"
      COMPONENT sdk-overlay)
  swift_install_in_component(FILES "${libstdcxx_header_out}"
      DESTINATION "lib/swift/${SWIFT_SDK_${sdk}_LIB_SUBDIR}"
      COMPONENT sdk-overlay)

  if(SWIFT_BUILD_STATIC_STDLIB)
    swift_install_in_component(FILES "${libstdcxx_modulemap_out_static}"
        DESTINATION "lib/swift_static/${SWIFT_SDK_${sdk}_LIB_SUBDIR}"
        COMPONENT sdk-overlay)
    swift_install_in_component(FILES "${libstdcxx_header_out_static}"
        DESTINATION "lib/swift_static/${SWIFT_SDK_${sdk}_LIB_SUBDIR}"
        COMPONENT sdk-overlay)
  endif()

  if(BOOTSTRAPPING_MODE MATCHES "BOOTSTRAPPING.*")
    foreach(bootstrapping "0" "1")
      get_bootstrapping_path(bootstrapping_dir ${module_dir} ${bootstrapping})
      set(libstdcxx_modulemap_out_bootstrapping "${bootstrapping_dir}/libstdcxx.modulemap")
      set(libstdcxx_header_out_bootstrapping "${bootstrapping_dir}/libstdcxx.h")

      add_custom_command_target(unused_var
          COMMAND
          "${CMAKE_COMMAND}" "-E" "make_directory" "${bootstrapping_dir}"
          COMMAND
          "${CMAKE_COMMAND}" "-E" "copy_if_different"
          "${CMAKE_CURRENT_SOURCE_DIR}/${libstdcxx_modulemap}" "${libstdcxx_modulemap_out_bootstrapping}"

          CUSTOM_TARGET_NAME "copy-libstdcxx-modulemap-bootstrapping${bootstrapping}"
          OUTPUT "${libstdcxx_modulemap_out_bootstrapping}"
          DEPENDS ${libstdcxx_modulemap}
          COMMENT "Copying libstdcxx modulemap to resources for bootstrapping${bootstrapping}")

      add_custom_command_target(unused_var
          COMMAND
          "${CMAKE_COMMAND}" "-E" "make_directory" "${bootstrapping_dir}"
          COMMAND
          "${CMAKE_COMMAND}" "-E" "copy_if_different"
          "${CMAKE_CURRENT_SOURCE_DIR}/${libstdcxx_header}" "${libstdcxx_header_out_bootstrapping}"

          CUSTOM_TARGET_NAME "copy-libstdcxx-header-bootstrapping${bootstrapping}"
          OUTPUT "${libstdcxx_header_out_bootstrapping}"
          DEPENDS ${libstdcxx_header}
          COMMENT "Copying libstdcxx header to resources for bootstrapping${bootstrapping}")
    endforeach()
  endif()
endforeach()
add_custom_target(libstdcxx-modulemap DEPENDS ${libstdcxx_modulemap_target_list})
set_property(TARGET libstdcxx-modulemap PROPERTY FOLDER "Miscellaneous")
add_dependencies(sdk-overlay libstdcxx-modulemap)
