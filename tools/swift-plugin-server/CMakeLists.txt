if (SWIFT_BUILD_SWIFT_SYNTAX)
  # _swiftCSwiftPluginServer is just a C support library for swift-plugin-server
  # Don't bother to create '.a' for that.
  add_swift_host_library(_swiftCSwiftPluginServer OBJECT
    Sources/CSwiftPluginServer/PluginServer.cpp
  )
  target_include_directories(_swiftCSwiftPluginServer PUBLIC
    Sources/CSwiftPluginServer/include
  )

  add_pure_swift_host_tool(swift-plugin-server
    Sources/swift-plugin-server/swift-plugin-server.swift
    DEPENDENCIES
      $<TARGET_OBJECTS:_swiftCSwiftPluginServer>
    SWIFT_COMPONENT
      compiler
    SWIFT_DEPENDENCIES
      SwiftSyntaxMacros
      SwiftSyntaxMacroExpansion
      SwiftCompilerPluginMessageHandling
      swiftLLVMJSON
  )
  target_include_directories(swift-plugin-server PRIVATE
    Sources/CSwiftPluginServer/include
  )
endif()
