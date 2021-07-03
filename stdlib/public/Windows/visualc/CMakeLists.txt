set(sources

    sal.h
    setjmp.h
    stdint.h
    vcruntime.h

    visualc.apinotes
    module.modulemap
    )

set(output_dir "${SWIFTLIB_DIR}/visualc")
set(output_dir_static "${SWIFTSTATICLIB_DIR}/visualc")

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

list(APPEND outputs "${output_dir}")

add_custom_target("copy_visualc_headers"
    DEPENDS "${outputs}"
    COMMENT "Copying visualc module to ${output_dir}")

swift_install_in_component(FILES ${sources}
    DESTINATION "lib/swift/visualc"
    COMPONENT stdlib)

if(SWIFT_BUILD_STATIC_STDLIB)
  swift_install_in_component(FILES ${sources}
      DESTINATION "lib/swift_static/visualc"
      COMPONENT stdlib)
endif()
