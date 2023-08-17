if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(SWIFT_GYB_FLAGS --line-directive "^\"#line %(line)d \\\"%(file)s\\\"^\"")
else()
  set(SWIFT_GYB_FLAGS --line-directive "\'#line" "%(line)d" "\"%(file)s\"\'")
endif()

set(generated_include_sources
    Attr.def.gyb
    TokenKinds.def.gyb)

