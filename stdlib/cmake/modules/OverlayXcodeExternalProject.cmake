#===--- OverlayXcodeExternalProject.cmake - build overlays with Xcode   --===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
#===----------------------------------------------------------------------===#
include(ExternalProject)

function(build_corefoundation_and_foundation_overlays)
  set(options)
  set(oneValueArgs "FOUNDATION_OVERLAYS_SOURCE_DIR")
  set(multiValueArgs "TARGET_SDKS" "ADDITIONAL_XCODEBUILD_ARGUMENTS")

  cmake_parse_arguments(BCFFO "${options}" "${oneValueArgs}"
                                "${multiValueArgs}" ${ARGN} )
 
  if(DEFINED BCFFO_TARGET_SDKS AND
      BCFFO_TARGET_SDKS AND
      DEFINED BCFFO_FOUNDATION_OVERLAYS_SOURCE_DIR AND
      BCFFO_FOUNDATION_OVERLAYS_SOURCE_DIR)
    add_overlay_xcode_project(CoreFoundation
      SOURCE_DIR ${BCFFO_FOUNDATION_OVERLAYS_SOURCE_DIR}
       BUILD_TARGET CoreFoundation-swiftoverlay
       ADDITIONAL_XCODEBUILD_ARGUMENTS ${BCFFO_ADDITIONAL_XCODEBUILD_ARGS}
       TARGET_SDKS ${BCFFO_TARGET_SDKS})
    add_overlay_xcode_project(Foundation
      SOURCE_DIR ${BCFFO_FOUNDATION_OVERLAYS_SOURCE_DIR}
       BUILD_TARGET Foundation-swiftoverlay
       ADDITIONAL_XCODEBUILD_ARGUMENTS ${BCFFO_ADDITIONAL_XCODEBUILD_ARGS}
       TARGET_SDKS ${BCFFO_TARGET_SDKS}
       DEPENDS CoreFoundationOverlay)
  endif()

endfunction()

# Build an overlay project from Xcode sources
# for given SDKs. For now tailored for
# Foundation and CoreFoundation
function(add_overlay_xcode_project overlay)
  set(options)
  set(oneValueArgs "SOURCE_DIR" "BUILD_TARGET")
  set(multiValueArgs "DEPENDS" "TARGET_SDKS" "ADDITIONAL_XCODEBUILD_ARGUMENTS")

  cmake_parse_arguments(AOXP "${options}" "${oneValueArgs}"
                                "${multiValueArgs}" ${ARGN} )

  foreach(sdk ${AOXP_TARGET_SDKS})
    add_overlay_xcode_project_single(
      ${overlay}
      SOURCE_DIR ${AOXP_SOURCE_DIR}
      BUILD_TARGET ${AOXP_BUILD_TARGET}
      DEPENDS ${AOXP_DEPENDS}
      ADDITIONAL_XCODEBUILD_ARGUMENTS ${AOXP_ADDITIONAL_XCODEBUILD_ARGUMENTS}
      TARGET_SDK ${sdk})
  endforeach()
endfunction()

function(add_overlay_xcode_project_single overlay)
  set(options)
  set(oneValueArgs "SOURCE_DIR" "BUILD_TARGET" "TARGET_SDK")
  set(multiValueArgs "DEPENDS" "ADDITIONAL_XCODEBUILD_ARGUMENTS")

  cmake_parse_arguments(AOXP "${options}" "${oneValueArgs}"
                                "${multiValueArgs}" ${ARGN} )

  set(sdk ${AOXP_TARGET_SDK})
  set(sdk_name ${SWIFT_SDK_${sdk}_LIB_SUBDIR})
  set(sdk_path ${SWIFT_SDK_${sdk}_PATH})
  string(JOIN " " joined_sdk_archs ${SWIFT_SDK_${sdk}_ARCHITECTURES})
  set(temp_install_subpath "usr/lib/swift")

  set(dependencies swiftCore ${AOXP_DEPENDS})
  list(TRANSFORM dependencies APPEND "-${sdk_name}")
  set(xcode_overlay_target_name ${overlay}Overlay-${sdk_name})

  ExternalProject_Add(${xcode_overlay_target_name}
    SOURCE_DIR ${AOXP_SOURCE_DIR}
    INSTALL_DIR  ${SWIFTLIB_DIR}/${sdk_name}
    CONFIGURE_COMMAND ""
    # let update-checkout and xcodebuild
    # figure out if we have to rebuild
    BUILD_ALWAYS 1
    BUILD_IN_SOURCE TRUE
    BUILD_COMMAND xcodebuild install
    -target ${AOXP_BUILD_TARGET}
    -sdk ${sdk_path}
    SYMROOT=<TMP_DIR>
    OBJROOT=<TMP_DIR>
    DSTROOT=<TMP_DIR>
    SWIFT_EXEC=${SWIFT_NATIVE_SWIFT_TOOLS_PATH}/swiftc
    MACOSX_DEPLOYMENT_TARGET=${SWIFT_DARWIN_DEPLOYMENT_VERSION_OSX}
    IPHONEOS_DEPLOYMENT_TARGET=${SWIFT_DARWIN_DEPLOYMENT_VERSION_IOS}
    TVOS_DEPLOYMENT_TARGET=${SWIFT_DARWIN_DEPLOYMENT_VERSION_TVOS}
    WATCHOS_DEPLOYMENT_TARGET=${SWIFT_DARWIN_DEPLOYMENT_VERSION_WATCHOS}
    ARCHS=${joined_sdk_archs}
    ${AOXP_ADDITIONAL_XCODEBUILD_ARGUMENTS}
    # This should have been the install command,
    # but need to fold into the build one
    # to be sure the dependencies are considered correctly
    COMMAND ditto <TMP_DIR>/${temp_install_subpath} <INSTALL_DIR>
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS <INSTALL_DIR>/${overlay}.swiftmodule
    <INSTALL_DIR>/libswift${overlay}.dylib
    EXCLUDE_FROM_ALL TRUE
    DEPENDS ${dependencies})
endfunction()

function(add_stdlib_dependencies_to)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs "OVERLAYS" "TARGET_SDKS")

  cmake_parse_arguments(ASDT "${options}" "${oneValueArgs}"
                                "${multiValueArgs}" ${ARGN} )

  foreach(sdk ${ASDT_TARGET_SDKS})
    set(sdk_name ${SWIFT_SDK_${sdk}_LIB_SUBDIR})
    foreach(overlay ${ASDT_OVERLAYS})
      set(xcode_overlay_target_name ${overlay}Overlay-${sdk_name})
      add_dependencies(sdk-overlay ${xcode_overlay_target_name})
      foreach(arch ${SWIFT_SDK_${sdk}_ARCHITECTURES})
        set(variant_suffix "${sdk_name}-${arch}")
        add_dependencies(swift-stdlib-${variant_suffix} ${xcode_overlay_target_name})
      endforeach()
    endforeach()
  endforeach()
endfunction()

function(add_overlay_dependencies_to target)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs "OVERLAYS" "TARGET_SDKS")

  cmake_parse_arguments(AODT "${options}" "${oneValueArgs}"
                                "${multiValueArgs}" ${ARGN} )

  foreach(sdk ${AODT_TARGET_SDKS})
    set(sdk_name ${SWIFT_SDK_${sdk}_LIB_SUBDIR})
    foreach(overlay ${AODT_OVERLAYS})
      # We added this check so not to force the user to explicitly
      # check if the overlays are built or not 
      if(TARGET ${overlay}Overlay-${sdk_name})
        add_dependencies(${target}-${sdk_name} ${overlay}Overlay-${sdk_name})
      else()
        message(WARNING "${overlay}Overlay-${sdk_name} does not exist, assuming that's on purpose and not adding as dependencies for ${target}-${sdk_name}")
      endif()
    endforeach()
  endforeach()
endfunction()
