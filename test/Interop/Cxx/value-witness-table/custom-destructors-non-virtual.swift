// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop)
// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop -O)
//
// REQUIRES: executable_test

import CustomDestructor
import StdlibUnittest

var CXXDestructorTestSuite = TestSuite("CXXDestructor")

protocol InitWithPtr {
  init(_: UnsafeMutablePointer<Int32>!)
}

extension HasUserProvidedDestructor : InitWithPtr { }

protocol InitWithMember {
  init(member: HasUserProvidedDestructor)
}

extension HasEmptyDestructorAndMemberWithUserDefinedConstructor
  : InitWithMember { }

@inline(never)
@_optimize(none)
func withCxxDestructorSideEffects<T>(_ _: inout T) { }

func createTypeWithUserProvidedDestructor(_ ptr: UnsafeMutablePointer<Int32>) {
  var obj = HasUserProvidedDestructor(ptr)
  withCxxDestructorSideEffects(&obj)
}

func createTypeWithEmptyDestructorAndMemberWithUserDefinedConstructor(
  _ ptr: UnsafeMutablePointer<Int32>
) {
  let member = HasUserProvidedDestructor(ptr)
  var obj = HasEmptyDestructorAndMemberWithUserDefinedConstructor(member: member)
  withCxxDestructorSideEffects(&obj)
}

func createTypeWithNonTrivialImplicitDestructor(
  _ ptr: UnsafeMutablePointer<Int32>
) {
  let member = HasUserProvidedDestructor(ptr)
  var obj = HasNonTrivialImplicitDestructor(member: member)
  withCxxDestructorSideEffects(&obj)
}

func createTypeWithNonTrivialDefaultDestructor(
  _ ptr: UnsafeMutablePointer<Int32>
) {
  let member = HasUserProvidedDestructor(ptr)
  var obj = HasNonTrivialDefaultedDestructor(member: member)
  withCxxDestructorSideEffects(&obj)
}

func createTypeWithGeneric<T : InitWithPtr>(
  _ ptr: UnsafeMutablePointer<Int32>,
  type: T.Type
) {
  var obj = T(ptr)
  withCxxDestructorSideEffects(&obj)
}

func createTypeWithProtocol(
  _ ptr: UnsafeMutablePointer<Int32>,
  type: InitWithPtr.Type
) {
  var obj = type.self.init(ptr)
  withCxxDestructorSideEffects(&obj)
}

func createTypeWithProtocol(
  _ ptr: UnsafeMutablePointer<Int32>,
  type: InitWithPtr.Type,
  holder: InitWithMember.Type
) {
  let member = type.self.init(ptr)
  var obj = holder.self.init(member: member as! HasUserProvidedDestructor)
  withCxxDestructorSideEffects(&obj)
}

CXXDestructorTestSuite.test("Basic object with destructor") {
  var value: Int32 = 0
  createTypeWithUserProvidedDestructor(&value)
  expectEqual(value, 42)
}

CXXDestructorTestSuite.test("Nested objects with destructors") {
  var value: Int32 = 0
  createTypeWithEmptyDestructorAndMemberWithUserDefinedConstructor(&value)
  expectEqual(value, 42)
}

CXXDestructorTestSuite.test("Implicit destructor, member with user-defined destructor") {
  var value: Int32 = 0
  createTypeWithNonTrivialImplicitDestructor(&value)
  expectEqual(value, 42)
}

CXXDestructorTestSuite.test("Default destructor, member with user-defined destructor") {
  var value: Int32 = 0
  createTypeWithNonTrivialDefaultDestructor(&value)
  expectEqual(value, 42)
}

CXXDestructorTestSuite.test("Generic with destructor") {
  var value: Int32 = 0
  createTypeWithGeneric(&value, type: HasUserProvidedDestructor.self)
  expectEqual(value, 42)
}

CXXDestructorTestSuite.test("Protocol with destructor") {
  var value: Int32 = 0
  createTypeWithProtocol(&value, type: HasUserProvidedDestructor.self)
  expectEqual(value, 42)
}

CXXDestructorTestSuite.test("Protocol with member with destructor") {
  var value: Int32 = 0
  createTypeWithProtocol(
    &value,
    type: HasUserProvidedDestructor.self,
    holder: HasEmptyDestructorAndMemberWithUserDefinedConstructor.self)
  expectEqual(value, 42)
}

runAllTests()
