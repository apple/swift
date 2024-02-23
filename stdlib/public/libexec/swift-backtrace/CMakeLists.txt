# If we're building overlays, make sure we mention the ones we need.
# Otherwise, assume they already exist.
set(darwin)
set(wincrt_sdk)
set(glibc)
set(musl)

if(SWIFT_BUILD_SDK_OVERLAY)
  set(darwin Darwin)
  set(wincrt_sdk CRT WinSDK)
  set(glibc Glibc)
  set(musl Musl)
endif()

# Similarly, we only want the _Backtracing dependency if we're building
# with the stdlib.
set(backtracing)
if(SWIFT_BUILD_STDLIB AND SWIFT_ENABLE_BACKTRACING)
  set(backtracing _Backtracing)
endif()

if(NOT SWIFT_BUILD_STDLIB)
  set(BUILD_STANDALONE TRUE)
endif()

set(BACKTRACING_COMPILE_FLAGS
  "-I${SWIFT_STDLIB_SOURCE_DIR}/public/Backtracing/modules"
  "-Xcc;-I${SWIFT_SOURCE_DIR}/include"
  "-Xcc;-I${CMAKE_BINARY_DIR}/include")

set(BACKTRACING_SOURCES
  main.swift
  AnsiColor.swift
  TargetMacOS.swift
  TargetLinux.swift
  Themes.swift
  Utils.swift
  )


add_swift_target_executable(swift-backtrace BUILD_WITH_LIBEXEC
  ${BACKTRACING_SOURCES}

  SWIFT_MODULE_DEPENDS         ${backtracing}

  SWIFT_MODULE_DEPENDS_OSX          ${darwin}
  SWIFT_MODULE_DEPENDS_WINDOWS      ${wincrt_sdk}
  SWIFT_MODULE_DEPENDS_LINUX        ${glibc}
  SWIFT_MODULE_DEPENDS_LINUX_STATIC ${musl}

  INSTALL_IN_COMPONENT libexec
  COMPILE_FLAGS
    ${BACKTRACING_COMPILE_FLAGS}
    -parse-as-library

  TARGET_SDKS OSX LINUX)

set(static_target_sdks)
if(SWIFT_BUILD_STATIC_STDLIB)
  list(APPEND static_target_sdks "LINUX")
endif()
if("LINUX_STATIC" IN_LIST SWIFT_SDKS)
  list(APPEND static_target_sdks "LINUX_STATIC")
endif()
if(static_target_sdks)
  add_swift_target_executable(swift-backtrace-static BUILD_WITH_LIBEXEC
    PREFER_STATIC

    ${BACKTRACING_SOURCES}

    SWIFT_MODULE_DEPENDS         ${backtracing}

    SWIFT_MODULE_DEPENDS_OSX          ${darwin}
    SWIFT_MODULE_DEPENDS_WINDOWS      ${wincrt_sdk}
    SWIFT_MODULE_DEPENDS_LINUX        ${glibc}
    SWIFT_MODULE_DEPENDS_LINUX_STATIC ${musl}

    INSTALL_IN_COMPONENT libexec
    COMPILE_FLAGS
      ${BACKTRACING_COMPILE_FLAGS}
      -parse-as-library

    TARGET_SDKS ${static_target_sdks})
endif()
