
add_swift_host_library(swiftIDETool STATIC
  CompileInstance.cpp
  CompilerInvocation.cpp
  IDEInspectionInstance.cpp
  DependencyChecking.cpp
  SyntacticMacroExpansion.cpp
  )

target_link_libraries(swiftIDETool PRIVATE
  swiftAST
  swiftDriver
  swiftFrontend
  swiftFrontendTool
  clangAST
  clangFrontend)

set_swift_llvm_is_available(swiftIDETool)
