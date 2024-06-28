if(SWIFT_BUILD_SDK_OVERLAY)
set(swift_reflection_test_darwin_depencencies Darwin)
else()
set(swift_reflection_test_darwin_depencencies)
endif()

if (SWIFT_INCLUDE_TESTS AND SWIFT_BUILD_DYNAMIC_STDLIB)
  add_swift_target_library(swiftSwiftReflectionTest ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB
    SwiftReflectionTest.swift
    SWIFT_COMPILE_FLAGS ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
    SWIFT_COMPILE_FLAGS_LINUX -Xcc -D_GNU_SOURCE
    SWIFT_MODULE_DEPENDS_OSX ${swift_reflection_test_darwin_depencencies}
    SWIFT_MODULE_DEPENDS_IOS ${swift_reflection_test_darwin_depencencies}
    SWIFT_MODULE_DEPENDS_TVOS ${swift_reflection_test_darwin_depencencies}
    SWIFT_MODULE_DEPENDS_WATCHOS ${swift_reflection_test_darwin_depencencies}
    SWIFT_MODULE_DEPENDS_XROS ${swift_reflection_test_darwin_depencencies}
    SWIFT_MODULE_DEPENDS_ANDROID Android
    SWIFT_MODULE_DEPENDS_LINUX Glibc
    SWIFT_MODULE_DEPENDS_LINUX_STATIC Musl
    SWIFT_MODULE_DEPENDS_FREEBSD Glibc
    SWIFT_MODULE_DEPENDS_OPENBSD Glibc
    SWIFT_MODULE_DEPENDS_WINDOWS CRT
    INSTALL_IN_COMPONENT stdlib-experimental
    DARWIN_INSTALL_NAME_DIR "${SWIFT_DARWIN_STDLIB_PRIVATE_INSTALL_NAME_DIR}"

    TARGET_SDKS NOT LINUX_STATIC)

  foreach(SDK ${SWIFT_SDKS})
    if ("${SDK}" STREQUAL "LINUX_STATIC")
      continue()
    endif()

    foreach(ARCH ${SWIFT_SDK_${SDK}_ARCHITECTURES})
      set(VARIANT_SUFFIX "-${SWIFT_SDK_${SDK}_LIB_SUBDIR}-${ARCH}")
      add_dependencies("swiftSwiftReflectionTest${VARIANT_SUFFIX}"
        "swift-reflection-test${VARIANT_SUFFIX}")
    endforeach()
  endforeach()
endif()
