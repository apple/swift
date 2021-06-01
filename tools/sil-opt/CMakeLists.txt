add_swift_host_tool(sil-opt
  SILOpt.cpp
  SWIFT_COMPONENT tools
)
target_link_libraries(sil-opt
                      PRIVATE
                        swiftFrontend
                        swiftIRGen
                        swiftSIL
                        swiftSILGen
                        swiftSILOptimizer
                        LibSwiftSIL
                        LibSwiftOptimizer
                        # Clang libraries included to appease the linker on linux.
                        clangBasic
                        clangCodeGen)
