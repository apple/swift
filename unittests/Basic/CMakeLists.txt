set(generated_tests UnicodeGraphemeBreakTest.cpp.gyb)

handle_gyb_sources(
    gyb_dependency_targets
    generated_tests
    ARCH ${SWIFT_HOST_VARIANT_ARCH})

add_swift_unittest(SwiftBasicTests
  BlotMapVectorTest.cpp
  BlocklistTest.cpp
  CacheTest.cpp
  ClangImporterOptionsTest.cpp
  ClusteredBitVectorTest.cpp
  DemangleTest.cpp
  DiverseStackTest.cpp
  EditorPlaceholderTest.cpp
  EncodedSequenceTest.cpp
  ExponentialGrowthAppendingBinaryByteStreamTests.cpp
  FileSystemTest.cpp
  FixedBitSetTest.cpp
  FrozenMultiMapTest.cpp
  ImmutablePointerSetTest.cpp
  JSONSerialization.cpp
  OptionSetTest.cpp
  OwnedStringTest.cpp
  MultiMapCacheTest.cpp
  PointerIntEnumTest.cpp
  PrefixMapTest.cpp
  RangeTest.cpp
  SourceManagerTest.cpp
  StableHasher.cpp
  STLExtrasTest.cpp
  StringExtrasTest.cpp
  SuccessorMapTest.cpp
  TaggedUnionTest.cpp
  ThreadSafeRefCntPointerTest.cpp
  TransformRangeTest.cpp
  TypeLookupError.cpp
  UnicodeTest.cpp
  ${generated_tests}
  )

add_dependencies(SwiftBasicTests "${gyb_dependency_targets}")

target_link_libraries(SwiftBasicTests
  PRIVATE
  swiftBasic
  swiftThreading
  clangBasic
  LLVMTestingSupport
  )

if(SWIFT_HOST_VARIANT STREQUAL "windows")
  target_link_libraries(SwiftBasicTests PRIVATE Synchronization)
endif()
