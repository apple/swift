// RUN: %target-run-simple-swift

// REQUIRES: executable_test

//===----------------------------------------------------------------------===//
// Use of protocols with Self or associated type requirements
//===----------------------------------------------------------------------===//

protocol P {
  associatedtype Q
  func returnSelf() -> Self
  func takesNestedSelf(arg: (Self) -> ())
  func printANumber()
}

struct S: P {
  typealias Q = Int

  func returnSelf() -> Self { self }
  func takesNestedSelf(arg: (Self) -> ()) { arg(self) }
  func printANumber() { print("123") }
}

let p: P = S()

// CHECK: 123
p.returnSelf().printANumber()

// CHECK: 123
p.takesNestedSelf { $0.printANumber() }

// CHECK: 123
p.printANumber()

func takesP(arg: P) {
  // CHECK: 123
  p.returnSelf().printANumber()

  // CHECK: 123
  p.takesNestedSelf { $0.printANumber() }

  // CHECK: 123
  p.printANumber()
}

takesP(arg: p)

let pArray: [P] = [S()]
pArray.forEach {
  // CHECK: 123
  $0.returnSelf().printANumber()
}

// CHECK: true
print(S() is P)

// CHECK: true
print(p is P)

// CHECK: 123
(S() as P).printANumber()

//===----------------------------------------------------------------------===//
// Use of protocols with constrained associated types
//===----------------------------------------------------------------------===//

protocol MyIntCollection:
  BidirectionalCollection where Element == Int,
                                SubSequence == ArraySlice<Element>,
                                Index == Int {}
extension Array: MyIntCollection where Element == Int {}

let erasedIntArr: MyIntCollection = [5, 8, 1, 9, 3, 8]

// CHECK: 6
print(erasedIntArr.count)
// CHECK: 5
print(erasedIntArr[Int.zero])
// CHECK: 8
print(erasedIntArr.last.unsafelyUnwrapped)
// CHECK: [5, 8, 1, 9, 3]
print(erasedIntArr.dropLast())
// CHECK: [8, 3, 9, 1, 8, 5]
print(erasedIntArr.reversed())
// CHECK: 9
print(erasedIntArr.max().unsafelyUnwrapped)
// CHECK: [1, 3, 5, 8, 8, 9]
print(erasedIntArr.sorted())
// CHECK: false
print(erasedIntArr.contains(Int.zero))
// CHECK: [5, 8]
print(
  erasedIntArr[
    erasedIntArr.startIndex...erasedIntArr.firstIndex(
      of: erasedIntArr.last.unsafelyUnwrapped
    ).unsafelyUnwrapped
  ]
)

// FIXME: protocol != existential
//for element in erasedIntArr {}
