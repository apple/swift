# Swift compiler currently assumes that the Darwin overlay is a dependency of
# CxxStdlib, and fails to build CxxStdlib if Darwin.swiftmodule in build dir
# is built with a different (older) version of the compiler. To workaround this,
# declare the Darwin overlay as a dependency of CxxStdlib.
set(swift_cxxstdlib_darwin_dependencies)
if(SWIFT_BUILD_STDLIB AND (SWIFT_BUILD_SDK_OVERLAY OR SWIFT_BUILD_TEST_SUPPORT_MODULES))
  set(swift_cxxstdlib_darwin_dependencies Darwin)
endif()

#
# C++ Standard Library Overlay.
#
# The overlay is fragile (i.e. it does not use library evolution)
# as it's not ABI stable.
add_swift_target_library(swiftCxxStdlib STATIC NO_LINK_NAME IS_STDLIB IS_SWIFT_ONLY IS_FRAGILE
    std.swift
    String.swift

    SWIFT_MODULE_DEPENDS Cxx
    SWIFT_MODULE_DEPENDS_IOS ${swift_cxxstdlib_darwin_dependencies}
    SWIFT_MODULE_DEPENDS_OSX ${swift_cxxstdlib_darwin_dependencies}
    SWIFT_MODULE_DEPENDS_TVOS ${swift_cxxstdlib_darwin_dependencies}
    SWIFT_MODULE_DEPENDS_WATCHOS ${swift_cxxstdlib_darwin_dependencies}
    SWIFT_MODULE_DEPENDS_MACCATALYST ${swift_cxxstdlib_darwin_dependencies}

    SWIFT_COMPILE_FLAGS ${SWIFT_RUNTIME_SWIFT_COMPILE_FLAGS} ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
    -Xfrontend -enable-experimental-cxx-interop
    -Xfrontend -module-interface-preserve-types-as-written

    SWIFT_COMPILE_FLAGS_LINUX
    ${SWIFT_SDK_LINUX_CXX_OVERLAY_SWIFT_COMPILE_FLAGS}

    LINK_FLAGS "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"
    TARGET_SDKS ALL_APPLE_PLATFORMS LINUX WINDOWS
    INSTALL_IN_COMPONENT compiler
    INSTALL_WITH_SHARED
    DEPENDS libstdcxx-modulemap libcxxshim_modulemap)
