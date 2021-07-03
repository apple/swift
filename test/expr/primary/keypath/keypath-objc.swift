// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -typecheck -parse-as-library  %s -verify -enable-access-control-hacks
import ObjectiveC
import Foundation

// REQUIRES: objc_interop

@objc class A : NSObject {
  @objc var propB: B = B()
  @objc var propString: String = "" // expected-note {{did you mean 'propString'}}
  @objc var propArray: [String] = []
  @objc var propDict: [String: B] = [:]
  @objc var propSet: Set<String> = []
  @objc var propNSString: NSString?
  @objc var propNSArray: NSArray?
  @objc var propNSDict: NSDictionary?
  @objc var propNSSet: NSSet?
  @objc var propAnyObject: AnyObject?

  @objc var ambiguous: String? // expected-note{{'ambiguous' declared here}}

  @objc func someMethod() { }

  @objc var `repeat`: String?
}

@objc class B : NSObject  {
  @objc var propA: A?

  @objc var ambiguous: String? // expected-note{{'ambiguous' declared here}}
}

class C {
  var nonObjC: String? // expected-note{{add '@objc' to expose this property to Objective-C}}{{3-3=@objc }}
}

extension NSArray {
  @objc class Foo : NSObject {
    @objc var propString: String = ""
  }
}

extension Array {
  typealias Foo = NSArray.Foo
}

func testKeyPath(a: A, b: B) {
  // Property
  let _: String = #keyPath(A.propB)

  // Chained property
  let _: String = #keyPath(A.propB.propA)

  // Optional property
  let _: String = #keyPath(A.propB.propA.propB)

  // String property
  let _: String = #keyPath(A.propString)

  // Property of String property (which looks on NSString)
  let _: String = #keyPath(A.propString.URLsInText) // expected-warning{{'URLsInText' has been renamed to 'urlsInText'}}

  // String property with a suffix
  let _: String = #keyPath(A.propString).description
  let _ = #keyPath(A.propString).split(separator: ".")
  func keyPathSwitch(keyPath: String?) {
    switch keyPath {
    case (#keyPath(A.propString))?: break
    case #keyPath(A.propString)?: break
    default: break
    } 
  }

  // Array property (make sure we look at the array element).
  let _: String = #keyPath(A.propArray)
  let _: String = #keyPath(A.propArray.URLsInText) // expected-warning{{'URLsInText' has been renamed to 'urlsInText'}}

  // Dictionary property (make sure we look at the value type).
  let _: String = #keyPath(A.propDict.anyKeyName)
  let _: String = #keyPath(A.propDict.anyKeyName.propA)

  // Set property (make sure we look at the set element).
  let _: String = #keyPath(A.propSet)
  let _: String = #keyPath(A.propSet.URLsInText) // expected-warning{{'URLsInText' has been renamed to 'urlsInText'}}

  // AnyObject property
  let _: String = #keyPath(A.propAnyObject.URLsInText) // expected-warning{{'URLsInText' has been renamed to 'urlsInText'}}
  let _: String = #keyPath(A.propAnyObject.propA)  
  let _: String = #keyPath(A.propAnyObject.propB)  
  let _: String = #keyPath(A.propAnyObject.description)  

  // NSString property
  let _: String = #keyPath(A.propNSString.URLsInText) // expected-warning{{'URLsInText' has been renamed to 'urlsInText'}}

  // NSArray property (AnyObject array element).
  let _: String = #keyPath(A.propNSArray)
  let _: String = #keyPath(A.propNSArray.URLsInText) // expected-warning{{'URLsInText' has been renamed to 'urlsInText'}}

  // NSDictionary property (AnyObject value type).
  let _: String = #keyPath(A.propNSDict.anyKeyName)
  let _: String = #keyPath(A.propNSDict.anyKeyName.propA)

  // NSSet property (AnyObject set element).
  let _: String = #keyPath(A.propNSSet)
  let _: String = #keyPath(A.propNSSet.URLsInText) // expected-warning{{'URLsInText' has been renamed to 'urlsInText'}}

  // Property with keyword name.
  let _: String = #keyPath(A.repeat)

  // Nested type of a bridged type (rdar://problem/28061409).
  typealias IntArray = [Int]
  let _: String = #keyPath(IntArray.Foo.propString)

  let dict: [String: Int] = [:]
  let _: Int? = dict[#keyPath(A.propB)]
  let _ = [#keyPath(A.propB)]
}

func testAsStaticString() {
  let _: StaticString = #keyPath(A.propB)
}

func testSemanticErrors() {
  let _: String = #keyPath(A.blarg) // expected-error{{type 'A' has no member 'blarg'}}
  let _: String = #keyPath(blarg) // expected-error{{cannot find 'blarg' in scope}}
  let _: String = #keyPath(AnyObject.ambiguous) // expected-error{{ambiguous reference to member 'ambiguous'}}
  let _: String = #keyPath(C.nonObjC) // expected-error{{argument of '#keyPath' refers to non-'@objc' property 'nonObjC'}}
  let _: String = #keyPath(A.propArray.UTF8View) // expected-error{{type 'String' has no member 'UTF8View'}}
  let _: String = #keyPath(A.someMethod) // expected-error{{key path cannot refer to instance method 'someMethod()'}}
  let _: String = #keyPath(A) // expected-error{{empty key path does not refer to a property}}
  let _: String = #keyPath(A.propDict.anyKeyName.unknown) // expected-error{{type 'B' has no member 'unknown'}}
  let _: String = #keyPath(A.propNSDict.anyKeyName.unknown) // expected-error{{type 'AnyObject' has no member 'unknown'}}
}

func testParseErrors() {
  let _: String = #keyPath; // expected-error{{expected '(' following '#keyPath'}}
  let _: String = #keyPath(123; // expected-error{{expected property or type name within '#keyPath(...)'}}
  let _: String = #keyPath(a.123; // expected-error{{expected property or type name within '#keyPath(...)'}}
  let _: String = #keyPath(A(b:c:d:).propSet); // expected-error{{an Objective-C key path cannot reference a declaration with a compound name}} expected-error{{cannot find 'propSet' in scope}}
  let _: String = #keyPath(A.propString; // expected-error{{expected ')' to complete '#keyPath' expression}}
    // expected-note@-1{{to match this opening '('}}
}

func testTypoCorrection() {
  let _: String = #keyPath(A.proString) // expected-error {{type 'A' has no member 'proString'}}
}

class SR_10146_1 {
  @objc let b = 1
}

class SR_10146_2: SR_10146_1 {
  let a = \AnyObject.b // expected-error {{the root type of a Swift key path cannot be 'AnyObject'}}
}

class SR_10146_3 {
  @objc let abc: Int = 1
  
  func doNotCrash() {
    let _: KeyPath<AnyObject, Int> = \.abc // expected-error {{the root type of a Swift key path cannot be 'AnyObject'}}
  }

  func doNotCrash_1(_ obj: AnyObject, _ kp: KeyPath<AnyObject, Int>) {
    let _ = obj[keyPath: \.abc] // expected-error {{the root type of a Swift key path cannot be 'AnyObject'}}
    let _ = obj[keyPath: kp] // expected-error {{the root type of a Swift key path cannot be 'AnyObject'}}
  }
}
