// RUN: %target-swift-frontend -enforce-exclusivity=checked -swift-version 4 -emit-sil -primary-file %s -o /dev/null -verify

import Swift

func takesTwoInouts<T>(_ p1: inout T, _ p2: inout T) { }

func simpleInoutDiagnostic() {
  var i = 7

  // FIXME: This diagnostic should be removed if static enforcement is
  // turned on by default.
  // expected-error@+4{{inout arguments are not allowed to alias each other}}
  // expected-note@+3{{previous aliasing argument}}
  // expected-error@+2{{overlapping accesses to 'i', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@+1{{conflicting access is here}}
  takesTwoInouts(&i, &i)
}

func inoutOnInoutParameter(p: inout Int) {
  // expected-error@+4{{inout arguments are not allowed to alias each other}}
  // expected-note@+3{{previous aliasing argument}}
  // expected-error@+2{{overlapping accesses to 'p', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@+1{{conflicting access is here}}
  takesTwoInouts(&p, &p)
}

func swapNoSuppression(_ i: Int, _ j: Int) {
  var a: [Int] = [1, 2, 3]

  // expected-error@+2{{overlapping accesses to 'a', but modification requires exclusive access; consider calling MutableCollection.swapAt(_:_:)}}
  // expected-note@+1{{conflicting access is here}}
  swap(&a[i], &a[j])
}

class SomeClass { }

struct StructWithMutatingMethodThatTakesSelfInout {
  var f = SomeClass()
  mutating func mutate(_ other: inout StructWithMutatingMethodThatTakesSelfInout) { }
  mutating func mutate(_ other: inout SomeClass) { }

  mutating func callMutatingMethodThatTakesSelfInout() {
    // expected-error@+4{{inout arguments are not allowed to alias each other}}
    // expected-note@+3{{previous aliasing argument}}
    // expected-error@+2{{overlapping accesses to 'self', but modification requires exclusive access; consider copying to a local variable}}
    // expected-note@+1{{conflicting access is here}}
    mutate(&self)
  }

  mutating func callMutatingMethodThatTakesSelfStoredPropInout() {
    // expected-error@+2{{overlapping accesses to 'self', but modification requires exclusive access; consider copying to a local variable}}
    // expected-note@+1{{conflicting access is here}}
    mutate(&self.f)
  }
}

var globalStruct1 = StructWithMutatingMethodThatTakesSelfInout()
func callMutatingMethodThatTakesGlobalStoredPropInout() {
  // expected-error@+2{{overlapping accesses to 'globalStruct1', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@+1{{conflicting access is here}}
  globalStruct1.mutate(&globalStruct1.f)
}

class ClassWithFinalStoredProp {
  final var s1: StructWithMutatingMethodThatTakesSelfInout = StructWithMutatingMethodThatTakesSelfInout()
  final var s2: StructWithMutatingMethodThatTakesSelfInout = StructWithMutatingMethodThatTakesSelfInout()
  final var i = 7

  func callMutatingMethodThatTakesClassStoredPropInout() {
    s1.mutate(&s2.f) // no-warning

    // expected-error@+2{{overlapping accesses to 's1', but modification requires exclusive access; consider copying to a local variable}}
    // expected-note@+1{{conflicting access is here}}
    s1.mutate(&s1.f)

    let local1 = self

    // expected-error@+2{{overlapping accesses to 's1', but modification requires exclusive access; consider copying to a local variable}}
    // expected-note@+1{{conflicting access is here}}
    local1.s1.mutate(&local1.s1.f)
  }
}

func violationWithGenericType<T>(_ p: T) {
  var local = p
  // expected-error@+4{{inout arguments are not allowed to alias each other}}
  // expected-note@+3{{previous aliasing argument}}
  // expected-error@+2{{overlapping accesses to 'local', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@+1{{conflicting access is here}}
  takesTwoInouts(&local, &local)
}


// Helper.
struct StructWithTwoStoredProp {
  var f1: Int = 7
  var f2: Int = 8
}

// Take an unsafe pointer to a stored property while accessing another stored property.
func violationWithUnsafePointer(_ s: inout StructWithTwoStoredProp) {
  withUnsafePointer(to: &s.f1) { (ptr) in
    // expected-error@-1 {{overlapping accesses to 's.f1', but modification requires exclusive access; consider copying to a local variable}}
    _ = s.f1
    // expected-note@-1 {{conflicting access is here}}
  }

  // Statically treat accesses to separate stored properties in structs as
  // accessing separate storage.
  withUnsafePointer(to: &s.f1) { (ptr) in // no-error
    _ = s.f2
  }
}
// Tests for Fix-Its to replace swap(&collection[a], &collection[b]) with
// collection.swapAt(a, b)

struct StructWithField {
  var f = 12
}

struct StructWithFixits {
  var arrayProp: [Int] = [1, 2, 3]
  var dictionaryProp: [Int : Int] = [0 : 10, 1 : 11]

  mutating
  func shouldHaveFixIts<T>(_ i: Int, _ j: Int, _ param: T, _ paramIndex: T.Index) where T : MutableCollection {
    var array1 = [1, 2, 3]
    // expected-error@+2{{overlapping accesses}}{{5-41=array1.swapAt(i + 5, j - 2)}}
    // expected-note@+1{{conflicting access is here}}
    swap(&array1[i + 5], &array1[j - 2])

    // expected-error@+2{{overlapping accesses}}{{5-49=self.arrayProp.swapAt(i, j)}}
    // expected-note@+1{{conflicting access is here}}
    swap(&self.arrayProp[i], &self.arrayProp[j])

    var localOfGenericType = param
    // expected-error@+2{{overlapping accesses}}{{5-75=localOfGenericType.swapAt(paramIndex, paramIndex)}}
    // expected-note@+1{{conflicting access is here}}
    swap(&localOfGenericType[paramIndex], &localOfGenericType[paramIndex])

    // expected-error@+2{{overlapping accesses}}{{5-39=array1.swapAt(i, j)}}
    // expected-note@+1{{conflicting access is here}}
    Swift.swap(&array1[i], &array1[j]) // no-crash
  }

  mutating
  func shouldHaveNoFixIts(_ i: Int, _ j: Int) {
    var s = StructWithField()
    // expected-error@+2{{overlapping accesses}}{{none}}
    // expected-note@+1{{conflicting access is here}}
    swap(&s.f, &s.f)

    var array1 = [1, 2, 3]
    var array2 = [1, 2, 3]

    // Swapping between different arrays should cannot have the
    // Fix-It.
    swap(&array1[i], &array2[j]) // no-warning no-fixit
    swap(&array1[i], &self.arrayProp[j]) // no-warning no-fixit

    // Dictionaries aren't MutableCollections so don't support swapAt().
    // expected-error@+2{{overlapping accesses}}{{none}}
    // expected-note@+1{{conflicting access is here}}
    swap(&dictionaryProp[i], &dictionaryProp[j])

    // We could safely Fix-It this but don't now because the left and
    // right bases are not textually identical.
    // expected-error@+2{{overlapping accesses}}{{none}}
    // expected-note@+1{{conflicting access is here}}
    swap(&self.arrayProp[i], &arrayProp[j])

    // We could safely Fix-It this but we're not that heroic.
    // We don't suppress when swap() is used as a value
    let mySwap: (inout Int, inout Int) -> () = swap

    // expected-error@+2{{overlapping accesses}}{{none}}
    // expected-note@+1{{conflicting access is here}}
    mySwap(&array1[i], &array1[j])

    func myOtherSwap<T>(_ a: inout T, _ b: inout T) {
      swap(&a, &b) // no-warning
    }

    // expected-error@+2{{overlapping accesses}}{{none}}
    // expected-note@+1{{conflicting access is here}}
    mySwap(&array1[i], &array1[j])
  }
}

func takesInoutAndNoEscapeClosure<T>(_ p: inout T, _ c: () -> ()) { }

func callsTakesInoutAndNoEscapeClosure() {
  var local = 5
  takesInoutAndNoEscapeClosure(&local) { // expected-error {{overlapping accesses to 'local', but modification requires exclusive access; consider copying to a local variable}}
    local = 8  // expected-note {{conflicting access is here}}
  }
}

func inoutReadWriteInout(x: inout Int) {
  // expected-error@+2{{overlapping accesses to 'x', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@+1{{conflicting access is here}}
  takesInoutAndNoEscapeClosure(&x, { _ = x })
}

func inoutWriteWriteInout(x: inout Int) {
  // expected-error@+2{{overlapping accesses to 'x', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@+1{{conflicting access is here}}
  takesInoutAndNoEscapeClosure(&x, { x = 42 })
}

func callsTakesInoutAndNoEscapeClosureWithRead() {
  var local = 5
  takesInoutAndNoEscapeClosure(&local) { // expected-error {{overlapping accesses to 'local', but modification requires exclusive access; consider copying to a local variable}}
    _ = local  // expected-note {{conflicting access is here}}
  }
}

func takesInoutAndNoEscapeClosureThatThrows<T>(_ p: inout T, _ c: () throws -> ()) { }

func callsTakesInoutAndNoEscapeClosureThatThrowsWithNonThrowingClosure() {
  var local = 5
  takesInoutAndNoEscapeClosureThatThrows(&local) { // expected-error {{overlapping accesses to 'local', but modification requires exclusive access; consider copying to a local variable}}
    local = 8  // expected-note {{conflicting access is here}}
  }
}

func takesInoutAndNoEscapeClosureAndThrows<T>(_ p: inout T, _ c: () -> ()) throws { }

func callsTakesInoutAndNoEscapeClosureAndThrows() {
  var local = 5
  try! takesInoutAndNoEscapeClosureAndThrows(&local) { // expected-error {{overlapping accesses to 'local', but modification requires exclusive access; consider copying to a local variable}}
    local = 8  // expected-note {{conflicting access is here}}
  }
}

func takesTwoNoEscapeClosures(_ c1: () -> (), _ c2: () -> ()) { }

func callsTakesTwoNoEscapeClosures() {
  var local = 7
  takesTwoNoEscapeClosures({local = 8}, {local = 9}) // no-error
  _ = local
}

func takesInoutAndEscapingClosure<T>(_ p: inout T, _ c: @escaping () -> ()) { }

func callsTakesInoutAndEscapingClosure() {
  var local = 5
  takesInoutAndEscapingClosure(&local) { // no-error
    local = 8
  }
}

func callsClosureLiteralImmediately() {
  var i = 7;
  // Closure literals that are called immediately are considered nonescaping
  _ = ({ (p: inout Int) in
         i
         // expected-note@-1 {{conflicting access is here}}
       }
      )(&i)
  // expected-error@-1 {{overlapping accesses to 'i', but modification requires exclusive access; consider copying to a local variable}}
}

func callsStoredClosureLiteral() {
  var i = 7;
  let c = { (p: inout Int) in i}

  // Closure literals that are stored and later called are treated as escaping
  // We don't expect a static exclusivity diagnostic here, but the issue
  // will be caught at run time
  _ = c(&i) // no-error
}


// Calling this with an inout expression for the first parameter performs a
// read access for the duration of a call
func takesUnsafePointerAndNoEscapeClosure<T>(_ p: UnsafePointer<T>, _ c: () -> ()) { }

// Calling this with an inout expression for the first parameter performs a
// modify access for the duration of a call
func takesUnsafeMutablePointerAndNoEscapeClosure<T>(_ p: UnsafeMutablePointer<T>, _ c: () -> ()) { }


func callsTakesUnsafePointerAndNoEscapeClosure() {
  var local = 1
  takesUnsafePointerAndNoEscapeClosure(&local) { // expected-note {{conflicting access is here}}
     local = 2 // expected-error {{overlapping accesses to 'local', but modification requires exclusive access; consider copying to a local variable}}
  }
}

func callsTakesUnsafePointerAndNoEscapeClosureThatReads() {
  var local = 1

  // Overlapping reads
  takesUnsafePointerAndNoEscapeClosure(&local) {
     _ = local // no-error
  }
}

func callsTakesUnsafeMutablePointerAndNoEscapeClosureThatReads() {
  var local = 1

  // Overlapping modify and read
  takesUnsafeMutablePointerAndNoEscapeClosure(&local) { // expected-error {{overlapping accesses to 'local', but modification requires exclusive access; consider copying to a local variable}}
     _ = local  // expected-note {{conflicting access is here}}
  }

}
func takesThrowingAutoClosureReturningGeneric<T: Equatable>(_ : @autoclosure () throws -> T) { }
func takesInoutAndClosure<T>(_: inout T, _ : () -> ()) { }

func callsTakesThrowingAutoClosureReturningGeneric() {
  var i = 0
  takesInoutAndClosure(&i) { // expected-error {{overlapping accesses to 'i', but modification requires exclusive access; consider copying to a local variable}}
    takesThrowingAutoClosureReturningGeneric(i) // expected-note {{conflicting access is here}}
  }
}

struct StructWithMutatingMethodThatTakesAutoclosure {
  var f = 2
  mutating func takesAutoclosure(_ p: @autoclosure () throws -> ()) rethrows { }
}

func conflictOnSubPathInNoEscapeAutoclosure() {
  var s = StructWithMutatingMethodThatTakesAutoclosure()
  s.takesAutoclosure(s.f = 2)
  // expected-error@-1 {{overlapping accesses to 's', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@-2 {{conflicting access is here}}
}

func conflictOnWholeInNoEscapeAutoclosure() {
  var s = StructWithMutatingMethodThatTakesAutoclosure()
  takesInoutAndNoEscapeClosure(&s.f) {
    // expected-error@-1 {{overlapping accesses to 's.f', but modification requires exclusive access; consider copying to a local variable}}
    s = StructWithMutatingMethodThatTakesAutoclosure()
    // expected-note@-1 {{conflicting access is here}}
  }
}

struct ParameterizedStruct<T> {
  mutating func takesFunctionWithGenericReturnType(_ f: (Int) -> T) {}
}

func testReabstractionThunk(p1: inout ParameterizedStruct<Int>,
                            p2: inout ParameterizedStruct<Int>) {
  // Since takesFunctionWithGenericReturnType() takes a closure with a generic
  // return type it expects the value to be returned @out. But the closure
  // here has an 'Int' return type, so the compiler uses a reabstraction thunk
  // to pass the closure to the method.
  // This tests that we still detect access violations for closures passed
  // using a reabstraction thunk.
  p1.takesFunctionWithGenericReturnType { _ in
    // expected-error@-1 {{overlapping accesses to 'p1', but modification requires exclusive access; consider copying to a local variable}}
    p2 = p1
    // expected-note@-1 {{conflicting access is here}}
    return 3
  }
}


func takesNoEscapeBlockClosure
(
  _ p: inout Int, _ c: @convention(block) () -> ()
) { }

func takesEscapingBlockClosure
(
  _ p: inout Int, _ c: @escaping @convention(block) () -> ()
) { }

func testCallNoEscapeBlockClosure() {
  var i = 7
  takesNoEscapeBlockClosure(&i) {
    // expected-error@-1 {{overlapping accesses to 'i', but modification requires exclusive access; consider copying to a local variable}}
    i = 7
    // expected-note@-1 {{conflicting access is here}}
  }
}

func testCallNoEscapeBlockClosureRead() {
  var i = 7
  takesNoEscapeBlockClosure(&i) {
    // expected-error@-1 {{overlapping accesses to 'i', but modification requires exclusive access; consider copying to a local variable}}
    _ = i
    // expected-note@-1 {{conflicting access is here}}
  }
}

func testCallEscapingBlockClosure() {
  var i = 7
  takesEscapingBlockClosure(&i) { // no-warning
    i = 7
  }
}



func testCallNonEscapingWithEscapedBlock() {
  var i = 7
  let someBlock : @convention(block) () -> () = {
    i = 8
  }

  takesNoEscapeBlockClosure(&i, someBlock) // no-warning
}

func takesInoutAndClosureWithGenericArg<T>(_ p: inout Int, _ c: (T) -> Int) { }

func callsTakesInoutAndClosureWithGenericArg() {
  var i = 7
  takesInoutAndClosureWithGenericArg(&i) { (p: Int) in
    // expected-error@-1 {{overlapping accesses to 'i', but modification requires exclusive access; consider copying to a local variable}}
    return i + p
    // expected-note@-1 {{conflicting access is here}}
  }
}

func takesInoutAndClosureTakingNonOptional(_ p: inout Int, _ c: (Int) -> ()) { }
func callsTakesInoutAndClosureTakingNonOptionalWithClosureTakingOptional() {
  var i = 7
  // Test for the thunk converting an (Int?) -> () to an (Int) -> ()
  takesInoutAndClosureTakingNonOptional(&i) { (p: Int?) in
    // expected-error@-1 {{overlapping accesses to 'i', but modification requires exclusive access; consider copying to a local variable}}
    i = 8
    // expected-note@-1 {{conflicting access is here}}
  }
}

// Helper.
func doOne(_ f: () -> ()) {
  f()
}

func noEscapeBlock() {
  var x = 3
  doOne {
    // expected-error@+2{{overlapping accesses to 'x', but modification requires exclusive access; consider copying to a local variable}}
    // expected-note@+1{{conflicting access is here}}
    takesInoutAndNoEscapeClosure(&x, { _ = x })
  }
}

func inoutSeparateStructStoredProperties() {
  var s = StructWithTwoStoredProp()
  takesTwoInouts(&s.f1, &s.f2) // no-error
}

func inoutSameStoredProperty() {
  var s = StructWithTwoStoredProp()
  takesTwoInouts(&s.f1, &s.f1)
  // expected-error@-1{{overlapping accesses to 's.f1', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@-2{{conflicting access is here}}
}

func inoutSeparateTupleElements() {
  var t = (1, 4)
  takesTwoInouts(&t.0, &t.1) // no-error
}

func inoutSameTupleElement() {
  var t = (1, 4)
  takesTwoInouts(&t.0, &t.0)
  // expected-error@-1{{overlapping accesses to 't.0', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@-2{{conflicting access is here}}
}

func inoutSameTupleNamedElement() {
  var t = (name1: 1, name2: 4)
  takesTwoInouts(&t.name2, &t.name2)
  // expected-error@-1{{overlapping accesses to 't.name2', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@-2{{conflicting access is here}}
}

func inoutSamePropertyInSameTuple() {
  var t = (name1: 1, name2: StructWithTwoStoredProp())
  takesTwoInouts(&t.name2.f1, &t.name2.f1)
  // expected-error@-1{{overlapping accesses to 't.name2.f1', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@-2{{conflicting access is here}}
}

// Noescape closures and separate stored structs

func callsTakesInoutAndNoEscapeClosureNoWarningOnSeparateStored() {
  var local = StructWithTwoStoredProp()
  takesInoutAndNoEscapeClosure(&local.f1) {
    local.f2 = 8 // no-error
  }
}

func callsTakesInoutAndNoEscapeClosureWarningOnSameStoredProp() {
  var local = StructWithTwoStoredProp()
  takesInoutAndNoEscapeClosure(&local.f1) { // expected-error {{overlapping accesses to 'local.f1', but modification requires exclusive access; consider copying to a local variable}}
    local.f1 = 8 // expected-note {{conflicting access is here}}
  }
}

func callsTakesInoutAndNoEscapeClosureWarningOnAggregateAndStoredProp() {
  var local = StructWithTwoStoredProp()
  takesInoutAndNoEscapeClosure(&local) { // expected-error {{overlapping accesses to 'local', but modification requires exclusive access; consider copying to a local variable}}
    local.f1 = 8 // expected-note {{conflicting access is here}}
  }
}

func callsTakesInoutAndNoEscapeClosureWarningOnStoredPropAndAggregate() {
  var local = StructWithTwoStoredProp()
  takesInoutAndNoEscapeClosure(&local.f1) { // expected-error {{overlapping accesses to 'local.f1', but modification requires exclusive access; consider copying to a local variable}}
    local = StructWithTwoStoredProp() // expected-note {{conflicting access is here}}
  }
}

func callsTakesInoutAndNoEscapeClosureWarningOnStoredPropAndBothPropertyAndAggregate() {
  var local = StructWithTwoStoredProp()
  takesInoutAndNoEscapeClosure(&local.f1) { // expected-error {{overlapping accesses to 'local.f1', but modification requires exclusive access; consider copying to a local variable}}
    local.f1 = 8
    // We want the diagnostic on the access for the aggregate and not the projection.
    local = StructWithTwoStoredProp() // expected-note {{conflicting access is here}}
  }
}

func callsTakesInoutAndNoEscapeClosureWarningOnStoredPropAndBothAggregateAndProperty() {
  var local = StructWithTwoStoredProp()
  takesInoutAndNoEscapeClosure(&local.f1) { // expected-error {{overlapping accesses to 'local.f1', but modification requires exclusive access; consider copying to a local variable}}
    // We want the diagnostic on the access for the aggregate and not the projection.
    local = StructWithTwoStoredProp() // expected-note {{conflicting access is here}}
    local.f1 = 8
  }
}


struct MyStruct<T> {
  var prop = 7
  mutating func inoutBoundGenericStruct() {
    takesTwoInouts(&prop, &prop)
    // expected-error@-1{{overlapping accesses to 'self.prop', but modification requires exclusive access; consider copying to a local variable}}
    // expected-note@-2{{conflicting access is here}}
  }
}


func testForLoopCausesReadAccess() {
  var a: [Int] = [1]
  takesInoutAndNoEscapeClosure(&a) { // expected-error {{overlapping accesses to 'a', but modification requires exclusive access; consider copying to a local variable}}
    for _ in a { // expected-note {{conflicting access is here}}
    }
  }
}

func testKeyPathStructField() {
  let getF = \StructWithField.f
  var local = StructWithField()
  takesInoutAndNoEscapeClosure(&local[keyPath: getF]) { // expected-error {{overlapping accesses to 'local', but modification requires exclusive access; consider copying to a local variable}}
    local.f = 17 // expected-note {{conflicting access is here}}
  }
}

func testKeyPathWithClassFinalStoredProperty() {
  let getI = \ClassWithFinalStoredProp.i
  let local = ClassWithFinalStoredProp()

  // Ideally we would diagnose statically here, but it is not required by the
  // model.
  takesTwoInouts(&local[keyPath: getI], &local[keyPath: getI])
}

func takesInoutAndOptionalClosure(_: inout Int, _ f: (()->())?) {
  f!()
}

// An optional closure is not considered @noescape:
// This violation will only be caught dynamically.
//
// apply %takesInoutAndOptionalClosure(%closure)
//   : $@convention(thin) (@inout Int, @owned Optional<@callee_guaranteed () -> ()>) -> ()
func testOptionalClosure() {
  var x = 0
  takesInoutAndOptionalClosure(&x) { x += 1 }
}

func takesInoutAndOptionalBlock(_: inout Int, _ f: (@convention(block) ()->())?) {
  f!()
}

// An optional block is not be considered @noescape.
// This violation will only be caught dynamically.
func testOptionalBlock() {
  var x = 0
  takesInoutAndOptionalBlock(&x) { x += 1 }
}

// Diagnost a conflict on a noescape closure that is conditionally passed as a function argument.
//
// <rdar://problem/42560459> [Exclusivity] Failure to statically diagnose a conflict when passing conditional noescape closures.
struct S {
  var x: Int

  mutating func takeNoescapeClosure(_ f: ()->()) { f() }

  mutating func testNoescapePartialApplyPhiUse(z : Bool) {
    func f1() {
      x = 1 // expected-note {{conflicting access is here}}
    }
    func f2() {
      x = 1 // expected-note {{conflicting access is here}}
    }
    takeNoescapeClosure(z ? f1 : f2)
    // expected-error@-1 2 {{overlapping accesses to 'self', but modification requires exclusive access; consider copying to a local variable}}
  }
}

func doit(x: inout Int, _ fn: () -> ()) {}

func nestedConflict(x: inout Int) {
  doit(x: &x, x == 0 ? { x = 1 } : { x = 2})
  // expected-error@-1 2{{overlapping accesses to 'x', but modification requires exclusive access; consider copying to a local variable}}
  // expected-note@-2 2{{conflicting access is here}}
}

// Avoid diagnosing a conflict on disjoint struct properties when one is a `let`.
// This requires an address projection before loading the `let` property.
//
// rdar://problem/35561050
// https://github.com/apple/swift/issues/52547
// [Exclusivity] SILGen loads entire struct when reading captured 'let'
// stored property

struct DisjointLetMember {
  var dummy: AnyObject // Make this a nontrivial struct because the SIL is more involved.
  mutating func get(makeValue: ()->Int) -> Int {
    return makeValue()
  }
}

class IntWrapper {
  var x = 0
}

struct DisjointLet {
  let a = 2 // Using a `let` forces a full load.
  let b: IntWrapper
  var cache: DisjointLetMember

  init(b: IntWrapper) {
    self.b = b
    self.cache = DisjointLetMember(dummy: b)
  }

  mutating func testDisjointLet() -> Int {
    // Access to inout `self` for member .cache`.
    return cache.get {
      // Access to captured `self` for member .cache`.
      a + b.x
    }
  }
}

// -----------------------------------------------------------------------------
// coroutineWithClosureArg: AccessedSummaryAnalysis must consider
// begin_apply a valid user of partial_apply.
//
// Test that this does not assert in hasExpectedUsesOfNoEscapePartialApply.
//
// This test needs two closures, one to capture the variable, another
// to recapture the variable, so AccessSummary is forced to process
// the closure.
func coroutineWithClosureArg(i: Int, x: inout Int, d: inout Dictionary<Int, Int>) {
  { d[i, default: x] = 0 }()
}

// -----------------------------------------------------------------------------
//
struct TestConflictInCoroutineClosureArg {
  static let defaultKey = 0

  var dictionary = [defaultKey:0]

  mutating func incrementValue(at key: Int) {
    dictionary[key, default:
      dictionary[TestConflictInCoroutineClosureArg.defaultKey]!] += 1
    // expected-error@-2 {{overlapping accesses to 'self.dictionary', but modification requires exclusive access; consider copying to a local variable}}
    // expected-note@-2 {{conflicting access is here}}
  }
}

// Check that AccessSummaryAnalysis does not crash with this:
struct TestStruct {
  var x = 7
  mutating func b()  {
    func c() {}
    func d() {
        x // expected-warning {{property is accessed but result is unused}}
    }
     withoutActuallyEscaping(c) { e in
        withoutActuallyEscaping(d) { e in
        }
    }
  }
}
