add_swift_host_library(swiftSILOptimizer STATIC
  SILOptimizer.cpp)
target_link_libraries(swiftSILOptimizer PRIVATE
  swiftSIL)
set_swift_llvm_is_available(swiftSILOptimizer)

add_subdirectory(ARC)
add_subdirectory(Analysis)
add_subdirectory(Differentiation)
add_subdirectory(FunctionSignatureTransforms)
add_subdirectory(IPO)
add_subdirectory(LoopTransforms)
add_subdirectory(Mandatory)
add_subdirectory(PassManager)
add_subdirectory(SILCombiner)
add_subdirectory(SemanticARC)
add_subdirectory(Transforms)
add_subdirectory(UtilityPasses)
add_subdirectory(Utils)
