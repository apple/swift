
class Klass {
    var name = "John"
}

#if TEST_LIBRARY_EVOLUTION
public struct MoveOnly : ~Copyable {
    var k = Klass()
    var k2 = Klass()
    public init() {}
    deinit {
        print("==> I am in the deinit resiliently!")
        print("==> My name is \(k.name)")
    }
}
#else
public struct MoveOnly : ~Copyable {
    var k = Klass()
    var k2 = Klass()
    public init() {}
    deinit {
        print("==> I am in the deinit!")
        print("==> My name is \(k.name)")
    }
}
#endif
