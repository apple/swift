// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -emit-module -o %t/Foo.swiftmodule -emit-abi-descriptor-path %t/abi-before.json %s -enable-library-evolution -DBASELINE -emit-tbd-path %t/abi-before.tbd
// RUN: %target-swift-frontend -emit-module -o %t/Foo.swiftmodule -emit-abi-descriptor-path %t/abi-after.json %s -enable-library-evolution -emit-tbd-path %t/abi-after.tbd
// RUN: %api-digester -diagnose-sdk --input-paths %t/abi-before.json -input-paths %t/abi-after.json -abi -o %t/include-result.txt
// RUN: %FileCheck %s -check-prefix CHECK_INCLUDE_SPI < %t/include-result.txt

// RUN: %api-digester -diagnose-sdk --input-paths %t/abi-before.json -input-paths %t/abi-after.json -abi -o %t/exclude-result.txt -ignore-spi-group secret
// RUN: %FileCheck -check-prefix CHECK_EXCLUDE_SPI %s < %t/exclude-result.txt

#if BASELINE

public struct Struct {
  public var x: Int {
    get { 0 }
    @_spi(secret) set {}
  }
}

@_spi(secret)
public func foo() {}

#else

public struct Struct {
  public var x: Int {
    get { 0 }
  }
}

#endif

// CHECK_INCLUDE_SPI: Accessor Struct.x.Modify() has been removed
// CHECK_EXCLUDE_SPI-NOT: Struct.x.Modify()

// CHECK_INCLUDE_SPI: Accessor Struct.x.Set() has been removed
// CHECK_EXCLUDE_SPI-NOT: Struct.x.Set()

// CHECK_INCLUDE_SPI: Func foo() has been removed
// CHECK_EXCLUDE_SPI-NOT: foo()
