add_swift_host_tool(sil-llvm-gen
  SILLLVMGen.cpp
  swift_lto_main.cpp
  SWIFT_COMPONENT tools
)
target_link_libraries(sil-llvm-gen
                      PRIVATE
                        swiftFrontend
                        swiftIRGen
                        swiftSILGen
                        swiftSILOptimizer
                        swiftLTO
                        # Clang libraries included to appease the linker on linux.
                        clangBasic
                        clangCodeGen)
