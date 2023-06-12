
#if TEST_LIBRARY_WITH_LIBRARY_EVOLUTION
public struct MoveOnly : ~Copyable {
#if MAKE_LARGE
  var x0 = 0, x1 = 0, x2 = 0, x3 = 0, x4 = 0, x5 = 0, x6 = 0, x7 = 0,
  x8 = 0, x9 = 0, x10 = 0, x11 = 0, x12 = 0, x13 = 0, x14 = 0, x15 = 0,
  x16 = 0, x17 = 0, x18 = 0, x19 = 0, x20 = 0, x21 = 0, x22 = 0,
  x23 = 0, x24 = 0, x25 = 0, x26 = 0, x27 = 0, x28 = 0, x29 = 0,
  x30 = 0, x31 = 0, x32 = 0, x33 = 0, x34 = 0, x35 = 0, x36 = 0,
  x37 = 0, x38 = 0
#endif
  var name = "John"

  public init() {}
  deinit {
    print("==> LIBRARY_EVOLUTION: I am in the deinit!")
    print("==> My name is: \(name)!")
  }
}
#else

#if TEST_LIBRARY_WITHOUT_LIBRARY_EVOLUTION

public struct MoveOnly : ~Copyable {
#if MAKE_LARGE
  var x0 = 0, x1 = 0, x2 = 0, x3 = 0, x4 = 0, x5 = 0, x6 = 0, x7 = 0,
  x8 = 0, x9 = 0, x10 = 0, x11 = 0, x12 = 0, x13 = 0, x14 = 0, x15 = 0,
  x16 = 0, x17 = 0, x18 = 0, x19 = 0, x20 = 0, x21 = 0, x22 = 0,
  x23 = 0, x24 = 0, x25 = 0, x26 = 0, x27 = 0, x28 = 0, x29 = 0,
  x30 = 0, x31 = 0, x32 = 0, x33 = 0, x34 = 0, x35 = 0, x36 = 0,
  x37 = 0, x38 = 0
#endif
  var name = "John"
  public init() {}
  deinit {
    print("==> LIBRARY: I am in the deinit!")
    print("==> My name is: \(name)!")
  }
}

#else

struct MoveOnly : ~Copyable {
#if MAKE_LARGE
  var x0 = 0, x1 = 0, x2 = 0, x3 = 0, x4 = 0, x5 = 0, x6 = 0, x7 = 0,
  x8 = 0, x9 = 0, x10 = 0, x11 = 0, x12 = 0, x13 = 0, x14 = 0, x15 = 0,
  x16 = 0, x17 = 0, x18 = 0, x19 = 0, x20 = 0, x21 = 0, x22 = 0,
  x23 = 0, x24 = 0, x25 = 0, x26 = 0, x27 = 0, x28 = 0, x29 = 0,
  x30 = 0, x31 = 0, x32 = 0, x33 = 0, x34 = 0, x35 = 0, x36 = 0,
  x37 = 0, x38 = 0
#endif

  var name = "John"
  deinit {
    print("==> I am in the deinit!")
    print("==> My name is: \(name)!")
  }
}

#endif
#endif
