add_swift_unittest(SwiftSILTests
  PartitionUtilsTest.cpp
  SILBitfieldTest.cpp
)

target_link_libraries(SwiftSILTests
   PRIVATE
   swiftSIL
)
