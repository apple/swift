// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -typecheck -parse-as-library -verify-additional-file %clang-importer-sdk-path/usr/include/objc_structs.h -verify %s

// RUN: %target-swift-ide-test(mock-sdk: %clang-importer-sdk) -print-module -module-to-print=objc_structs -I %S/Inputs -source-filename=x -enable-objc-interop | %FileCheck -check-prefix=CHECK-IDE-TEST %s

// REQUIRES: objc_interop

import Foundation
import objc_structs

extension String {
  func onlyOnString() -> String { return self }
}

extension Bool {
  func onlyOnBool() -> Bool { return self }
}

extension Array {
  func onlyOnArray() { }
}

extension Dictionary {
  func onlyOnDictionary() { }
}

extension Set {
  func onlyOnSet() { }
}

func foo() {
  _  = NSStringToNSString as (String?) -> String?
  _ = DummyClass().nsstringProperty.onlyOnString() as String

  _  = BOOLtoBOOL as (Bool) -> Bool
  _  = DummyClass().boolProperty.onlyOnBool() as Bool

  _  = arrayToArray as (Array<Any>?) -> (Array<Any>?)
  DummyClass().arrayProperty.onlyOnArray()

  _ = dictToDict as (Dictionary<AnyHashable, Any>?) -> Dictionary<AnyHashable, Any>?

  DummyClass().dictProperty.onlyOnDictionary()

  _ = setToSet as (Set<AnyHashable>?) -> Set<AnyHashable>?
  DummyClass().setProperty.onlyOnSet()
}

func allocateMagic(_ zone: NSZone) -> UnsafeMutableRawPointer {
  return allocate(zone)
}

func constPointerToObjC(_ objects: [AnyObject]) -> NSArray {
  return NSArray(objects: objects, count: objects.count)
}

func mutablePointerToObjC(_ path: String) throws -> NSString {
  return try NSString(contentsOfFile: path)
}

func objcStructs(_ s: StructOfNSStrings, sb: StructOfBlocks) {
  // Struct fields must not be bridged.
  _ = s.nsstr! as Bool // expected-error {{cannot convert value of type 'NSString' to type 'Bool' in coercion}}
  
  // FIXME: Blocks should also be Unmanaged.
  _ = sb.block as Bool // expected-error {{cannot convert value of type '() -> Void' to type 'Bool' in coercion}}
  sb.block() // okay
}

// CHECK-IDE-TEST: struct StrongsInAStruct {
// CHECK-IDE-TEST:   init(myobj: MYObject)
// CHECK-IDE-TEST:   var myobj: MYObject
// CHECK-IDE-TEST: }

// CHECK-IDE-TEST: struct WeaksInAStruct {
// CHECK-IDE-TEST:   init()
// CHECK-IDE-TEST:   init(myobj: MYObject?)
// CHECK-IDE-TEST:   weak var myobj: @sil_weak MYObject?
// CHECK-IDE-TEST: }

// CHECK-IDE-TEST: struct WeakAndNonnull {
// CHECK-IDE-TEST: }

// Structs with non-trivial copy/destroy should be imported
func objcStructsWithArcPointers(withWeaks weaks: WeaksInAStruct, strongs: StrongsInAStruct) -> StrongsInAStruct {
  
  // Weak references should be bridged as Optional
  let anObject: MYObject = weaks.myobj ?? MYObject()
  
  // Should be able to construct these as well
  _ = WeaksInAStruct(myobj: anObject)
    
  // Strong references should be retained by the struct's ctor
  let aStrongInAStruct = StrongsInAStruct(myobj: anObject)

  return aStrongInAStruct
}

func objcStructWithWeakNonnullDoesntMakeSense() {
  let sct = WeakAndNonnull(myobj: MYObject()) // expected-error {{'WeakAndNonnull' cannot be constructed because it has no accessible initializers}}
}

func test_repair_does_not_interfere_with_conversions() {
  func foo(_: Any, _: AnyHashable) {}
  func bar(_ a: AnyObject, _ b: AnyObject) {
    foo(a, b as! NSObject) // Ok
  }
}
