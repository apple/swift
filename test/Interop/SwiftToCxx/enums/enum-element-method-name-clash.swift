// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Enums -clang-header-expose-decls=all-public -emit-clang-header-path %t/enums.h
// RUN: %FileCheck %s < %t/enums.h

public enum Foo: Hashable, Sendable {
    case bar(Parameters)

    // We do not want to generate C++ wrappers for this function yet
    // because its name clashes with the enum element above preventing
    // the generated header from compilation.
    public static func bar(version: Int) -> Self {
        Self.bar(Parameters(version: version))
    }
}

extension Foo {
    public struct Parameters: Hashable, Sendable {
        public var version: Int

        public init(version: Int) {
            self.version = version
        }
    }
}

// CHECK-NOT:   Foo bar(swift::Int version)

// CHECK:    switch (_getEnumTag()) {
// CHECK-NEXT:      case 0: return cases::bar;
// CHECK-NEXT:      default: abort();
// CHECK-NEXT:    }
// CHECK-NEXT:  }
// CHECK-EMPTY:
// Before the fix, we had the static method's thunk here.
// CHECK-NEXT:    swift::Int getHashValue() const