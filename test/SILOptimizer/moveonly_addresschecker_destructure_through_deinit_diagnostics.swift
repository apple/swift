// RUN: %target-swift-emit-sil -sil-verify-all -verify -enable-experimental-feature NoImplicitCopy -enable-experimental-feature MoveOnlyClasses -enable-experimental-feature MoveOnlyTuples %s

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

@_moveOnly
struct KlassPair {
  var lhs: Klass
  var rhs: MoveOnlyKlass
}

@_moveOnly
struct AggStruct {
  var pair: KlassPair
}

@_moveOnly
struct KlassPair2 {
  var lhs: MoveOnlyKlass
  var rhs: MoveOnlyKlass
}

@_moveOnly
struct AggStruct2 {
  var lhs: MoveOnlyKlass
  var pair: KlassPair2
  var rhs: MoveOnlyKlass
}

@_moveOnly
struct SingleIntContainingStruct {
    var value: Int = 0
}

func consume(_ x: consuming MoveOnlyKlass) {}
func consume(_ x: consuming Klass) {}

////////////////////
// Test Top Level //
////////////////////

@_moveOnly
struct DeinitStruct {
  var first: Klass
  var second: (Klass, Klass)
  var third: KlassPair
  var fourth: (MoveOnlyKlass, MoveOnlyKlass)
  var fifth: MoveOnlyKlass

  deinit {}
  // expected-note @-1 {{deinit declared here}}
  // expected-note @-2 {{deinit declared here}}
  // expected-note @-3 {{deinit declared here}}
  // expected-note @-4 {{deinit declared here}}
  // expected-note @-5 {{deinit declared here}}
  // expected-note @-6 {{deinit declared here}}
  // expected-note @-7 {{deinit declared here}}
  // expected-note @-8 {{deinit declared here}}
  // expected-note @-9 {{deinit declared here}}
  // expected-note @-10 {{deinit declared here}}
}

func testConsumeCopyable(_ x: consuming DeinitStruct) {
    consume(x.first)
    consume(x.second.0)
    consume(x.third.lhs)
}

func testConsumeNonCopyable1(_ x: consuming DeinitStruct) {
    // expected-error @-1 {{Cannot partially consume 'x' since it has a user defined deinit}}
    consume(x.third.rhs) // expected-note {{consuming use here}}
}

func testConsumeNonCopyable2(_ x: consuming DeinitStruct) {
    // expected-error @-1 {{Cannot partially consume 'x' since it has a user defined deinit}}
    consume(x.fourth.0) // expected-note {{consuming use here}}
}

func testConsumeNonCopyable3(_ x: consuming DeinitStruct) {
    // expected-error @-1 {{Cannot partially consume 'x' since it has a user defined deinit}}
    consume(x.fourth.1) // expected-note {{consuming use here}}
}


func testConsumeNonCopyable4(_ x: consuming DeinitStruct) {
    // expected-error @-1 {{Cannot partially consume 'x' since it has a user defined deinit}}
    consume(x.fifth) // expected-note {{consuming use here}}
}

/////////////////
// Test Fields //
/////////////////

@_moveOnly
struct StructContainDeinitStruct {
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


func testStructContainStructContainDeinitStructConsumeNonCopyable1(_ x: consuming StructContainDeinitStruct) {
    // expected-error @-1 {{Cannot partially consume 'x' since it contains field 'x.first' whose type 'DeinitStruct' has a user defined deinit}}
    consume(x.first.third.rhs) // expected-note {{consuming use here}}
}

func testStructContainStructContainDeinitStructConsumeNonCopyable1a(_ x: consuming StructContainDeinitStruct) {
    // expected-error @-1 {{Cannot partially consume 'x' since it contains field 'x.second.0' whose type 'DeinitStruct' has a user defined deinit}}
    consume(x.second.0.third.rhs) // expected-note {{consuming use here}}
}

func testStructContainStructContainDeinitStructConsumeNonCopyable2(_ x: consuming StructContainDeinitStruct) {
    // expected-error @-1 {{Cannot partially consume 'x' since it contains field 'x.first' whose type 'DeinitStruct' has a user defined deinit}}
    consume(x.first.fourth.0) // expected-note {{consuming use here}}
}

func testStructContainStructContainDeinitStructConsumeNonCopyable2a(_ x: consuming StructContainDeinitStruct) {
    // expected-error @-1 {{Cannot partially consume 'x' since it contains field 'x.second.1' whose type 'DeinitStruct' has a user defined deinit}}
    consume(x.second.1.fourth.0) // expected-note {{consuming use here}}
}

func testStructContainStructContainDeinitStructConsumeNonCopyable3(_ x: consuming StructContainDeinitStruct) {
    // expected-error @-1 {{Cannot partially consume 'x' since it contains field 'x.first' whose type 'DeinitStruct' has a user defined deinit}}
    consume(x.first.fourth.1) // expected-note {{consuming use here}}
}


func testStructContainStructContainDeinitStructConsumeNonCopyable4(_ x: consuming StructContainDeinitStruct) {
    // expected-error @-1 {{Cannot partially consume 'x' since it contains field 'x.first' whose type 'DeinitStruct' has a user defined deinit}}
    consume(x.first.fifth) // expected-note {{consuming use here}}
}
