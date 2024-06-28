// RUN: %target-swift-frontend -emit-sil -verify %s

protocol Foo {
    var foo: String { get }
} 
 
func identity(_ a: consuming any Foo) -> String {
    return a.foo
}
