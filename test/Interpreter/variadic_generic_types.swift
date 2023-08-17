// RUN: %target-run-simple-swift(-Xfrontend -disable-concrete-type-metadata-mangled-name-accessors -Xfrontend -disable-availability-checking)
// RUN: %target-run-simple-swift(-Xfrontend -disable-availability-checking)

// REQUIRES: executable_test

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import StdlibUnittest

var types = TestSuite("VariadicGenericTypes")

//
// Metadata instantiation tests
//

public struct Outer<each U> {
  public struct Inner<each V> {}

  public struct InnerSameShape<each V> where (repeat (each U, each V)): Any {}
}

func makeMetatype<each T>(_: repeat (each T).Type) -> Any.Type {
  return Outer<repeat each T>.self
}

func blackHole<T>(_: T) {}

types.test("OuterRepeated") {
  // Instantiate a type containing type parameters to avoid caching
  blackHole(makeMetatype())
  blackHole(makeMetatype())
  blackHole(makeMetatype(Int.self))
  blackHole(makeMetatype(Int.self))
  blackHole(makeMetatype(String.self, Substring.self))
  blackHole(makeMetatype(String.self, Substring.self))
}

types.test("Outer") {
  expectEqual("main.Outer<Pack{}>", _typeName(Outer< >.self))
  expectEqual("main.Outer<Pack{Swift.Int}>", _typeName(Outer<Int>.self))
  expectEqual("main.Outer<Pack{Swift.Int, Swift.String}>", _typeName(Outer<Int, String>.self))
  expectEqual("main.Outer<Pack{Swift.Int, Swift.String, Swift.Float}>", _typeName(Outer<Int, String, Float>.self))
}

types.test("Outer.Inner") {
  expectEqual("main.Outer<Pack{}>.Inner<Pack{}>", _typeName(Outer< >.Inner< >.self))
  expectEqual("main.Outer<Pack{Swift.Int}>.Inner<Pack{}>", _typeName(Outer<Int>.Inner< >.self))
  expectEqual("main.Outer<Pack{Swift.Int, Swift.String}>.Inner<Pack{}>", _typeName(Outer<Int, String>.Inner< >.self))
  expectEqual("main.Outer<Pack{Swift.Int, Swift.String, Swift.Float}>.Inner<Pack{}>", _typeName(Outer<Int, String, Float>.Inner< >.self))

  expectEqual("main.Outer<Pack{}>.Inner<Pack{Swift.Bool}>", _typeName(Outer< >.Inner<Bool>.self))
  expectEqual("main.Outer<Pack{Swift.Int}>.Inner<Pack{Swift.Bool}>", _typeName(Outer<Int>.Inner<Bool>.self))
  expectEqual("main.Outer<Pack{Swift.Int, Swift.String}>.Inner<Pack{Swift.Bool}>", _typeName(Outer<Int, String>.Inner<Bool>.self))
  expectEqual("main.Outer<Pack{Swift.Int, Swift.String, Swift.Float}>.Inner<Pack{Swift.Bool}>", _typeName(Outer<Int, String, Float>.Inner<Bool>.self))

  expectEqual("main.Outer<Pack{}>.Inner<Pack{Swift.Bool, Swift.Double}>", _typeName(Outer< >.Inner<Bool, Double>.self))
  expectEqual("main.Outer<Pack{Swift.Int}>.Inner<Pack{Swift.Bool, Swift.Double}>", _typeName(Outer<Int>.Inner<Bool, Double>.self))
  expectEqual("main.Outer<Pack{Swift.Int, Swift.String}>.Inner<Pack{Swift.Bool, Swift.Double}>", _typeName(Outer<Int, String>.Inner<Bool, Double>.self))
  expectEqual("main.Outer<Pack{Swift.Int, Swift.String, Swift.Float}>.Inner<Pack{Swift.Bool, Swift.Double}>", _typeName(Outer<Int, String, Float>.Inner<Bool, Double>.self))

  expectEqual("main.Outer<Pack{}>.Inner<Pack{Swift.Bool, Swift.Double, Swift.Character}>", _typeName(Outer< >.Inner<Bool, Double, Character>.self))
  expectEqual("main.Outer<Pack{Swift.Int}>.Inner<Pack{Swift.Bool, Swift.Double, Swift.Character}>", _typeName(Outer<Int>.Inner<Bool, Double, Character>.self))
  expectEqual("main.Outer<Pack{Swift.Int, Swift.String}>.Inner<Pack{Swift.Bool, Swift.Double, Swift.Character}>", _typeName(Outer<Int, String>.Inner<Bool, Double, Character>.self))
  expectEqual("main.Outer<Pack{Swift.Int, Swift.String, Swift.Float}>.Inner<Pack{Swift.Bool, Swift.Double, Swift.Character}>", _typeName(Outer<Int, String, Float>.Inner<Bool, Double, Character>.self))
}

types.test("Outer.InnerSameShape") {
  expectEqual("main.Outer<Pack{}>.InnerSameShape<Pack{}>", _typeName(Outer< >.InnerSameShape< >.self))
  expectEqual("main.Outer<Pack{Swift.Int}>.InnerSameShape<Pack{Swift.Bool}>", _typeName(Outer<Int>.InnerSameShape<Bool>.self))
  expectEqual("main.Outer<Pack{Swift.Int, Swift.String}>.InnerSameShape<Pack{Swift.Bool, Swift.Double}>", _typeName(Outer<Int, String>.InnerSameShape<Bool, Double>.self))
  expectEqual("main.Outer<Pack{Swift.Int, Swift.String, Swift.Float}>.InnerSameShape<Pack{Swift.Bool, Swift.Double, Swift.Character}>", _typeName(Outer<Int, String, Float>.InnerSameShape<Bool, Double, Character>.self))
}

public struct ConformanceReq<each T: Equatable> {}

types.test("ConformanceReq") {
  expectEqual("main.ConformanceReq<Pack{}>", _typeName(ConformanceReq< >.self))
  expectEqual("main.ConformanceReq<Pack{Swift.Int}>", _typeName(ConformanceReq<Int>.self))
  expectEqual("main.ConformanceReq<Pack{Swift.Int, Swift.String}>", _typeName(ConformanceReq<Int, String>.self))
  expectEqual("main.ConformanceReq<Pack{Swift.Int, Swift.String, Swift.Float}>", _typeName(ConformanceReq<Int, String, Float>.self))
}

public class Base {}
public class Derived: Base {}

public struct SuperclassReq<each T: Base> {}

types.test("SuperclassReq") {
  expectEqual("main.SuperclassReq<Pack{}>", _typeName(SuperclassReq< >.self))
  expectEqual("main.SuperclassReq<Pack{main.Base}>", _typeName(SuperclassReq<Base>.self))
  expectEqual("main.SuperclassReq<Pack{main.Derived, main.Base}>", _typeName(SuperclassReq<Derived, Base>.self))
}

public struct LayoutReq<each T: AnyObject> {}

types.test("LayoutReq") {
  expectEqual("main.LayoutReq<Pack{}>", _typeName(LayoutReq< >.self))
  expectEqual("main.LayoutReq<Pack{Swift.AnyObject}>", _typeName(LayoutReq<AnyObject>.self))
  expectEqual("main.LayoutReq<Pack{Swift.AnyObject, main.Base}>", _typeName(LayoutReq<AnyObject, Base>.self))
}

public struct OuterSeq<each T: Sequence> {
  public struct InnerSeq<each U: Sequence> where repeat (each T).Element == (each U).Element {}
}

types.test("SameTypeReq") {
  expectEqual("main.OuterSeq<Pack{}>.InnerSeq<Pack{}>", _typeName(OuterSeq< >.InnerSeq< >.self))
  expectEqual("main.OuterSeq<Pack{Swift.Array<Swift.Int>}>.InnerSeq<Pack{Swift.Set<Swift.Int>}>", _typeName(OuterSeq<Array<Int>>.InnerSeq<Set<Int>>.self))
  expectEqual("main.OuterSeq<Pack{Swift.Array<Swift.Int>, Swift.Set<Swift.String>}>.InnerSeq<Pack{Swift.Set<Swift.Int>, Swift.Array<Swift.String>}>", _typeName(OuterSeq<Array<Int>, Set<String>>.InnerSeq<Set<Int>, Array<String>>.self))
}


//
// Stored property layout tests
//

public struct FancyTuple<each T> {
  private var x: (repeat each T)
}

public func returnSize<T>(_: T.Type) -> Int {
  return MemoryLayout<T>.size
}

types.test("FancyTuple") {
  expectEqual(returnSize(FancyTuple< >.self),
              returnSize(Void.self))
  expectEqual(returnSize(FancyTuple<Int8>.self),
              returnSize((Int8).self))
  expectEqual(returnSize(FancyTuple<Int8, Int16>.self),
              returnSize((Int8, Int16).self))
  expectEqual(returnSize(FancyTuple<Int8, Int16, Int32>.self),
              returnSize((Int8, Int16, Int32).self))
  expectEqual(returnSize(FancyTuple<Int8, Int16, Int32, Int64>.self),
              returnSize((Int8, Int16, Int32, Int64).self))
}

public struct SequenceElementTuple<each T: Sequence> {
  private var x: (repeat (each T).Element)
}

types.test("SequenceElementTuple") {
  expectEqual(returnSize(SequenceElementTuple< >.self),
              returnSize(Void.self))
  expectEqual(returnSize(SequenceElementTuple<Array<Int8>>.self),
              returnSize((Int8).self))
  expectEqual(returnSize(SequenceElementTuple<Array<Int8>, Array<Int16>>.self),
              returnSize((Int8, Int16).self))
  expectEqual(returnSize(SequenceElementTuple<Array<Int8>, Array<Int16>, Array<Int32>>.self),
              returnSize((Int8, Int16, Int32).self))
  expectEqual(returnSize(SequenceElementTuple<Array<Int8>, Array<Int16>, Array<Int32>, Array<Int64>>.self),
              returnSize((Int8, Int16, Int32, Int64).self))
}

runAllTests()
