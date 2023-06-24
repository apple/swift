set(libcxxshim_modulemap_target_list)
foreach(sdk ${SWIFT_SDKS})
  set(module_dir "${SWIFTLIB_DIR}/${SWIFT_SDK_${sdk}_LIB_SUBDIR}")
  set(module_dir_static "${SWIFTSTATICLIB_DIR}/${SWIFT_SDK_${sdk}_LIB_SUBDIR}")

  add_custom_command(OUTPUT ${module_dir}
                     COMMAND ${CMAKE_COMMAND} "-E" "make_directory" "${module_dir}")
  if(SWIFT_BUILD_STATIC_STDLIB)
    add_custom_command(OUTPUT ${module_dir_static}
                       COMMAND ${CMAKE_COMMAND} "-E" "make_directory" "${module_dir_static}")
  endif()

  set(outputs)
  foreach(source libcxxshim.modulemap libcxxshim.h libcxxstdlibshim.h)
    add_custom_command(OUTPUT ${module_dir}/${source}
                       DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${source}
                       COMMAND ${CMAKE_COMMAND} "-E" "copy_if_different" "${CMAKE_CURRENT_SOURCE_DIR}/${source}" "${module_dir}/${source}"
                       COMMENT "Copying ${source} to ${module_dir}")
    list(APPEND outputs "${module_dir}/${source}")

    if(SWIFT_BUILD_STATIC_STDLIB)
      add_custom_command(OUTPUT ${module_dir_static}/${source}
                         DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${source}
                         COMMAND ${CMAKE_COMMAND} "-E" "copy_if_different" "${CMAKE_CURRENT_SOURCE_DIR}/${source}" "${module_dir_static}/${source}"
                         COMMENT "Copying ${source} to ${module_dir_static}")
      list(APPEND outputs "${module_dir_static}/${source}")
    endif()
  endforeach()
  list(APPEND outputs ${module_dir})
  if(SWIFT_BUILD_STATIC_STDLIB)
    list(APPEND outputs ${module_dir_static})
  endif()

  add_custom_target(cxxshim-${sdk} ALL
                    DEPENDS ${outputs}
                    COMMENT "Copying cxxshims to ${module_dir}")
  list(APPEND libcxxshim_modulemap_target_list cxxshim-${sdk})


  swift_install_in_component(FILES libcxxshim.modulemap libcxxshim.h libcxxstdlibshim.h
                             DESTINATION "lib/swift/${SWIFT_SDK_${sdk}_LIB_SUBDIR}"
                             COMPONENT compiler)
  if(SWIFT_BUILD_STATIC_STDLIB)
    swift_install_in_component(FILES libcxxshim.modulemap libcxxshim.h libcxxstdlibshim.h
                               DESTINATION "lib/swift_static/${SWIFT_SDK_${sdk}_LIB_SUBDIR}"
                               COMPONENT compiler)
  endif()
endforeach()

add_custom_target(libcxxshim_modulemap DEPENDS ${libcxxshim_modulemap_target_list})
set_property(TARGET libcxxshim_modulemap PROPERTY FOLDER "Miscellaneous")
add_dependencies(sdk-overlay libcxxshim_modulemap)
add_dependencies(compiler libcxxshim_modulemap)
