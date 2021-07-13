@_cdecl("test")
public func test() -> UInt {
  return unsafeBitCast(c, to: UInt.self)
}

enum E {
case one
case two
case three
}

class C {
  let x = "123"
  let y = 456
  let e = E.two
}

let c = C()
