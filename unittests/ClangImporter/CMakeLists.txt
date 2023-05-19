configure_file(${SWIFT_SOURCE_DIR}/stdlib/public/Cxx/libstdcxx/libstdcxx.modulemap
  ${CMAKE_CURRENT_BINARY_DIR}/libstdcxx.modulemap COPYONLY)

add_swift_unittest(SwiftClangImporterTests
  ClangImporterTests.cpp
)

target_link_libraries(SwiftClangImporterTests
    PRIVATE
    swiftClangImporter
    swiftParse
    swiftAST
)
