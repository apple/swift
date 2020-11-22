// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// REQUIRES: optimized_stdlib

import StdlibUnittest

struct StructWithMembers {
  var a = 1
  var b = "Hello World"
}

class ClassWithMembers {
  var a = 1
  var b = "Hello World"
}

class ClassWithMirror: CustomReflectable {
  var a = 1
  var customMirror: Mirror {
    return Mirror(self, children: ["a" : 1, "b" : "Hello World"])
  }
}

class SubClassWithMirror: ClassWithMirror {
  override var customMirror: Mirror {
    return Mirror(self, children: ["z" : 99])
  }
}

#if _runtime(_ObjC)
struct DontBridgeThisStruct {
  var message = "Hello World"
}

extension DontBridgeThisStruct : _ObjectiveCBridgeable {
  typealias _ObjectiveCType = AnyObject

  func _bridgeToObjectiveC() -> _ObjectiveCType {
    fatalError("tried to bridge DontBridgeThisStruct")
  }

  static func _forceBridgeFromObjectiveC(
    _ source: _ObjectiveCType,
    result: inout DontBridgeThisStruct?
  ) {
    result = nil
  }

  static func _conditionallyBridgeFromObjectiveC(
    _ source: _ObjectiveCType,
    result: inout DontBridgeThisStruct?
  ) -> Bool {
    result = nil
    return false
  }

  static func _unconditionallyBridgeFromObjectiveC(_ source: _ObjectiveCType?)
      -> DontBridgeThisStruct {
    return DontBridgeThisStruct()
  }
}
#endif

let StringForPrintObjectTests = TestSuite("StringForPrintObject")
StringForPrintObjectTests.test("StructWithMembers") {
  let printed = _stringForPrintObject(StructWithMembers())
  expectEqual(printed, "▿ StructWithMembers\n  - a : 1\n  - b : \"Hello World\"\n")
}

#if _runtime(_ObjC)
StringForPrintObjectTests.test("ClassWithMembers") {
  let printed = _stringForPrintObject(ClassWithMembers())
  expectTrue(printed.hasPrefix("<ClassWithMembers: 0x"))
}
#endif

StringForPrintObjectTests.test("ClassWithMirror") {
  let printed = _stringForPrintObject(ClassWithMirror())
  expectEqual(printed, "▿ ClassWithMirror\n  - a : 1\n  - b : \"Hello World\"\n")
}

StringForPrintObjectTests.test("SubClassWithMirror") {
  let printed = _stringForPrintObject(SubClassWithMirror())
  let expected =
    "▿ SubClassWithMirror\n" +
    "  ▿ super : ClassWithMirror\n" +
    "    - a : 1\n" +
    "  - z : 99\n";
  expectEqual(expected, printed);
}

StringForPrintObjectTests.test("Array") {
  let printed = _stringForPrintObject([1,2,3,4])
  expectEqual(printed, "▿ 4 elements\n  - 0 : 1\n  - 1 : 2\n  - 2 : 3\n  - 3 : 4\n")
}

StringForPrintObjectTests.test("Dictionary") {
  let printed = _stringForPrintObject([1:2])
  expectEqual("▿ 1 element\n  ▿ 0 : 2 elements\n    - key : 1\n    - value : 2\n",
              printed)
}

StringForPrintObjectTests.test("NilOptional") {
  let printed = _stringForPrintObject(nil as Int?)
  expectEqual(printed, "nil\n")
}

StringForPrintObjectTests.test("SomeOptional") {
  let printed = _stringForPrintObject(3 as Int?)
  expectEqual(printed, "▿ Optional<Int>\n  - some : 3\n")
}

#if _runtime(_ObjC)
StringForPrintObjectTests.test("DontBridgeThisStruct") {
  let printed = _stringForPrintObject(DontBridgeThisStruct())
  expectEqual(printed, "▿ DontBridgeThisStruct\n  - message : \"Hello World\"\n")
}
#endif

class RefCountedObj {
  var patatino : Int
  init(_ p : Int) {
    self.patatino = p
  }
  public func f() -> Int {
    return self.patatino
  }
}

let RefcountTests = TestSuite("RefCount")
RefcountTests.test("Basic") {
  var Obj = RefCountedObj(47);

  // Counters for live objects should be always positive.
  // We try to be a little bit more lax here because optimizations
  // or different stdlib versions might impact the exact value of
  // refcounting, and we're just interested in testing whether the
  // stub works properly.
  expectGT(_getRetainCount(Obj), 0);
  expectGT(_getWeakRetainCount(Obj), 0);
  expectGT(_getUnownedRetainCount(Obj), 0);

  // Try to keep the object live here.
  let _ = Obj.f()
}

runAllTests()
