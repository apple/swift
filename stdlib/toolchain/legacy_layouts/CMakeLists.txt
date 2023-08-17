add_custom_target("copy-legacy-layouts" ALL)

function(add_layout_copying sdk arch bootstrapping)
  set(platform "${SWIFT_SDK_${sdk}_LIB_SUBDIR}")

  set(input "${SWIFT_SOURCE_DIR}/stdlib/toolchain/legacy_layouts/${platform}/layouts-${arch}.yaml")
  get_bootstrapping_path(lib_dir ${SWIFTLIB_DIR} "${bootstrapping}")
  set(output "${lib_dir}/${platform}/layouts-${arch}.yaml")

  if(NOT "${bootstrapping}" STREQUAL "")
    set(target_suffix "-bootstrapping${bootstrapping}")
  endif()
  set(copy_target "copy-legacy-layouts-${platform}-${arch}${target_suffix}")
  set(stdlib_target "swift-stdlib-${platform}-${arch}")

  if(EXISTS "${input}")
    # Copy the input file to the build directory.
    add_custom_command(
      OUTPUT "${output}"
      DEPENDS "${input}"
      COMMAND "${CMAKE_COMMAND}" -E copy "${input}" "${output}")

    # Define a target for this so that we can depend on it when
    # building Swift sources.
    add_custom_target(
      "${copy_target}"
      DEPENDS "${output}"
      SOURCES "${input}")

    # Make sure we ultimately always do this as part of building the
    # standard library. In practice we'll do this earlier if at least
    # one Swift source file has changed.
    if(TARGET "${stdlib_target}")
      add_dependencies("${stdlib_target}" "${copy_target}")
    endif()

    swift_install_in_component(
      FILES "${input}"
      DESTINATION "lib/swift/${platform}/"
      COMPONENT compiler)
  else()
    # Add a dummy target that does nothing so we can still depend on it
    # later without checking if the input files exist.
    add_custom_target("${copy_target}")
  endif()

  add_dependencies("copy-legacy-layouts" "${copy_target}")

endfunction()


foreach(sdk ${SWIFT_SDKS})
  foreach(arch ${SWIFT_SDK_${sdk}_ARCHITECTURES} ${SWIFT_SDK_${sdk}_MODULE_ARCHITECTURES})
    add_layout_copying(${sdk} ${arch} "")
  endforeach()
endforeach()

if(BOOTSTRAPPING_MODE MATCHES "BOOTSTRAPPING.*")
  # The resource dir for bootstrapping0 may be used explicitly
  # to cross compile for other architectures, so we would need
  # to have all the legacy layouts in there
  foreach(arch
          ${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_ARCHITECTURES}
          ${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_MODULE_ARCHITECTURES})
    # Bootstrapping - level 0
    add_layout_copying( ${SWIFT_HOST_VARIANT_SDK} ${arch} "0")
  endforeach()

  # Bootstrapping - level 1
  add_layout_copying( ${SWIFT_HOST_VARIANT_SDK} ${SWIFT_HOST_VARIANT_ARCH} "1")
endif()
