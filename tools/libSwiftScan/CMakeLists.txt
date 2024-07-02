# Use an 'internal' name, this is primarily intended for SwiftDriver to import.
set(SWIFT_SCAN_LIB_NAME "_InternalSwiftScan")

set(LLVM_EXPORTED_SYMBOL_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/libSwiftScan.exports)

add_swift_host_library(libSwiftScan SHARED
  libSwiftScan.cpp
  c-include-check.c
  SwiftCaching.cpp
  HAS_SWIFT_MODULES)

if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_DARWIN_PLATFORMS)
  # Workaround for a linker crash related to autolinking: rdar://77839981
  set_property(TARGET libSwiftScan APPEND_STRING PROPERTY
               LINK_FLAGS " -lobjc ")
endif()

add_dependencies(libSwiftScan
  clang
  swiftDependencyScan)

target_link_libraries(libSwiftScan PRIVATE
    swiftDependencyScan
    swiftDriverTool
    swiftStaticMirror
    swiftRemoteInspection
    swiftCompilerModules)

set_target_properties(libSwiftScan
    PROPERTIES
    OUTPUT_NAME ${SWIFT_SCAN_LIB_NAME})

if(SWIFT_HOST_VARIANT_SDK MATCHES "LINUX|ANDROID|OPENBSD|FREEBSD" AND BOOTSTRAPPING_MODE STREQUAL "HOSTTOOLS")
  # Runtime INSTALL_RPATH are set by 'add_swift_host_library', but that expects
  # libSwiftScan be installed in 'lib'. But since it's actually installed in 'lib/swift/host',
  # we need to have correct swift runtime path to 'lib/swift/{platform}'.
  # FIXME: BUILD_RPATH and INSTALL_PATH should be different
  # FIXME: add_swift_host_library should accept 'DESTINATION' and handle installation
  # FIXME: Build this library into 'lib/swift/host/' instead of 'lib/'
  set_property(
    TARGET libSwiftScan
    APPEND PROPERTY INSTALL_RPATH "$ORIGIN/../${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR}"
  )
endif()

if(SWIFT_BUILD_SWIFT_SYNTAX)
  if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_DARWIN_PLATFORMS)
    # Ensure that we can find the shared swift-syntax libraries.
    set_property(
      TARGET libSwiftScan
      APPEND PROPERTY INSTALL_RPATH "@loader_path/swift/host/compiler")
    set_property(
      TARGET libSwiftScan
      APPEND PROPERTY INSTALL_RPATH "@loader_path/../host/compiler")
  elseif(SWIFT_HOST_VARIANT_SDK MATCHES "LINUX|ANDROID|OPENBSD|FREEBSD")
    set_property(
      TARGET libSwiftScan
      APPEND PROPERTY INSTALL_RPATH "$ORIGIN/swift/host/compiler")
    set_property(
      TARGET libSwiftScan
      APPEND PROPERTY INSTALL_RPATH "$ORIGIN/../host/compiler")
  endif()
endif()

add_llvm_symbol_exports(libSwiftScan ${LLVM_EXPORTED_SYMBOL_FILE})

# Adds -dead_strip option
add_link_opts(libSwiftScan)

add_dependencies(compiler libSwiftScan)


if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
swift_install_in_component(TARGETS libSwiftScan
  ARCHIVE DESTINATION "lib${LLVM_LIBDIR_SUFFIX}/swift/${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR}" COMPONENT compiler
  LIBRARY DESTINATION "lib${LLVM_LIBDIR_SUFFIX}/swift/${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR}" COMPONENT compiler
  RUNTIME DESTINATION "bin" COMPONENT compiler)
else()
  # On other platforms, instead install the library into 'lib/swift/host' and symlink to it from 'lib/swift/${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR}'
  swift_install_in_component(TARGETS libSwiftScan
    ARCHIVE DESTINATION "lib${LLVM_LIBDIR_SUFFIX}/swift/host" COMPONENT compiler
    LIBRARY DESTINATION "lib${LLVM_LIBDIR_SUFFIX}/swift/host" COMPONENT compiler
    RUNTIME DESTINATION "bin" COMPONENT compiler)

  # Create a symlink to previously-used path of 'lib/swift/${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR}'
  # to the new location under 'lib/swift/host' for clients of the legacy path.
  if(EXISTS ${LLVM_CMAKE_DIR}/LLVMInstallSymlink.cmake)
    set(INSTALL_SYMLINK ${LLVM_CMAKE_DIR}/LLVMInstallSymlink.cmake)
  endif()
  precondition(INSTALL_SYMLINK
               MESSAGE "LLVMInstallSymlink script must be available.")
  file(RELATIVE_PATH target_install_relative_path
       ${CMAKE_INSTALL_PREFIX}/lib${LLVM_LIBDIR_SUFFIX}/swift/${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR}
       ${CMAKE_INSTALL_PREFIX}/lib${LLVM_LIBDIR_SUFFIX}/swift/host/lib${SWIFT_SCAN_LIB_NAME}${CMAKE_SHARED_LIBRARY_SUFFIX})
  message(STATUS "Installing symlink (${target_install_relative_path}) to lib${LLVM_LIBDIR_SUFFIX}/swift/${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR}/lib${SWIFT_SCAN_LIB_NAME}${CMAKE_SHARED_LIBRARY_SUFFIX}")

  install(SCRIPT ${INSTALL_SYMLINK}
          CODE "install_symlink(lib${SWIFT_SCAN_LIB_NAME}${CMAKE_SHARED_LIBRARY_SUFFIX}
                                ${target_install_relative_path}
                                lib${LLVM_LIBDIR_SUFFIX}/swift/${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR}
                                ${SWIFT_COPY_OR_SYMLINK})"
          COMPONENT compiler)
endif()

swift_install_in_component(DIRECTORY "${SWIFT_MAIN_INCLUDE_DIR}/swift-c/DependencyScan/"
                           DESTINATION "lib${LLVM_LIBDIR_SUFFIX}/swift/${SWIFT_SCAN_LIB_NAME}"
                           COMPONENT compiler)
