

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(SWIFT_GYB_FLAGS --line-directive "^\"#line %(line)d \\\"%(file)s\\\"^\"")
else()
  set(SWIFT_GYB_FLAGS --line-directive "\'#line" "%(line)d" "\"%(file)s\"\'")
endif()

add_swift_host_library(swiftParse STATIC
  Confusables.cpp
  Lexer.cpp
  ParseBridging.cpp
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

if (SWIFT_BUILD_SWIFT_SYNTAX)
  target_link_libraries(swiftParse
    PRIVATE
    swiftASTGen
  )

  add_dependencies(swiftParse
    swiftASTGen
  )
endif()

if(SWIFT_BUILD_REGEX_PARSER_IN_COMPILER)
  target_compile_definitions(swiftParse
    PRIVATE
    SWIFT_BUILD_REGEX_PARSER_IN_COMPILER
  )
endif()

if(SWIFT_COMPILER_VERSION)
  set_property(SOURCE ParseVersion.cpp APPEND_STRING PROPERTY COMPILE_FLAGS
    " -DSWIFT_COMPILER_VERSION=\"\\\"${SWIFT_COMPILER_VERSION}\\\"\"")
endif()

set_swift_llvm_is_available(swiftParse)
