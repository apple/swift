set(SUPPORTED_IOS_ARCHS "arm64;arm64e")
set(SUPPORTED_IOS_SIMULATOR_ARCHS "x86_64;arm64")
set(SUPPORTED_TVOS_ARCHS "arm64")
set(SUPPORTED_TVOS_SIMULATOR_ARCHS "x86_64;arm64")
set(SUPPORTED_WATCHOS_ARCHS "armv7k;arm64_32")
set(SUPPORTED_WATCHOS_SIMULATOR_ARCHS "i386;x86_64;arm64")
set(SUPPORTED_OSX_ARCHS "x86_64;arm64")
set(SUPPORTED_XROS_ARCHS "arm64;arm64e")
set(SUPPORTED_XROS_SIMULATOR_ARCHS "arm64")

is_sdk_requested(OSX swift_build_osx)
if(swift_build_osx)
  configure_sdk_darwin(
      OSX "OS X" "${SWIFT_DARWIN_DEPLOYMENT_VERSION_OSX}"
      macosx macosx macos "${SUPPORTED_OSX_ARCHS}")
  configure_target_variant(OSX-DA "OS X Debug+Asserts"   OSX DA "Debug+Asserts")
  configure_target_variant(OSX-RA "OS X Release+Asserts" OSX RA "Release+Asserts")
  configure_target_variant(OSX-R  "OS X Release"         OSX R  "Release")
endif()

is_sdk_requested(FREESTANDING swift_build_freestanding)
if(swift_build_freestanding AND (SWIFT_FREESTANDING_FLAVOR STREQUAL "apple"))
  set(SWIFT_FREESTANDING_SDK "" CACHE STRING
      "Which SDK to use when building the FREESTANDING stdlib")
  set(SWIFT_FREESTANDING_TRIPLE_NAME "" CACHE STRING
      "Which triple name (e.g. 'none-macho') to use when building the FREESTANDING stdlib")
  set(SWIFT_FREESTANDING_MODULE_NAME "" CACHE STRING
      "Which .swiftmodule name (e.g. 'freestanding') to use when building the FREESTANDING stdlib")
  set(SWIFT_FREESTANDING_ARCHS "" CACHE STRING
      "Which architectures to build when building the FREESTANDING stdlib")
  configure_sdk_darwin(
      FREESTANDING "FREESTANDING" ""
      "${SWIFT_FREESTANDING_SDK}"
      "${SWIFT_FREESTANDING_TRIPLE_NAME}" "${SWIFT_FREESTANDING_MODULE_NAME}" "${SWIFT_FREESTANDING_ARCHS}")
  set(SWIFT_SDK_FREESTANDING_LIB_SUBDIR "freestanding")
  configure_target_variant(FREESTANDING-DA "FREESTANDING Debug+Asserts"   FREESTANDING DA "Debug+Asserts")
  configure_target_variant(FREESTANDING-RA "FREESTANDING Release+Asserts" FREESTANDING RA "Release+Asserts")
  configure_target_variant(FREESTANDING-R  "FREESTANDING Release"         FREESTANDING R  "Release")
  configure_target_variant(FREESTANDING-S  "FREESTANDING MinSizeRelease"  FREESTANDING S  "MinSizeRelease")

  set(SWIFT_FREESTANDING_TEST_DEPENDENCIES "Darwin")
endif()

# Compatible cross-compile SDKS for Darwin OSes: IOS, IOS_SIMULATOR, TVOS,
#   TVOS_SIMULATOR, WATCHOS, WATCHOS_SIMULATOR, XROS, XROS_SIMULATOR
#   (archs hardcoded below).

is_sdk_requested(IOS swift_build_ios)
if(swift_build_ios)
  configure_sdk_darwin(
      IOS "iOS" "${SWIFT_DARWIN_DEPLOYMENT_VERSION_IOS}"
      iphoneos ios ios "${SUPPORTED_IOS_ARCHS}")
  configure_target_variant(IOS-DA "iOS Debug+Asserts"   IOS DA "Debug+Asserts")
  configure_target_variant(IOS-RA "iOS Release+Asserts" IOS RA "Release+Asserts")
  configure_target_variant(IOS-R  "iOS Release"         IOS R "Release")
endif()

is_sdk_requested(IOS_SIMULATOR swift_build_ios_simulator)
if(swift_build_ios_simulator)
  configure_sdk_darwin(
      IOS_SIMULATOR "iOS Simulator" "${SWIFT_DARWIN_DEPLOYMENT_VERSION_IOS}"
      iphonesimulator ios ios-simulator
      "${SUPPORTED_IOS_SIMULATOR_ARCHS}")
  configure_target_variant(
      IOS_SIMULATOR-DA "iOS Debug+Asserts"   IOS_SIMULATOR DA "Debug+Asserts")
  configure_target_variant(
      IOS_SIMULATOR-RA "iOS Release+Asserts" IOS_SIMULATOR RA "Release+Asserts")
  configure_target_variant(
      IOS_SIMULATOR-R  "iOS Release"         IOS_SIMULATOR R "Release")
endif()

is_sdk_requested(TVOS swift_build_tvos)
if(swift_build_tvos)
  configure_sdk_darwin(
      TVOS "tvOS" "${SWIFT_DARWIN_DEPLOYMENT_VERSION_TVOS}"
      appletvos tvos tvos "${SUPPORTED_TVOS_ARCHS}")
  configure_target_variant(TVOS-DA "tvOS Debug+Asserts"   TVOS DA "Debug+Asserts")
  configure_target_variant(TVOS-RA "tvOS Release+Asserts" TVOS RA "Release+Asserts")
  configure_target_variant(TVOS-R  "tvOS Release"         TVOS R "Release")
endif()

is_sdk_requested(TVOS_SIMULATOR swift_build_tvos_simulator)
if(swift_build_tvos_simulator)
  configure_sdk_darwin(
      TVOS_SIMULATOR "tvOS Simulator" "${SWIFT_DARWIN_DEPLOYMENT_VERSION_TVOS}"
      appletvsimulator tvos tvos-simulator
      "${SUPPORTED_TVOS_SIMULATOR_ARCHS}")
  configure_target_variant(
    TVOS_SIMULATOR-DA "tvOS Debug+Asserts"   TVOS_SIMULATOR DA "Debug+Asserts")
  configure_target_variant(
    TVOS_SIMULATOR-RA "tvOS Release+Asserts" TVOS_SIMULATOR RA "Release+Asserts")
  configure_target_variant(
    TVOS_SIMULATOR-R  "tvOS Release"         TVOS_SIMULATOR R "Release")
endif()

is_sdk_requested(WATCHOS swift_build_watchos)
if(swift_build_watchos)
  configure_sdk_darwin(
      WATCHOS "watchOS" "${SWIFT_DARWIN_DEPLOYMENT_VERSION_WATCHOS}"
      watchos watchos watchos "${SUPPORTED_WATCHOS_ARCHS}")
  configure_target_variant(WATCHOS-DA "watchOS Debug+Asserts"   WATCHOS DA "Debug+Asserts")
  configure_target_variant(WATCHOS-RA "watchOS Release+Asserts" WATCHOS RA "Release+Asserts")
  configure_target_variant(WATCHOS-R  "watchOS Release"         WATCHOS R "Release")
endif()

is_sdk_requested(WATCHOS_SIMULATOR swift_build_watchos_simulator)
if(swift_build_watchos_simulator)
  configure_sdk_darwin(
      WATCHOS_SIMULATOR "watchOS Simulator" "${SWIFT_DARWIN_DEPLOYMENT_VERSION_WATCHOS}"
      watchsimulator watchos watchos-simulator
      "${SUPPORTED_WATCHOS_SIMULATOR_ARCHS}")
  configure_target_variant(WATCHOS_SIMULATOR-DA "watchOS Debug+Asserts"   WATCHOS_SIMULATOR DA "Debug+Asserts")
  configure_target_variant(WATCHOS_SIMULATOR-RA "watchOS Release+Asserts" WATCHOS_SIMULATOR RA "Release+Asserts")
  configure_target_variant(WATCHOS_SIMULATOR-R  "watchOS Release"         WATCHOS_SIMULATOR R "Release")
endif()

is_sdk_requested(XROS swift_build_xros)
if(swift_build_xros)
  configure_sdk_darwin(
      XROS "xrOS" "${SWIFT_DARWIN_DEPLOYMENT_VERSION_XROS}"
      xros xros xros "${SUPPORTED_XROS_ARCHS}")
  configure_target_variant(XROS-DA "xrOS Debug+Asserts"   XROS DA "Debug+Asserts")
  configure_target_variant(XROS-RA "xrOS Release+Asserts" XROS RA "Release+Asserts")
  configure_target_variant(XROS-R  "xrOS Release"         XROS R "Release")
endif()

is_sdk_requested(XROS_SIMULATOR swift_build_xros_simulator)
if(swift_build_xros_simulator)
  configure_sdk_darwin(
      XROS_SIMULATOR "xrOS Simulator" "${SWIFT_DARWIN_DEPLOYMENT_VERSION_XROS}"
      xrsimulator xros xros-simulator
      "${SUPPORTED_XROS_SIMULATOR_ARCHS}")

  configure_target_variant(
      XROS_SIMULATOR-DA "xrOS Simulator Debug+Asserts"   XROS_SIMULATOR DA "Debug+Asserts")
  configure_target_variant(
      XROS_SIMULATOR-RA "xrOS Simulator Release+Asserts" XROS_SIMULATOR RA "Release+Asserts")
  configure_target_variant(
      XROS_SIMULATOR-R  "xrOS Simulator Release"         XROS_SIMULATOR R "Release")
endif()
