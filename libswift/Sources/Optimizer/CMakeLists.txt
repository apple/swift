
add_swift_host_library(LibSwiftOptimizer
  SHARED
  PURE_SWIFT
  Empty.swift)

target_link_libraries(LibSwiftOptimizer PRIVATE LibSwiftSIL)

add_subdirectory(InstructionPasses)
add_subdirectory(PassManager)
add_subdirectory(FunctionPasses)
