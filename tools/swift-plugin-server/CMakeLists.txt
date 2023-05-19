if (SWIFT_SWIFT_PARSER)
  # _swiftCSwiftPluginServer is just a C support library for swift-plugin-server
  # Don't bother to create '.a' for that.
  add_swift_host_library(_swiftCSwiftPluginServer OBJECT
    Sources/CSwiftPluginServer/PluginServer.cpp
  )
  target_link_libraries(_swiftCSwiftPluginServer PRIVATE
    swiftDemangling
  )
  target_include_directories(_swiftCSwiftPluginServer PUBLIC
    Sources/CSwiftPluginServer/include
  )

  add_pure_swift_host_tool(swift-plugin-server
    Sources/swift-plugin-server/swift-plugin-server.swift
    DEPENDENCIES
      swiftDemangling
      $<TARGET_OBJECTS:_swiftCSwiftPluginServer>
    SWIFT_DEPENDENCIES
      SwiftSyntax::SwiftSyntaxMacros
      SwiftSyntax::SwiftSyntaxMacroExpansion
      SwiftSyntax::SwiftCompilerPluginMessageHandling
      swiftLLVMJSON
  )
  target_include_directories(swift-plugin-server PRIVATE
    Sources/CSwiftPluginServer/include
  )
  swift_install_in_component(TARGETS swift-plugin-server
    RUNTIME
      DESTINATION bin
      COMPONENT compiler
  )
endif()
