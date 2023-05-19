
add_swift_host_library(swiftFrontend STATIC
  ArgsToFrontendInputsConverter.cpp
  ArgsToFrontendOptionsConverter.cpp
  ArgsToFrontendOutputsConverter.cpp
  CachedDiagnostics.cpp
  CachingUtils.cpp
  CompileJobCacheKey.cpp
  CompilerInvocation.cpp
  DependencyVerifier.cpp
  DiagnosticVerifier.cpp
  Frontend.cpp
  FrontendInputsAndOutputs.cpp
  FrontendOptions.cpp
  ModuleInterfaceBuilder.cpp
  ModuleInterfaceLoader.cpp
  ModuleInterfaceSupport.cpp
  PrintingDiagnosticConsumer.cpp
  Serialization.cpp
  SerializedDiagnosticConsumer.cpp)
add_dependencies(swiftFrontend
  SwiftOptions)
target_link_libraries(swiftFrontend PRIVATE
  swiftAPIDigester
  swiftAST
  swiftConstExtract
  swiftIDE
  swiftSIL
  swiftOption
  swiftSILGen
  swiftSILOptimizer
  swiftIRGen
  swiftLocalization
  swiftSema
  swiftSerialization
  swiftSymbolGraphGen)

set_swift_llvm_is_available(swiftFrontend)

if (SWIFT_SWIFT_PARSER)
  target_compile_definitions(swiftFrontend
    PRIVATE
    SWIFT_SWIFT_PARSER
    )
endif()
