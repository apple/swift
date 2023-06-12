// RUN: %target-swift-emit-sil -sil-verify-all -verify -enable-experimental-feature MoveOnlyClasses -enable-experimental-feature MoveOnlyTuples %s

// This test validates that we properly emit errors if we partially invalidate
// through a type with a deinit.

//////////////////
// Declarations //
//////////////////

class Klass {}

@_moveOnly
class MoveOnlyKlass {
  var value: Int = 0
}

struct KlassPair : ~Copyable {
  var lhs: Klass
  var rhs: MoveOnlyKlass
}

struct AggStruct : ~Copyable {
  var pair: KlassPair
}

struct KlassPair2 : ~Copyable {
  var lhs: MoveOnlyKlass
  var rhs: MoveOnlyKlass
}

struct AggStruct2 : ~Copyable {
  var lhs: MoveOnlyKlass
  var pair: KlassPair2
  var rhs: MoveOnlyKlass
}

struct SingleIntContainingStruct : ~Copyable {
    var value: Int = 0
}

struct MoveOnlyPair : ~Copyable {
  var first = SingleIntContainingStruct()
  var second = SingleIntContainingStruct()
}

protocol P {
  static var value: Self { get }
}

func consume<T : P>(_ x: consuming T) {}
func consume(_ x: consuming SingleIntContainingStruct) {}
func consume(_ x: consuming MoveOnlyKlass) {}
func consume(_ x: consuming MoveOnlyPair) {}
func consume(_ x: consuming Klass) {}

////////////////////
// MARK: Loadable //
////////////////////

struct DeinitStruct : ~Copyable {
  var first: Klass
  var second: (Klass, Klass)
  var third: KlassPair
  var fourth: (MoveOnlyKlass, MoveOnlyKlass)
  var fifth: MoveOnlyKlass

  deinit {} // expected-note 10{{deinitializer declared here}}
}

func testConsumeCopyable(_ x: consuming DeinitStruct) {
    consume(x.first)
    consume(x.second.0)
    consume(x.third.lhs)
}

func testConsumeNonCopyable1(_ x: consuming DeinitStruct) {
    // expected-error @+1 {{cannot partially consume 'x' when it has a deinitializer}}
    consume(x.third.rhs)
}

func testConsumeNonCopyable2(_ x: consuming DeinitStruct) {
    // expected-error @+1 {{cannot partially consume 'x' when it has a deinitializer}}
    consume(x.fourth.0)
}

func testConsumeNonCopyable3(_ x: consuming DeinitStruct) {
    // expected-error @+1 {{cannot partially consume 'x' when it has a deinitializer}}
    consume(x.fourth.1)
}


func testConsumeNonCopyable4(_ x: consuming DeinitStruct) {
    // expected-error @+1 {{cannot partially consume 'x' when it has a deinitializer}}
    consume(x.fifth)
}


///////////////////////////
// MARK: Loadable Fields //
///////////////////////////

struct StructContainDeinitStruct : ~Copyable {
  var first: DeinitStruct
  var second: (DeinitStruct, DeinitStruct)
  var third: Klass
  var fourth: (Klass, Klass)
  var fifth: MoveOnlyKlass
  var sixth: (MoveOnlyKlass, MoveOnlyKlass)
}

func testStructContainDeinitStructConsumeCopyable1(_ x: consuming StructContainDeinitStruct) {
    consume(x.first.first)
    consume(x.first.second.0)
    consume(x.first.third.lhs)
    consume(x.second.0.first)
    consume(x.second.1.second.0)
    consume(x.second.0.third.lhs)
    consume(x.sixth.0)
}


func testStructContainStructContainDeinitStructConsumeNonCopyable1(_ xyz: consuming StructContainDeinitStruct) {
    // expected-error @+1 {{cannot partially consume 'xyz.first' when it has a deinitializer}}
    consume(xyz.first.third.rhs)
}

func testStructContainStructContainDeinitStructConsumeNonCopyable1a(_ x: consuming StructContainDeinitStruct) {
    // expected-error @+1 {{cannot partially consume 'x.second.0' when it has a deinitializer}}
    consume(x.second.0.third.rhs)
}

func testStructContainStructContainDeinitStructConsumeNonCopyable2(_ x: consuming StructContainDeinitStruct) {
    // expected-error @+1 {{cannot partially consume 'x.first' when it has a deinitializer}}
    consume(x.first.fourth.0)
}

func testStructContainStructContainDeinitStructConsumeNonCopyable2a(_ x: consuming StructContainDeinitStruct) {
    // expected-error @+1 {{cannot partially consume 'x.second.1' when it has a deinitializer}}
    consume(x.second.1.fourth.0)
}

func testStructContainStructContainDeinitStructConsumeNonCopyable3(_ x: consuming StructContainDeinitStruct) {
    // expected-error @+1 {{cannot partially consume 'x.first' when it has a deinitializer}}
    consume(x.first.fourth.1)
}


func testStructContainStructContainDeinitStructConsumeNonCopyable4(_ x: consuming StructContainDeinitStruct) {
    // expected-error @+1 {{cannot partially consume 'x.first' when it has a deinitializer}}
    consume(x.first.fifth)
}

////////////////////////
// MARK: Address Only //
////////////////////////

struct AddressOnlyDeinitStruct<T : P> : ~Copyable {
  var copyable: T = T.value
  var moveOnly = SingleIntContainingStruct()
  var moveOnlyPair = MoveOnlyPair()

  deinit {} // expected-note 5{{deinitializer declared here}}
}

func consume<T : P>(_ x: consuming AddressOnlyDeinitStruct<T>) {}

func testAddressOnlyCanConsumeEntireType<T : P>(_ x: consuming AddressOnlyDeinitStruct<T>) {
  // This is ok since we are consuming a copyable value.
  consume(x.copyable)
  // This is ok since we are consuming the entire value.
  consume(x)
}

func testAddressOnlyCannotPartialConsume<T : P>(_ x: consuming AddressOnlyDeinitStruct<T>) {
  consume(x.moveOnly) // expected-error {{cannot partially consume 'x' when it has a deinitializer}}
  consume(x.moveOnlyPair.first) // expected-error {{cannot partially consume 'x' when it has a deinitializer}}
  consume(x.copyable)
}

struct AddressOnlyContainingDeinitStruct<T : P> : ~Copyable {
  var a = AddressOnlyDeinitStruct<T>()
}

func testAddressOnlyCannotPartialConsumeEvenIfSingleElt<T : P>(_ x: consuming AddressOnlyContainingDeinitStruct<T>) {
  // We do not error here since we can partially consume x, but not x.a
  consume(x.a)
  consume(x.a.moveOnlyPair) // expected-error {{cannot partially consume 'x.a' when it has a deinitializer}}
}

struct AddressOnlyContainingDeinitSingleField<T : P> : ~Copyable {
  var moveOnly = SingleIntContainingStruct()
  deinit {} // expected-note {{deinitializer declared here}}
}

struct AddressOnlyContainingDeinitStruct3<T : P> : ~Copyable {
  var a = AddressOnlyContainingDeinitSingleField<T>()
}

func consume<T : P>(_ x: consuming AddressOnlyContainingDeinitSingleField<T>) {}

func testAddressOnlyCannotPartialConsumeEvenIfSingleElt<T : P>(_ x: consuming AddressOnlyContainingDeinitStruct3<T>) {
  // We do not error here since we can partially consume x, but not x.a
  consume(x.a)
  consume(x.a.moveOnly) // expected-error {{cannot partially consume 'x.a' when it has a deinitializer}}
}


struct AddressOnlyContainingDeinitStructPair<T : P> : ~Copyable {
  var first = AddressOnlyDeinitStruct<T>()
  var second = AddressOnlyDeinitStruct<T>()
}

// Make sure that if the deinit is in an intermediate element of the path that
// we still handle it appropriately.
func testAddressOnlyDeinitInMiddlePath<T : P>(_ x: consuming AddressOnlyContainingDeinitStructPair<T>) {
  consume(x.first.moveOnly) // expected-error {{cannot partially consume 'x.first' when it has a deinitializer}}
  consume(x.first.moveOnlyPair.first) // expected-error {{cannot partially consume 'x.first' when it has a deinitializer}}
  consume(x.first.copyable)
}
