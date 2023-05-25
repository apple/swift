// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -module-name ProtocolClassInheritance -emit-module -emit-module-path %t/
// RUN: %target-swift-symbolgraph-extract -module-name ProtocolClassInheritance -I %t -output-dir %t
// RUN: %FileCheck %s --input-file %t/ProtocolClassInheritance.symbols.json

// When a protocol that declares a class inheritance requirement is added by an extension, make sure
// that SymbolGraphGen does not crash (rdar://109418762)

public class ClassOne {}
public protocol ProtoOne: ClassOne {}

public class ClassTwo: ClassOne {}
extension ClassTwo: ProtoOne {}

// Same for a generic class inheritance requirement

public class ClassThree<T> {}
public protocol ProtoTwo: ClassThree<Int> {}

public class ClassFour: ClassThree<Int> {}
extension ClassFour: ProtoTwo {}

// Same for a protocol with a primary associated type

public protocol ProtoThree<T> {
    associatedtype T
}
public protocol ProtoFour: ProtoThree<Int> {}

public class ClassFive: ProtoThree {
    public typealias T = Int
}
extension ClassFive: ProtoFour {}

// ClassTwo conforms to ProtoOne
// CHECK-DAG: {"kind":"conformsTo","source":"s:24ProtocolClassInheritance0B3TwoC","target":"s:24ProtocolClassInheritance8ProtoOneP"}

// ClassFour conforms to ProtoTwo
// CHECK-DAG: {"kind":"conformsTo","source":"s:24ProtocolClassInheritance0B4FourC","target":"s:24ProtocolClassInheritance8ProtoTwoP"}

// ClassFive conforms to ProtoFour
// CHECK-DAG: {"kind":"conformsTo","source":"s:24ProtocolClassInheritance0B4FiveC","target":"s:24ProtocolClassInheritance9ProtoFourP"}
