// RUN: %empty-directory(%t)
//
// RUN: %target-swift-ide-test -print-indexed-symbols -source-filename %s | %FileCheck -check-prefix=CHECK %s

public struct CustomFoo: Hashable {
    public let a: Int
    public let b: String
}
func testFoo() {
    var hasher = Hasher()
    let f = CustomFoo(a: 0, b: "b")
    f.hash(into: &hasher)
    // CHECK: [[@LINE-1]]:7 | instance-method/Swift | hash(into:) | s:14swift_ide_test9CustomFooV4hash4intoys6HasherVz_tF | Ref,Call,RelCall,RelCont | rel: 1
    hasher.finalize()
}
