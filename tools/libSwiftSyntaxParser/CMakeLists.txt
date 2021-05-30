include(SwiftWindowsSupport)
swift_swap_compiler_if_needed("SwiftSyntaxParser")

# Use an 'internal' name, this is primarily intended for SwiftSyntax to import.
set(SYNTAX_PARSER_LIB_NAME "_InternalSwiftSyntaxParser")

set(LLVM_EXPORTED_SYMBOL_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/libSwiftSyntaxParser.exports)

add_swift_host_library(libSwiftSyntaxParser SHARED
  c-include-check.c
  libSwiftSyntaxParser.cpp)

add_swift_host_library(libSwiftSyntaxParserStatic STATIC
  libSwiftSyntaxParser.cpp)

if(NOT SWIFT_BUILT_STANDALONE AND NOT CMAKE_C_COMPILER_ID MATCHES Clang)
  add_dependencies(libSwiftSyntaxParser clang)
  add_dependencies(libSwiftSyntaxParserStatic clang)
endif()
target_link_libraries(libSwiftSyntaxParser PRIVATE
  swiftParse)
target_link_libraries(libSwiftSyntaxParserStatic PRIVATE
  swiftParse)
set_target_properties(libSwiftSyntaxParser
    PROPERTIES
    OUTPUT_NAME ${SYNTAX_PARSER_LIB_NAME})
  set_target_properties(libSwiftSyntaxParserStatic
    PROPERTIES
    OUTPUT_NAME ${SYNTAX_PARSER_LIB_NAME})

add_llvm_symbol_exports(libSwiftSyntaxParser ${LLVM_EXPORTED_SYMBOL_FILE})

# Adds -dead_strip option
add_link_opts(libSwiftSyntaxParser)

if (SWIFT_LIBPARSER_VER)
  set(SWIFTSYNTAX_PARSER_VERSION_STRING "${SWIFT_LIBPARSER_VER}")
else()
  set(SWIFTSYNTAX_PARSER_VERSION_STRING "${SWIFT_COMPILER_VERSION}")
endif()

if(SWIFTSYNTAX_PARSER_VERSION_STRING)
  if("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin")
    set(LIBPARSER_LINK_FLAGS " -Wl,-compatibility_version -Wl,1")
    set(LIBPARSER_LINK_FLAGS "${LIBPARSER_LINK_FLAGS} -Wl,-current_version -Wl,${SWIFTSYNTAX_PARSER_VERSION_STRING}")
    set_property(TARGET libSwiftSyntaxParser APPEND_STRING PROPERTY
                 LINK_FLAGS ${LIBPARSER_LINK_FLAGS})
  else()
    set_target_properties(libSwiftSyntaxParser
      PROPERTIES
      VERSION ${SWIFTSYNTAX_PARSER_VERSION_STRING})
  endif()
endif()

set_property(TARGET libSwiftSyntaxParser APPEND_STRING PROPERTY
  COMPILE_FLAGS " -fblocks")
set_property(TARGET libSwiftSyntaxParserStatic APPEND_STRING PROPERTY
  COMPILE_FLAGS " -fblocks")
if(NOT CMAKE_SYSTEM_NAME STREQUAL Darwin)
  target_link_libraries(libSwiftSyntaxParser PRIVATE
    BlocksRuntime)
  target_link_libraries(libSwiftSyntaxParserStatic PRIVATE
    BlocksRuntime)
endif()

add_dependencies(parser-lib libSwiftSyntaxParser)
add_dependencies(parser-lib libSwiftSyntaxParserStatic)
swift_install_in_component(TARGETS libSwiftSyntaxParser
  ARCHIVE DESTINATION "lib${LLVM_LIBDIR_SUFFIX}/swift/${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR}" COMPONENT parser-lib
  LIBRARY DESTINATION "lib${LLVM_LIBDIR_SUFFIX}/swift/${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR}" COMPONENT parser-lib
  RUNTIME DESTINATION "bin" COMPONENT parser-lib)
swift_install_in_component(TARGETS libSwiftSyntaxParserStatic
  ARCHIVE DESTINATION "lib${LLVM_LIBDIR_SUFFIX}/swift/${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR}" COMPONENT parser-lib
  LIBRARY DESTINATION "lib${LLVM_LIBDIR_SUFFIX}/swift/${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR}" COMPONENT parser-lib
  RUNTIME DESTINATION "bin" COMPONENT parser-lib)
swift_install_in_component(DIRECTORY "${SWIFT_MAIN_INCLUDE_DIR}/swift-c/SyntaxParser/"
                           DESTINATION "lib${LLVM_LIBDIR_SUFFIX}/swift/${SYNTAX_PARSER_LIB_NAME}"
                           COMPONENT parser-lib)
