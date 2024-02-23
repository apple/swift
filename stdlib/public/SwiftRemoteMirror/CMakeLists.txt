# libswiftRemoteMirror.dylib should not have runtime dependencies; it's
# always built as a shared library.
add_swift_target_library(swiftRemoteMirror
                         SHARED DONT_EMBED_BITCODE NOSWIFTRT
                         SwiftRemoteMirror.cpp
                         LINK_LIBRARIES
                           swiftRemoteInspection
                         C_COMPILE_FLAGS
                           ${SWIFT_RUNTIME_CXX_FLAGS}
                           -DswiftRemoteMirror_EXPORTS
                         LINK_FLAGS
                           ${SWIFT_RUNTIME_LINK_FLAGS}
                         PREFIX_INCLUDE_DIRS
                           ${SWIFT_SOURCE_DIR}/include/swift/RemoteInspection/RuntimeHeaders
                         INCORPORATE_OBJECT_LIBRARIES swiftLLVMSupport
                         SWIFT_COMPILE_FLAGS ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
                         DARWIN_INSTALL_NAME_DIR "${SWIFTLIB_DARWIN_INSTALL_NAME_DIR}"
                         INSTALL_IN_COMPONENT
                           swift-remote-mirror

                         TARGET_SDKS NOT LINUX_STATIC)
