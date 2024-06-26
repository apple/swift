// RUN: %target-typecheck-verify-swift

func escapeByBitCast(f: () -> ()) -> () -> () {
  return unsafeBitCast(f, to: (() -> ()).self)
  // expected-error@-1 {{converting non-escaping parameter 'f' to generic parameter 'T' may allow it to escape}}
}

func changeFnRep(f: @escaping () -> ()) -> @convention(block) () -> () {
  // expected-warning@+1{{'unsafeBitCast' from function type '() -> ()' to '@convention(block) () -> ()' changes @convention and is undefined; use an implicit conversion to change conventions}}
  return unsafeBitCast(f, to: (@convention(block) () -> ()).self)
}

class A {}
class B: A {}

class C<T> {}
class D: C<Int> {}

class N<T> {}

func castAToB(a: A) -> B {
  // expected-warning@+1{{'unsafeBitCast' from 'A' to 'B' can be replaced with 'unsafeDowncast'}} {{7-20=unsafeDowncast}}
  _ = unsafeBitCast(a, to: B.self)
  // expected-warning@+1{{'unsafeBitCast' from 'A' to 'B' can be replaced with 'unsafeDowncast'}} {{18-31=unsafeDowncast}}
  return Swift  .unsafeBitCast  (a, to: B.self)
  // expected-warning@+1{{'unsafeBitCast' from 'A' to 'B' can be replaced with 'unsafeDowncast'}} {{16-29=unsafeDowncast}}
  return Swift.unsafeBitCast(_:to:)(a, B.self)
}

func castCToD<T>(c: C<T>) -> D {
  return unsafeBitCast(c, to: D.self) // expected-no-warning
}

func castNToD<T>(n: N<T>) -> D {
  return unsafeBitCast(n, to: D.self) // CHECK-NOT: warning
}

func castDToC<T>(d: D) -> (C<T>, C<Int>) {
  let a = unsafeBitCast(d, to: C<T>.self) // not necessarily a no-op if T != Int
  // expected-warning@+1{{'unsafeBitCast' from 'D' to 'C<Int>' is unnecessary}}{{11-25=}}{{26-44=}}
  let b = unsafeBitCast(d, to: C<Int>.self)

  return (a, b)
}

func castIntToInt(x: Int) -> Int {
  // expected-warning@+1{{'unsafeBitCast' from 'Int' to 'Int' is unnecessary}}{{10-24=}}{{25-40=}}
  return unsafeBitCast(x, to: Int.self)
}

func castBetweenNumberReps(i: Int, f: Float, u: UInt, d: Double, ii: Int32, uu: UInt32, iii: Int64, uuu: UInt64) {
  // expected-warning@+1{{'unsafeBitCast' from 'Int' to 'Float' can be replaced with 'bitPattern:' initializer}}{{7-21=Float(bitPattern: UInt32(bitPattern: Int32(}}{{22-39=)))}}
  _ = unsafeBitCast(i, to: Float.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Int' to 'Double' can be replaced with 'bitPattern:' initializer}}{{7-21=Double(bitPattern: UInt64(bitPattern: Int64(}}{{22-40=)))}}
  _ = unsafeBitCast(i, to: Double.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Int' to 'UInt' can be replaced with 'bitPattern:' initializer}}{{7-21=UInt(bitPattern: }}{{22-38=)}}
  _ = unsafeBitCast(i, to: UInt.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Int' to 'Int32' can be replaced with 'Int32' initializer}}{{7-21=Int32(}}{{22-39=)}}
  _ = unsafeBitCast(i, to: Int32.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Int' to 'UInt32' can be replaced with 'bitPattern:' initializer}}{{7-21=UInt32(bitPattern: Int32(}}{{22-40=))}}
  _ = unsafeBitCast(i, to: UInt32.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Int' to 'Int64' can be replaced with 'Int64' initializer}}{{7-21=Int64(}}{{22-39=)}}
  _ = unsafeBitCast(i, to: Int64.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Int' to 'UInt64' can be replaced with 'bitPattern:' initializer}}{{7-21=UInt64(bitPattern: Int64(}}{{22-40=))}}
  _ = unsafeBitCast(i, to: UInt64.self)

  // expected-warning@+1{{'unsafeBitCast' from 'UInt' to 'Float' can be replaced with 'bitPattern:' initializer}}{{7-21=Float(bitPattern: UInt32(}}{{22-39=))}}
  _ = unsafeBitCast(u, to: Float.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt' to 'Double' can be replaced with 'bitPattern:' initializer}}{{7-21=Double(bitPattern: UInt64(}}{{22-40=))}}
  _ = unsafeBitCast(u, to: Double.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt' to 'Int' can be replaced with 'bitPattern:' initializer}}{{7-21=Int(bitPattern: }}{{22-37=)}}
  _ = unsafeBitCast(u, to: Int.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt' to 'Int32' can be replaced with 'bitPattern:' initializer}}{{7-21=Int32(bitPattern: UInt32(}}{{22-39=))}}
  _ = unsafeBitCast(u, to: Int32.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt' to 'UInt32' can be replaced with 'UInt32' initializer}}{{7-21=UInt32(}}{{22-40=)}}
  _ = unsafeBitCast(u, to: UInt32.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt' to 'Int64' can be replaced with 'bitPattern:' initializer}}{{7-21=Int64(bitPattern: UInt64(}}{{22-39=))}}
  _ = unsafeBitCast(u, to: Int64.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt' to 'UInt64' can be replaced with 'UInt64' initializer}}{{7-21=UInt64(}}{{22-40=)}}
  _ = unsafeBitCast(u, to: UInt64.self)

  // expected-warning@+1{{'unsafeBitCast' from 'Float' to 'Int' can be replaced with 'bitPattern' property}}{{7-21=Int(bitPattern: UInt(}}{{22-37=.bitPattern))}}
  _ = unsafeBitCast(f, to: Int.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Float' to 'UInt' can be replaced with 'bitPattern' property}}{{7-21=UInt(}}{{22-38=.bitPattern)}}
  _ = unsafeBitCast(f, to: UInt.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Float' to 'Int32' can be replaced with 'bitPattern' property}}{{7-21=Int32(bitPattern: }}{{22-39=)}}
  _ = unsafeBitCast(f, to: Int32.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Float' to 'UInt32' can be replaced with 'bitPattern' property}}{{7-21=}}{{22-40=.bitPattern}}
  _ = unsafeBitCast(f, to: UInt32.self)
  _ = unsafeBitCast(f, to: Int64.self)
  _ = unsafeBitCast(f, to: UInt64.self)
  _ = unsafeBitCast(f, to: Double.self)

  // expected-warning@+1{{'unsafeBitCast' from 'Double' to 'Int' can be replaced with 'bitPattern' property}}{{7-21=Int(bitPattern: UInt(}}{{22-37=.bitPattern))}}
  _ = unsafeBitCast(d, to: Int.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Double' to 'UInt' can be replaced with 'bitPattern' property}}{{7-21=UInt(}}{{22-38=.bitPattern)}}
  _ = unsafeBitCast(d, to: UInt.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Double' to 'Int64' can be replaced with 'bitPattern' property}}{{7-21=Int64(bitPattern: }}{{22-39=)}}
  _ = unsafeBitCast(d, to: Int64.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Double' to 'UInt64' can be replaced with 'bitPattern' property}}{{7-21=}}{{22-40=.bitPattern}}
  _ = unsafeBitCast(d, to: UInt64.self)
  _ = unsafeBitCast(d, to: Int32.self)
  _ = unsafeBitCast(d, to: UInt32.self)
  _ = unsafeBitCast(d, to: Float.self)

  // expected-warning@+1{{'unsafeBitCast' from 'Int32' to 'Float' can be replaced with 'bitPattern:' initializer}}{{7-21=Float(bitPattern: UInt32(bitPattern: }}{{23-40=))}}
  _ = unsafeBitCast(ii, to: Float.self)
  _ = unsafeBitCast(ii, to: Double.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Int32' to 'Int' can be replaced with 'Int' initializer}}{{7-21=Int(}}{{23-38=)}}
  _ = unsafeBitCast(ii, to: Int.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Int32' to 'UInt' can be replaced with 'bitPattern:' initializer}}{{7-21=UInt(UInt32(bitPattern: }}{{23-39=))}}
  _ = unsafeBitCast(ii, to: UInt.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Int32' to 'UInt32' can be replaced with 'bitPattern:' initializer}}{{7-21=UInt32(bitPattern: }}{{23-41=)}}
  _ = unsafeBitCast(ii, to: UInt32.self)
  _ = unsafeBitCast(ii, to: Int64.self)
  _ = unsafeBitCast(ii, to: UInt64.self)

  // expected-warning@+1{{'unsafeBitCast' from 'UInt32' to 'Float' can be replaced with 'bitPattern:' initializer}}{{7-21=Float(bitPattern: }}{{23-40=)}}
  _ = unsafeBitCast(uu, to: Float.self)
  _ = unsafeBitCast(uu, to: Double.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt32' to 'Int' can be replaced with 'bitPattern:' initializer}}{{7-21=Int(Int32(bitPattern: }}{{23-38=))}}
  _ = unsafeBitCast(uu, to: Int.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt32' to 'UInt' can be replaced with 'UInt' initializer}}{{7-21=UInt(}}{{23-39=)}}
  _ = unsafeBitCast(uu, to: UInt.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt32' to 'Int32' can be replaced with 'bitPattern:' initializer}}{{7-21=Int32(bitPattern: }}{{23-40=)}}
  _ = unsafeBitCast(uu, to: Int32.self)
  _ = unsafeBitCast(uu, to: Int64.self)
  _ = unsafeBitCast(uu, to: UInt64.self)

  _ = unsafeBitCast(iii, to: Float.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Int64' to 'Double' can be replaced with 'bitPattern:' initializer}}{{7-21=Double(bitPattern: UInt64(bitPattern: }}{{24-42=))}}
  _ = unsafeBitCast(iii, to: Double.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Int64' to 'Int' can be replaced with 'Int' initializer}}{{7-21=Int(}}{{24-39=)}}
  _ = unsafeBitCast(iii, to: Int.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Int64' to 'UInt' can be replaced with 'bitPattern:' initializer}}{{7-21=UInt(UInt64(bitPattern: }}{{24-40=))}}
  _ = unsafeBitCast(iii, to: UInt.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Int64' to 'UInt64' can be replaced with 'bitPattern:' initializer}}{{7-21=UInt64(bitPattern: }}{{24-42=)}}
  _ = unsafeBitCast(iii, to: UInt64.self)
  _ = unsafeBitCast(iii, to: Int32.self)
  _ = unsafeBitCast(iii, to: UInt32.self)

  _ = unsafeBitCast(uuu, to: Float.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt64' to 'Double' can be replaced with 'bitPattern:' initializer}}{{7-21=Double(bitPattern: }}{{24-42=)}}
  _ = unsafeBitCast(uuu, to: Double.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt64' to 'Int' can be replaced with 'bitPattern:' initializer}}{{7-21=Int(Int64(bitPattern: }}{{24-39=))}}
  _ = unsafeBitCast(uuu, to: Int.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt64' to 'UInt' can be replaced with 'UInt' initializer}}{{7-21=UInt(}}{{24-40=)}}
  _ = unsafeBitCast(uuu, to: UInt.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt64' to 'Int64' can be replaced with 'bitPattern:' initializer}}{{7-21=Int64(bitPattern: }}{{24-41=)}}
  _ = unsafeBitCast(uuu, to: Int64.self)
  _ = unsafeBitCast(uuu, to: Int32.self)
  _ = unsafeBitCast(uuu, to: UInt32.self)
}

func castBetweenPointers(p: UnsafePointer<Int>, q: UnsafePointer<Float>,
                         mp: UnsafeMutablePointer<Int>, mq: UnsafeMutablePointer<Float>,
                         rp: UnsafeRawPointer, mrp: UnsafeMutableRawPointer) {
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Int>' to 'UnsafePointer<Float>' changes pointee type and may lead to undefined behavior; use the 'withMemoryRebound' method on 'UnsafePointer<Int>' to rebind the type of memory}}
  _ = unsafeBitCast(p, to: UnsafePointer<Float>.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Int>' to 'UnsafeMutablePointer<Float>' changes pointee type and may lead to undefined behavior; use the 'withMemoryRebound' method on 'UnsafePointer<Int>' to rebind the type of memory}}
  _ = unsafeBitCast(p, to: UnsafeMutablePointer<Float>.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Int>' to 'UnsafeMutablePointer<Int>' can be replaced with 'UnsafeMutablePointer' initializer}}{{7-21=UnsafeMutablePointer(mutating: }}{{22-59=)}}
  _ = unsafeBitCast(p, to: UnsafeMutablePointer<Int>.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Int>' to 'UnsafeRawPointer' can be replaced with 'UnsafeRawPointer' initializer}}{{7-21=UnsafeRawPointer(}}{{22-50=)}}
  _ = unsafeBitCast(p, to: UnsafeRawPointer.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Int>' to 'UnsafeMutableRawPointer' can be replaced with 'UnsafeMutableRawPointer' initializer}}{{7-21=UnsafeMutableRawPointer(mutating: }}{{22-57=)}}
  _ = unsafeBitCast(p, to: UnsafeMutableRawPointer.self)

  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Float>' to 'UnsafePointer<Int>' changes pointee type and may lead to undefined behavior; use the 'withMemoryRebound' method on 'UnsafePointer<Float>' to rebind the type of memory}}
  _ = unsafeBitCast(q, to: UnsafePointer<Int>.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Float>' to 'UnsafeMutablePointer<Int>' changes pointee type and may lead to undefined behavior; use the 'withMemoryRebound' method on 'UnsafePointer<Float>' to rebind the type of memory}}
  _ = unsafeBitCast(q, to: UnsafeMutablePointer<Int>.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Float>' to 'UnsafeMutablePointer<Float>' can be replaced with 'UnsafeMutablePointer' initializer}}{{7-21=UnsafeMutablePointer(mutating: }}{{22-61=)}}
  _ = unsafeBitCast(q, to: UnsafeMutablePointer<Float>.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Float>' to 'UnsafeRawPointer' can be replaced with 'UnsafeRawPointer' initializer}}{{7-21=UnsafeRawPointer(}}{{22-50=)}}
  _ = unsafeBitCast(q, to: UnsafeRawPointer.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Float>' to 'UnsafeMutableRawPointer' can be replaced with 'UnsafeMutableRawPointer' initializer}}{{7-21=UnsafeMutableRawPointer(mutating: }}{{22-57=)}}
  _ = unsafeBitCast(q, to: UnsafeMutableRawPointer.self)

  // expected-warning@+1{{'unsafeBitCast' from 'UnsafeMutablePointer<Int>' to 'UnsafePointer<Float>' changes pointee type and may lead to undefined behavior; use the 'withMemoryRebound' method on 'UnsafeMutablePointer<Int>' to rebind the type of memory}}
  _ = unsafeBitCast(mp, to: UnsafePointer<Float>.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafeMutablePointer<Int>' to 'UnsafeMutablePointer<Float>' changes pointee type and may lead to undefined behavior; use the 'withMemoryRebound' method on 'UnsafeMutablePointer<Int>' to rebind the type of memory}}
  _ = unsafeBitCast(mp, to: UnsafeMutablePointer<Float>.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafeMutablePointer<Int>' to 'UnsafePointer<Int>' can be replaced with 'UnsafePointer' initializer}}{{7-21=UnsafePointer(}}{{23-53=)}}
  _ = unsafeBitCast(mp, to: UnsafePointer<Int>.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafeMutablePointer<Int>' to 'UnsafeRawPointer' can be replaced with 'UnsafeRawPointer' initializer}}{{7-21=UnsafeRawPointer(}}{{23-51=)}}
  _ = unsafeBitCast(mp, to: UnsafeRawPointer.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafeMutablePointer<Int>' to 'UnsafeMutableRawPointer' can be replaced with 'UnsafeMutableRawPointer' initializer}}{{7-21=UnsafeMutableRawPointer(}}{{23-58=)}}
  _ = unsafeBitCast(mp, to: UnsafeMutableRawPointer.self)

  // expected-warning@+1{{'unsafeBitCast' from 'UnsafeMutablePointer<Float>' to 'UnsafePointer<Int>' changes pointee type and may lead to undefined behavior; use the 'withMemoryRebound' method on 'UnsafeMutablePointer<Float>' to rebind the type of memory}}
  _ = unsafeBitCast(mq, to: UnsafePointer<Int>.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafeMutablePointer<Float>' to 'UnsafeMutablePointer<Int>' changes pointee type and may lead to undefined behavior; use the 'withMemoryRebound' method on 'UnsafeMutablePointer<Float>' to rebind the type of memory}}
  _ = unsafeBitCast(mq, to: UnsafeMutablePointer<Int>.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafeMutablePointer<Float>' to 'UnsafePointer<Float>' can be replaced with 'UnsafePointer' initializer}}{{7-21=UnsafePointer(}}{{23-55=)}}
  _ = unsafeBitCast(mq, to: UnsafePointer<Float>.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafeMutablePointer<Float>' to 'UnsafeRawPointer' can be replaced with 'UnsafeRawPointer' initializer}}{{7-21=UnsafeRawPointer(}}{{23-51=)}}
  _ = unsafeBitCast(mq, to: UnsafeRawPointer.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafeMutablePointer<Float>' to 'UnsafeMutableRawPointer' can be replaced with 'UnsafeMutableRawPointer' initializer}}{{7-21=UnsafeMutableRawPointer(}}{{23-58=)}}
  _ = unsafeBitCast(mq, to: UnsafeMutableRawPointer.self)

  // expected-warning@+1{{'unsafeBitCast' from 'UnsafeRawPointer' to 'UnsafeMutableRawPointer' can be replaced with 'UnsafeMutableRawPointer' initializer}}{{7-21=UnsafeMutableRawPointer(mutating: }}{{23-58=)}}
  _ = unsafeBitCast(rp, to: UnsafeMutableRawPointer.self)
  // expected-warning@+3{{'unsafeBitCast' from 'UnsafeRawPointer' to 'UnsafePointer<Int>' gives a type to a raw pointer and may lead to undefined behavior}}
  // expected-note@+2{{use the 'assumingMemoryBound' method if the pointer is known to point to an existing value or array of type 'Int' in memory}}{{7-21=}}{{23-53=.assumingMemoryBound(to: Int.self)}}
  // expected-note@+1{{use the 'bindMemory' method to assign type 'Int' to uninitialized raw memory}}{{7-21=}}{{23-53=.bindMemory(to: Int.self, capacity: <#capacity#>)}}
  _ = unsafeBitCast(rp, to: UnsafePointer<Int>.self)
  // expected-warning@+3{{'unsafeBitCast' from 'UnsafeRawPointer' to 'UnsafeMutablePointer<Float>' gives a type to a raw pointer and may lead to undefined behavior}}
  // expected-note@+2{{use the 'assumingMemoryBound' method if the pointer is known to point to an existing value or array of type 'Float' in memory}}{{7-21=}}{{23-62=.assumingMemoryBound(to: Float.self)}}
  // expected-note@+1{{use the 'bindMemory' method to assign type 'Float' to uninitialized raw memory}}{{7-21=}}{{23-62=.bindMemory(to: Float.self, capacity: <#capacity#>)}}
  _ = unsafeBitCast(rp, to: UnsafeMutablePointer<Float>.self)

  // expected-warning@+1{{'unsafeBitCast' from 'UnsafeMutableRawPointer' to 'UnsafeRawPointer' can be replaced with 'UnsafeRawPointer' initializer}}{{7-21=UnsafeRawPointer(}}{{24-52=)}}
  _ = unsafeBitCast(mrp, to: UnsafeRawPointer.self)
  // expected-warning@+3{{'unsafeBitCast' from 'UnsafeMutableRawPointer' to 'UnsafePointer<Int>' gives a type to a raw pointer and may lead to undefined behavior}}
  // expected-note@+2{{use the 'assumingMemoryBound' method if the pointer is known to point to an existing value or array of type 'Int' in memory}}{{7-21=}}{{24-54=.assumingMemoryBound(to: Int.self)}}
  // expected-note@+1{{use the 'bindMemory' method to assign type 'Int' to uninitialized raw memory}}{{7-21=}}{{24-54=.bindMemory(to: Int.self, capacity: <#capacity#>)}}
  _ = unsafeBitCast(mrp, to: UnsafePointer<Int>.self)
  // expected-warning@+3{{'unsafeBitCast' from 'UnsafeMutableRawPointer' to 'UnsafeMutablePointer<Float>' gives a type to a raw pointer and may lead to undefined behavior}}
  // expected-note@+2{{use the 'assumingMemoryBound' method if the pointer is known to point to an existing value or array of type 'Float' in memory}}{{7-21=}}{{24-63=.assumingMemoryBound(to: Float.self)}}
  // expected-note@+1{{use the 'bindMemory' method to assign type 'Float' to uninitialized raw memory}}{{7-21=}}{{24-63=.bindMemory(to: Float.self, capacity: <#capacity#>)}}
  _ = unsafeBitCast(mrp, to: UnsafeMutablePointer<Float>.self)
}

func castBetweenIntAndPointer(p: UnsafePointer<Int>,
                              i: Int,
                              u: UInt,
                              ii: Int32,
                              uu: UInt32,
                              iii: Int64,
                              uuu: UInt64) {
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Int>' to 'Int' can be replaced with 'bitPattern:' initializer}}{{7-21=Int(bitPattern: }}{{22-37=)}}
  _ = unsafeBitCast(p, to: Int.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Int>' to 'UInt' can be replaced with 'bitPattern:' initializer}}{{7-21=UInt(bitPattern: }}{{22-38=)}}
  _ = unsafeBitCast(p, to: UInt.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Int>' to 'Int32' can be replaced with 'bitPattern:' initializer}}{{7-21=Int32(Int(bitPattern: }}{{22-39=))}}
  _ = unsafeBitCast(p, to: Int32.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Int>' to 'UInt32' can be replaced with 'bitPattern:' initializer}}{{7-21=UInt32(UInt(bitPattern: }}{{22-40=))}}
  _ = unsafeBitCast(p, to: UInt32.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Int>' to 'Int64' can be replaced with 'bitPattern:' initializer}}{{7-21=Int64(Int(bitPattern: }}{{22-39=))}}
  _ = unsafeBitCast(p, to: Int64.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UnsafePointer<Int>' to 'UInt64' can be replaced with 'bitPattern:' initializer}}{{7-21=UInt64(UInt(bitPattern: }}{{22-40=))}}
  _ = unsafeBitCast(p, to: UInt64.self)

  // expected-warning@+1{{'unsafeBitCast' from 'Int' to 'UnsafeRawPointer' can be replaced with 'bitPattern:' initializer}}{{7-21=UnsafeRawPointer(bitPattern: }}{{22-50=)}}
  _ = unsafeBitCast(i, to: UnsafeRawPointer.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt' to 'UnsafeRawPointer' can be replaced with 'bitPattern:' initializer}}{{7-21=UnsafeRawPointer(bitPattern: }}{{22-50=)}}
  _ = unsafeBitCast(u, to: UnsafeRawPointer.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Int32' to 'UnsafeRawPointer' can be replaced with 'bitPattern:' initializer}}{{7-21=UnsafeRawPointer(bitPattern: Int(}}{{23-51=))}}
  _ = unsafeBitCast(ii, to: UnsafeRawPointer.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt32' to 'UnsafeRawPointer' can be replaced with 'bitPattern:' initializer}}{{7-21=UnsafeRawPointer(bitPattern: UInt(}}{{23-51=))}}
  _ = unsafeBitCast(uu, to: UnsafeRawPointer.self)
  // expected-warning@+1{{'unsafeBitCast' from 'Int64' to 'UnsafeRawPointer' can be replaced with 'bitPattern:' initializer}}{{7-21=UnsafeRawPointer(bitPattern: Int(}}{{24-52=))}}
  _ = unsafeBitCast(iii, to: UnsafeRawPointer.self)
  // expected-warning@+1{{'unsafeBitCast' from 'UInt64' to 'UnsafeRawPointer' can be replaced with 'bitPattern:' initializer}}{{7-21=UnsafeRawPointer(bitPattern: UInt(}}{{24-52=))}}
  _ = unsafeBitCast(uuu, to: UnsafeRawPointer.self)
}

class GenericBase<T> {}

func bitcastingGenericType<A, B>(_: A, x: GenericBase<B>) {
  _ = unsafeBitCast(x, to: GenericBase<A>.self) // expected-no-warning
}

class Base {}

func bitcastingBaseType<A: Base, B: Base>(_: A, x: B) {
  _ = unsafeBitCast(x, to: A.self) // expected-no-warning
}
