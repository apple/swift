add_swift_host_tool(swift-module-summary-test
  swift-module-summary-test.cpp
  LLVM_LINK_COMPONENTS
    Support
  SWIFT_COMPONENT tools
)

target_link_libraries(swift-module-summary-test
                      PRIVATE
                        swiftAST
                        swiftDemangling
                        swiftSerialization)
