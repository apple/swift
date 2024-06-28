// RUN: %target-swift-frontend -target armv7-apple-none-macho -Xcc -D__MACH__ -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s
// RUN: %target-swift-frontend -target arm64-apple-none-macho -Xcc -D__MACH__ -Xcc -D__arm64__ -Xcc -D__APPLE__ -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: optimized_stdlib
// REQUIRES: CODEGENERATOR=ARM
// REQUIRES: embedded_stdlib_cross_compiling

// https://github.com/apple/swift/issues/73249
// UNSUPPORTED: OS=windows-msvc

class MyClass {}

public func test() {
  var bool = false
  var int = 42
  var int8: Int8 = 42
  var int32: Int32 = 42
  var uint = UInt(42)
  var float: Float = 7.77
  var double: Double = 7.77
  var optional1: Bool? = nil
  var optional2: Int? = 42
  var optional3: [Int]? = [1, 2, 3]
  var staticstring: StaticString = "hello"
  var p1 = UnsafeRawPointer(bitPattern: 42)!
  var p2 = UnsafeMutableRawPointer(bitPattern: 42)!
  var p3 = UnsafePointer<UInt>(bitPattern: 42)!
  var p4 = UnsafeMutablePointer<UInt>(bitPattern: 42)!
  var p5 = UnsafeBufferPointer<UInt>(start: p3, count: 1)
  var array: [Int] = [1, 2, 3]
  var array2: ContiguousArray<Int> = ContiguousArray(array)
  var subarray: ArraySlice<Int> = array[0..<2]
  var enumerated: EnumeratedSequence<ArraySlice<Int>> = subarray.enumerated()
  var range1 = 0 ..< 10
  var range2 = 0 ... 10
  _ = MemoryLayout<Int>.size
  _ = MemoryLayout<Int>.stride
  _ = MemoryLayout<Int>.alignment
  let unmanaged = Unmanaged.passUnretained(MyClass())
  let opaque = unmanaged.toOpaque()
  let unmanaged2 = Unmanaged<MyClass>.fromOpaque(opaque)
  let o = unmanaged2.takeUnretainedValue()
  var s1: Set<Int> = [1, 2, 3]
}

test()

// CHECK: define {{.*}}i32 @main(i32 %0, ptr %1)
