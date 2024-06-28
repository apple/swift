
set(SWIFT_HOST_VARIANT_SDK ANDROID CACHE STRING "")
set(SWIFT_HOST_VARIANT_ARCH i686 CACHE STRING "")

# NOTE(compnerd) disable the tools, we are trying to build just the standard
# library.
set(SWIFT_INCLUDE_TOOLS NO CACHE BOOL "")

# NOTE(compnerd) cannot build tests since the tests require the toolchain
set(SWIFT_INCLUDE_TESTS NO CACHE BOOL "")

# NOTE(compnerd) cannot build docs since that requires perl
set(SWIFT_INCLUDE_DOCS NO CACHE BOOL "")

# NOTE(compnerd) these are part of the toolchain, not the runtime.
set(SWIFT_BUILD_SOURCEKIT NO CACHE BOOL "")

# NOTE(compnerd) build with the compiler specified, not a just built compiler.
set(SWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER YES CACHE BOOL "")

set(SWIFT_SDK_ANDROID_ARCHITECTURES i686 CACHE STRING "")

# NOTE(compnerd) this is lollipop, which seems to still have decent usage.
set(SWIFT_ANDROID_API_LEVEL 21 CACHE STRING "")
