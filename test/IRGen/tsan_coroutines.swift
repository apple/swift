// REQUIRES: tsan_runtime
// This test case used to crash when tsan ran before co-routine lowering.
// RUN: %target-swift-frontend -emit-ir -sanitize=thread %s | %FileCheck %s

// TSan is only supported on 64 bit.
// REQUIRES: PTRSIZE=64

public class C { }

public struct Foobar {
  var things: [String: C] = [:]
}

extension Foobar {
    public struct Index {
        fileprivate typealias MyIndex = Dictionary<String, C>.Values.Index

        fileprivate let myIndex: MyIndex

        fileprivate init(_ index: MyIndex) {
            self.myIndex = index
        }
    }

    // We used to crash emitting the subscript function.
    // CHECK: define{{( dllexport| protected)?}} swiftcc { ptr, ptr } @"$s15tsan_coroutines6FoobarVyAA1CCAC5IndexVcir"
    @_borrowed
    public subscript(position: Index) -> C {
        return things.values[position.myIndex]
    }
}
