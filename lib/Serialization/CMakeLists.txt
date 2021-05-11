set_swift_llvm_is_available()
add_swift_host_library(swiftSerialization STATIC
  Deserialization.cpp
  DeserializeSIL.cpp
  ModuleDependencyScanner.cpp
  ModuleFile.cpp
  ModuleFileSharedCore.cpp
  ModuleSummaryFormat.cpp
  ModuleSummaryIndexer.cpp
  Serialization.cpp
  SerializedModuleLoader.cpp
  SerializedSILLoader.cpp
  SerializeDoc.cpp
  SerializeSIL.cpp

  LLVM_LINK_COMPONENTS
    BitstreamReader
  )
target_link_libraries(swiftSerialization PRIVATE
  swiftClangImporter
  swiftSIL
  swiftSymbolGraphGen)

