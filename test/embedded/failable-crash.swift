// RUN: %target-swift-emit-ir %s -module-name=main -enable-experimental-feature Embedded | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: OS=macosx || OS=linux-gnu

public func test() {
    _ = MyClass<String>(x: 42)
}

public class MyClass<T> {
    private let member: Int

    public init?(x: Int) {
        guard x > 0 else { return nil }
        self.member = 42
    }
}

// CHECK: define {{.*}}@main(
