set(swiftOnoneSupport_common_options
  IS_STDLIB

  # This file should be listed the first.  Module name is inferred from the
  # filename.
  SwiftOnoneSupport.swift

  "${SWIFT_SOURCE_DIR}/stdlib/linker-support/magic-symbols-for-install-name.c"

  SWIFT_COMPILE_FLAGS "-parse-stdlib" "-Xllvm" "-sil-inline-generics=false" "-Xfrontend" "-validate-tbd-against-ir=none" "-Xfrontend" "-check-onone-completeness" "-Xfrontend" "-disable-access-control" "${SWIFT_RUNTIME_SWIFT_COMPILE_FLAGS}" "${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}"
  LINK_FLAGS "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}")

if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND BOOTSTRAPPING_MODE STREQUAL "BOOTSTRAPPING")

  set(swiftOnoneSupport_common_bootstrapping_options
    SHARED
    ${swiftOnoneSupport_common_options}
    SDK ${SWIFT_HOST_VARIANT_SDK}
    ARCHITECTURE ${SWIFT_HOST_VARIANT_ARCH}
    INSTALL_IN_COMPONENT
      "never_install")

  # Bootstrapping - stage 0

  add_swift_target_library_single(swiftSwiftOnoneSupport-bootstrapping0 swiftSwiftOnoneSupport
    ${swiftOnoneSupport_common_bootstrapping_options}
    FILE_DEPENDS swiftCore-bootstrapping0
    BOOTSTRAPPING 0)

  # Bootstrapping - stage 1

  add_swift_target_library_single(swiftSwiftOnoneSupport-bootstrapping1 swiftSwiftOnoneSupport
    ${swiftOnoneSupport_common_bootstrapping_options}
    FILE_DEPENDS swiftCore-bootstrapping1
    BOOTSTRAPPING 1)

endif()

add_swift_target_library(swiftSwiftOnoneSupport
  ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES}
  ${swiftOnoneSupport_common_options}
  MACCATALYST_BUILD_FLAVOR "zippered"
  INSTALL_IN_COMPONENT stdlib)

if(WINDOWS IN_LIST SWIFT_SDKS)
  # When building in Debug mode, the standard library provides the symbols that
  # we need and as such SwiftOnoneSupport does not need to provide any exported
  # interfaces.  This results in the import library being elided.  However,
  # we explicitly link against the SwiftOnoneSupport library when building
  # programs in Debug mode, and need the import library to be generated even if
  # nothing is exported.  Because we will still generate the DLL, create an
  # empty import library.
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/swiftSwiftOnoneSupport.def
    "LIBRARY swiftSwiftOnoneSupport\n"
    "EXPORTS\n")
  foreach(architecture ${SWIFT_SDK_WINDOWS_ARCHITECTURES})
    target_sources(swiftSwiftOnoneSupport-windows-${architecture} PRIVATE
      ${CMAKE_CURRENT_BINARY_DIR}/swiftSwiftOnoneSupport.def)
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/swiftSwiftOnoneSupport.def
      PROPERTIES HEADER_FILE_ONLY TRUE)
  endforeach()
endif()
