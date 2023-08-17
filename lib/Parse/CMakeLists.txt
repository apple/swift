

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(SWIFT_GYB_FLAGS --line-directive "^\"#line %(line)d \\\"%(file)s\\\"^\"")
else()
  set(SWIFT_GYB_FLAGS --line-directive "\'#line" "%(line)d" "\"%(file)s\"\'")
endif()

add_swift_host_library(swiftParse STATIC
  Confusables.cpp
  Lexer.cpp
  ParseDecl.cpp
  ParseExpr.cpp
  ParseGeneric.cpp
  ParseIfConfig.cpp
  ParsePattern.cpp
  Parser.cpp
  ParseRegex.cpp
  ParseRequests.cpp
  ParseStmt.cpp
  ParseType.cpp
  ParseVersion.cpp
  PersistentParserState.cpp)
target_link_libraries(swiftParse PRIVATE
  swiftAST
)

if (SWIFT_SWIFT_PARSER)
  target_link_libraries(swiftParse
    PRIVATE
    SwiftSyntax::SwiftBasicFormat
    SwiftSyntax::SwiftParser
    SwiftSyntax::SwiftParserDiagnostics
    SwiftSyntax::SwiftDiagnostics
    SwiftSyntax::SwiftSyntax
    SwiftSyntax::SwiftOperators
    SwiftSyntax::SwiftSyntaxBuilder
    SwiftSyntax::SwiftSyntaxMacros
    swiftASTGen
  )

  add_dependencies(swiftParse
    SwiftSyntax::SwiftBasicFormat
    SwiftSyntax::SwiftParser
    SwiftSyntax::SwiftParserDiagnostics
    SwiftSyntax::SwiftDiagnostics
    SwiftSyntax::SwiftSyntax
    SwiftSyntax::SwiftOperators
    SwiftSyntax::SwiftSyntaxBuilder
    SwiftSyntax::SwiftSyntaxMacros
    swiftASTGen
  )

  target_compile_definitions(swiftParse
    PRIVATE
    SWIFT_SWIFT_PARSER
  )
endif()

if(SWIFT_COMPILER_VERSION)
  set_property(SOURCE ParseVersion.cpp APPEND_STRING PROPERTY COMPILE_FLAGS
    " -DSWIFT_COMPILER_VERSION=\"\\\"${SWIFT_COMPILER_VERSION}\\\"\"")
endif()

set_swift_llvm_is_available(swiftParse)
