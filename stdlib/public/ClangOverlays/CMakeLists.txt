if(NOT DEFINED SWIFT_BUILD_CLANG_OVERLAYS_SKIP_BUILTIN_FLOAT OR NOT SWIFT_BUILD_CLANG_OVERLAYS_SKIP_BUILTIN_FLOAT)
  add_swift_target_library(swift_Builtin_float
      ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES}
      IS_SDK_OVERLAY

      GYB_SOURCES
        float.swift.gyb

      SWIFT_COMPILE_FLAGS
        ${SWIFT_RUNTIME_SWIFT_COMPILE_FLAGS}
        ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
        -Xfrontend -previous-module-installname-map-file -Xfrontend "${SWIFT_SOURCE_DIR}/stdlib/linker-support/previous-module-installname.json"

      LINK_FLAGS
        ${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}

      INSTALL_IN_COMPONENT stdlib
      MACCATALYST_BUILD_FLAVOR zippered)
endif()
