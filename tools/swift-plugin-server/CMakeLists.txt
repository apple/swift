if (SWIFT_BUILD_SWIFT_SYNTAX)
  add_pure_swift_host_tool(swift-plugin-server
    Sources/swift-plugin-server/swift-plugin-server.swift
    SWIFT_COMPONENT
      compiler
    SWIFT_DEPENDENCIES
      SwiftCompilerPluginMessageHandling
      SwiftLibraryPluginProvider
  )

  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${SWIFT_HOST_LIBRARIES_DEST_DIR}")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${SWIFT_HOST_LIBRARIES_DEST_DIR}")
  add_pure_swift_host_library(SwiftInProcPluginServer SHARED
    Sources/SwiftInProcPluginServer/InProcPluginServer.swift
    SWIFT_DEPENDENCIES
      SwiftCompilerPluginMessageHandling
      SwiftLibraryPluginProvider
  )

  if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_DARWIN_PLATFORMS)
    set_property(TARGET SwiftInProcPluginServer
      APPEND PROPERTY INSTALL_RPATH
      "@loader_path")
  elseif(SWIFT_HOST_VARIANT_SDK MATCHES "LINUX|ANDROID|OPENBSD|FREEBSD")
    set_property(TARGET SwiftInProcPluginServer
      APPEND PROPERTY INSTALL_RPATH
        "$ORIGIN")
  endif()

  set_property(TARGET ${name}
    PROPERTY BUILD_WITH_INSTALL_RPATH YES)

  add_dependencies(compiler SwiftInProcPluginServer)
  swift_install_in_component(TARGETS SwiftInProcPluginServer
    ARCHIVE DESTINATION "lib${LLVM_LIBDIR_SUFFIX}/swift/host" COMPONENT compiler
    LIBRARY DESTINATION "lib${LLVM_LIBDIR_SUFFIX}/swift/host" COMPONENT compiler
    RUNTIME DESTINATION "bin" COMPONENT compiler)
endif()
