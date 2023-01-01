add_subdirectory(vim)
add_subdirectory(lldb)
add_subdirectory(api_checker)

swift_install_in_component(FILES swift-mode.el
                           DESTINATION "share/emacs/site-lisp"
                           COMPONENT editor-integration)

swift_install_in_component(PROGRAMS swift-api-dump.py
                           DESTINATION bin
                           COMPONENT tools)

# We install LLVM's FileCheck, if requested.
file(TO_CMAKE_PATH "${LLVM_BUILD_BINARY_DIR}/bin/FileCheck${CMAKE_EXECUTABLE_SUFFIX}" _SWIFT_UTILS_FILECHECK)
swift_install_in_component(PROGRAMS "${_SWIFT_UTILS_FILECHECK}"
                           DESTINATION bin
                           COMPONENT llvm-toolchain-dev-tools)

# Copy over llvm-ar to use in building the Swift portions of the corelibs on
# Unix platforms.
if(NOT "${SWIFT_HOST_VARIANT_SDK}" MATCHES "OSX|WINDOWS")
  file(COPY ${LLVM_RUNTIME_OUTPUT_INTDIR}/bin/llvm-ar
    DESTINATION "${SWIFT_RUNTIME_OUTPUT_INTDIR}"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
    GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()
