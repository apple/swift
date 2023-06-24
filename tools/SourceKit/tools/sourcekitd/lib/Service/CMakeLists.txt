
add_sourcekit_library(sourcekitdService
  Requests.cpp
)
target_compile_definitions(sourcekitdService PRIVATE
  sourcekitd_EXPORTS)
target_link_libraries(sourcekitdService PRIVATE
  sourcekitdAPI
  swiftBasic
  SourceKitSupport
  SourceKitSwiftLang
  swiftCompilerModules)
