// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -cxx-interoperability-mode=swift-5.9)
// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -cxx-interoperability-mode=swift-6)
// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -cxx-interoperability-mode=upcoming-swift)
//
// REQUIRES: executable_test

import MemberInline
import StdlibUnittest

var OperatorsTestSuite = TestSuite("Operators")

OperatorsTestSuite.test("LoadableIntWrapper.minus (inline)") {
  var lhs = LoadableIntWrapper(value: 42)
  let rhs = LoadableIntWrapper(value: 23)

  let result = lhs - rhs

  expectEqual(19, result.value)
}

OperatorsTestSuite.test("AddressOnlyIntWrapper.minus") {
   let lhs = AddressOnlyIntWrapper(42)
   let rhs = AddressOnlyIntWrapper(23)

   let result = lhs - rhs

   expectEqual(19, result.value)
}

OperatorsTestSuite.test("LoadableIntWrapper.equal (inline)") {
  let lhs = LoadableIntWrapper(value: 42)
  let rhs = LoadableIntWrapper(value: 42)

  let result = lhs == rhs

  expectTrue(result)
}

OperatorsTestSuite.test("LoadableIntWrapper.plusEqual (inline)") {
  var lhs = LoadableIntWrapper(value: 42)
  let rhs = LoadableIntWrapper(value: 42)

  lhs += rhs

  expectEqual(lhs.value, 84)
}

OperatorsTestSuite.test("LoadableIntWrapper.minusEqual (inline)") {
  var lhs = LoadableIntWrapper(value: 42)
  let rhs = LoadableIntWrapper(value: 42)

  lhs -= rhs

  expectEqual(lhs.value, 0)
}

OperatorsTestSuite.test("LoadableIntWrapper.unaryMinus (inline)") {
  let lhs = LoadableIntWrapper(value: 42)
  let inverseLhs = -lhs;
  expectEqual(-42, inverseLhs.value)
}

OperatorsTestSuite.test("LoadableIntWrapper.call (inline)") {
  var wrapper = LoadableIntWrapper(value: 42)

  let resultNoArgs = wrapper()
  let resultOneArg = wrapper(23)
  let resultTwoArgs = wrapper(3, 5)

  expectEqual(42, resultNoArgs)
  expectEqual(65, resultOneArg)
  expectEqual(57, resultTwoArgs)
}

OperatorsTestSuite.test("LoadableIntWrapper.successor() (inline)") {
  var wrapper = LoadableIntWrapper(value: 42)

  let result1 = wrapper.successor()
  expectEqual(43, result1.value)
  expectEqual(42, wrapper.value) // Calling `successor()` should not mutate `wrapper`.

  let result2 = result1.successor()
  expectEqual(44, result2.value)
  expectEqual(43, result1.value)
  expectEqual(42, wrapper.value)
}

OperatorsTestSuite.test("IntWrapperInNamespace.equal (inline)") {
  let lhs = NS.IntWrapperInNamespace(value: 42)
  let rhs = NS.IntWrapperInNamespace(value: 42)

  let result = lhs == rhs

  expectTrue(result)
}

OperatorsTestSuite.test("TemplatedWithFriendOperator.equal (inline)") {
  let lhs = TemplatedWithFriendOperatorSpec()
  let rhs = TemplatedWithFriendOperatorSpec()

  let result = lhs == rhs

  expectTrue(result)
}

OperatorsTestSuite.test("LoadableBoolWrapper.exclaim (inline)") {
  var wrapper = LoadableBoolWrapper(value: true)

  let resultExclaim = !wrapper
  expectEqual(false, resultExclaim.value)
}

OperatorsTestSuite.test("AddressOnlyIntWrapper.call (inline)") {
  var wrapper = AddressOnlyIntWrapper(42)

  let resultNoArgs = wrapper()
  let resultOneArg = wrapper(23)
  let resultTwoArgs = wrapper(3, 5)

  expectEqual(42, resultNoArgs)
  expectEqual(65, resultOneArg)
  expectEqual(57, resultTwoArgs)
}

OperatorsTestSuite.test("AddressOnlyIntWrapper.successor() (inline)") {
  var wrapper = AddressOnlyIntWrapper(0)

  let result1 = wrapper.successor()
  expectEqual(1, result1.value)
  expectEqual(0, wrapper.value) // Calling `successor()` should not mutate `wrapper`.

  let result2 = result1.successor()
  expectEqual(2, result2.value)
  expectEqual(1, result1.value)
  expectEqual(0, wrapper.value)
}

OperatorsTestSuite.test("HasPreIncrementOperatorWithAnotherReturnType.successor() (inline)") {
  var wrapper = HasPreIncrementOperatorWithAnotherReturnType()

  let result1 = wrapper.successor()
  expectEqual(1, result1.value)
  expectEqual(0, wrapper.value) // Calling `successor()` should not mutate `wrapper`.

  let result2 = result1.successor()
  expectEqual(2, result2.value)
  expectEqual(1, result1.value)
  expectEqual(0, wrapper.value)
}

OperatorsTestSuite.test("HasPreIncrementOperatorWithVoidReturnType.successor() (inline)") {
  var wrapper = HasPreIncrementOperatorWithVoidReturnType()

  let result1 = wrapper.successor()
  expectEqual(1, result1.value)
  expectEqual(0, wrapper.value) // Calling `successor()` should not mutate `wrapper`.

  let result2 = result1.successor()
  expectEqual(2, result2.value)
  expectEqual(1, result1.value)
  expectEqual(0, wrapper.value)
}

OperatorsTestSuite.test("DerivedFromAddressOnlyIntWrapper.call (inline, base class)") {
  var wrapper = DerivedFromAddressOnlyIntWrapper(42)

  let resultNoArgs = wrapper()
  let resultOneArg = wrapper(23)
  let resultTwoArgs = wrapper(3, 5)

  expectEqual(42, resultNoArgs)
  expectEqual(65, resultOneArg)
  expectEqual(57, resultTwoArgs)
}

OperatorsTestSuite.test("ReadWriteIntArray.subscript (inline)") {
  var arr = ReadWriteIntArray()

  let resultBefore = arr[1]
  expectEqual(2, resultBefore)

  arr[1] = 234

  let resultAfter = arr[1]
  expectEqual(234, resultAfter)
}

OperatorsTestSuite.test("DerivedFromReadWriteIntArray.subscript (inline, base class)") {
  var arr = DerivedFromReadWriteIntArray()

  let resultBefore = arr[1]
  expectEqual(2, resultBefore)

  arr[1] = 234

  let resultAfter = arr[1]
  expectEqual(234, resultAfter)
}

OperatorsTestSuite.test("ReadOnlyIntArray.subscript (inline)") {
  let arr = ReadOnlyIntArray(1)

  let result0 = arr[0]
  let result2 = arr[2]
  let result4 = arr[4]

  expectEqual(1, result0)
  expectEqual(3, result2)
  expectEqual(5, result4)
}

OperatorsTestSuite.test("WriteOnlyIntArray.subscript (inline)") {
  var arr = WriteOnlyIntArray()

  let resultBefore = arr[0]
  expectEqual(1, resultBefore)

  arr[0] = 654

  let resultAfter = arr[0]
  expectEqual(654, resultAfter)
}

OperatorsTestSuite.test("DifferentTypesArray.subscript (inline)") {
  let arr = DifferentTypesArray()

  let resultInt: Int32 = arr[2]
  let resultDouble: Double = arr[0.1]

  expectEqual(3, resultInt)
  expectEqual(1.5.rounded(.down), resultDouble.rounded(.down))
  expectEqual(1.5.rounded(.up), resultDouble.rounded(.up))
}

OperatorsTestSuite.test("IntArrayByVal.subscript (inline)") {
  var arr = IntArrayByVal()
  let result0 = arr[0]
  let result1 = arr[1]
  let result2 = arr[2]

  expectEqual(1, result0)
  expectEqual(2, result1)
  expectEqual(3, result2)

  arr.setValueAtIndex(42, 2)
  let result3 = arr[2]
  expectEqual(42, result3)
}

OperatorsTestSuite.test("NonTrivialIntArrayByVal.subscript (inline)") {
  var arr = NonTrivialIntArrayByVal(1)

  let result0 = arr[0]
  let result2 = arr[2]
  let result4 = arr[4]

  expectEqual(1, result0)
  expectEqual(3, result2)
  expectEqual(5, result4)

  arr.setValueAtIndex(42, 2)
  let result5 = arr[2]
  expectEqual(42, result5)
}

OperatorsTestSuite.test("DifferentTypesArrayByVal.subscript (inline)") {
  var arr = DifferentTypesArrayByVal()

  let resultInt: Int32 = arr[2]
  let resultDouble: Double = arr[0.1]

  expectEqual(3, resultInt)
  expectEqual(1.5.rounded(.down), resultDouble.rounded(.down))
  expectEqual(1.5.rounded(.up), resultDouble.rounded(.up))
}

OperatorsTestSuite.test("NonTrivialArrayByVal.subscript (inline)") {
  var arr = NonTrivialArrayByVal()
  let NonTrivialByVal = arr[0];
  let cStr = NonTrivialByVal.Str!
  expectEqual("Non-Trivial", String(cString: cStr))

  expectEqual(1, NonTrivialByVal.a)
  expectEqual(2, NonTrivialByVal.b)
  expectEqual(3, NonTrivialByVal.c)
  expectEqual(4, NonTrivialByVal.d)
  expectEqual(5, NonTrivialByVal.e)
  expectEqual(6, NonTrivialByVal.f)
}

OperatorsTestSuite.test("DerivedFromNonTrivialArrayByVal.subscript (inline, base class)") {
  var arr = DerivedFromNonTrivialArrayByVal()
  let NonTrivialByVal = arr[0];
  let cStr = NonTrivialByVal.Str!
  expectEqual("Non-Trivial", String(cString: cStr))

  expectEqual(1, NonTrivialByVal.a)
  expectEqual(2, NonTrivialByVal.b)
  expectEqual(3, NonTrivialByVal.c)
  expectEqual(4, NonTrivialByVal.d)
  expectEqual(5, NonTrivialByVal.e)
  expectEqual(6, NonTrivialByVal.f)
}

OperatorsTestSuite.test("PtrByVal.subscript (inline)") {
  var arr = PtrByVal()
  expectEqual(64, arr[0]![0])
  arr[0]![0] = 23
  expectEqual(23, arr[0]![0])
}

OperatorsTestSuite.test("ConstOpPtrByVal.subscript (inline)") {
  var arr = ConstOpPtrByVal()
  expectEqual(64, arr[0]![0])
}

OperatorsTestSuite.test("ConstPtrByVal.subscript (inline)") {
  var arr = ConstPtrByVal()
  expectEqual(64, arr[0]![0])
}

OperatorsTestSuite.test("RefToPtr.subscript (inline)") {
  var arr = RefToPtr()
  let ptr: UnsafeMutablePointer<Int32> =
    UnsafeMutablePointer<Int32>.allocate(capacity: 64)
  ptr[0] = 23

  expectEqual(64, arr[0]![0])
  arr[0] = ptr
  expectEqual(23, arr[0]![0])
}

OperatorsTestSuite.test("PtrToPtr.subscript (inline)") {
  var arr = PtrToPtr()
  let ptr: UnsafeMutablePointer<Int32> =
    UnsafeMutablePointer<Int32>.allocate(capacity: 64)
  ptr[0] = 23

  expectEqual(64, arr[0]![0]![0])
  arr[0]![0] = ptr
  expectEqual(23, arr[0]![0]![0])
}

// TODO: this causes a crash (does it also crash on main?)
//OperatorsTestSuite.test("TemplatedSubscriptArrayByVal.subscript (inline)") {
//  let ptr: UnsafeMutablePointer<Int32> =
//    UnsafeMutablePointer<Int32>.allocate(capacity: 64)
//  ptr[0] = 23
//  var arr = TemplatedSubscriptArrayByVal(ptr: ptr)
//  expectEqual(23, arr[0])
//}

OperatorsTestSuite.test("Iterator.pointee") {
  var iter = Iterator()
  let res = iter.pointee
  expectEqual(123, res)

  iter.pointee = 456
  expectEqual(456, iter.pointee)
}

OperatorsTestSuite.test("ConstIterator.pointee") {
  let iter = ConstIterator()
  let res = iter.pointee
  expectEqual(234, res)
}

OperatorsTestSuite.test("ConstIteratorByVal.pointee") {
  let iter = ConstIteratorByVal()
  let res = iter.pointee
  expectEqual(456, res)
}

OperatorsTestSuite.test("AmbiguousOperatorStar.pointee") {
  let stars = AmbiguousOperatorStar()
  let res = stars.pointee
  expectEqual(567, res)
}

OperatorsTestSuite.test("AmbiguousOperatorStar2.pointee") {
  let stars = AmbiguousOperatorStar2()
  let res = stars.pointee
  expectEqual(678, res)
}

OperatorsTestSuite.test("DerivedFromConstIterator.pointee") {
  let stars = DerivedFromConstIterator()
  let res = stars.pointee
  expectEqual(234, res)
}

OperatorsTestSuite.test("SubscriptSetterConst") {
  var setterConst = SubscriptSetterConst()
  setterConst[0] = 10
}

OperatorsTestSuite.test("SubscriptUnnamedParameter") {
  let unnamed = SubscriptUnnamedParameter()
  expectEqual(123, unnamed[0])
  expectEqual(123, unnamed[321])
}

OperatorsTestSuite.test("SubscriptUnnamedParameterReadWrite") {
  var unnamed = SubscriptUnnamedParameterReadWrite()
  expectEqual(0, unnamed[0])
  expectEqual(0, unnamed[321])

  unnamed[456] = 456
  expectEqual(456, unnamed[0])
  expectEqual(456, unnamed[321])
}

OperatorsTestSuite.test("DerivedFromConstIteratorPrivatelyWithUsingDecl.pointee") {
  let stars = DerivedFromConstIteratorPrivatelyWithUsingDecl()
  let res = stars.pointee
  expectEqual(234, res)
}

OperatorsTestSuite.test("DerivedFromAmbiguousOperatorStarPrivatelyWithUsingDecl.pointee") {
  let stars = DerivedFromAmbiguousOperatorStarPrivatelyWithUsingDecl()
  let res = stars.pointee
  expectEqual(567, res)
}

OperatorsTestSuite.test("DerivedFromLoadableIntWrapperWithUsingDecl") {
  var d = DerivedFromLoadableIntWrapperWithUsingDecl()
  d.setValue(123)
  var d1 = LoadableIntWrapper()
  d1.value = 543
  d += d1
  expectEqual(666, d.getValue())
}

OperatorsTestSuite.test("HasOperatorCallWithDefaultArg.call") {
  let h = HasOperatorCallWithDefaultArg(value: 321)
  let res = h(123)
  expectEqual(444, res)
}

OperatorsTestSuite.test("HasStaticOperatorCallBase.call") {
  let h = HasStaticOperatorCallBase()
  let res = h(1)
  expectEqual(43, res)
}

OperatorsTestSuite.test("HasStaticOperatorCallDerived.call") {
  let h = HasStaticOperatorCallDerived()
  let res = h(0)
  expectEqual(42, res)
}

OperatorsTestSuite.test("HasStaticOperatorCallWithConstOperator.call") {
  let h = HasStaticOperatorCallWithConstOperator()
  let res = h(10)
  expectEqual(9, res)
  let res2 = h(3, 5)
  expectEqual(8, res2)
}

runAllTests()
