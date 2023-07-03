// This file is also used by objc_properties_ios.swift.

// RUN: %swift -target %target-cpu-apple-macosx10.11 %s -disable-target-os-checking -emit-ir -disable-objc-attr-requires-foundation-module | %FileCheck -check-prefix=CHECK -check-prefix=CHECK-NEW %s
// RUN: %swift -target %target-cpu-apple-macosx10.10 %s -disable-target-os-checking -emit-ir -disable-objc-attr-requires-foundation-module | %FileCheck -check-prefix=CHECK -check-prefix=CHECK-OLD %s

// REQUIRES: OS=macosx
// REQUIRES: objc_interop

@objc class SomeObject {
  @objc var readonly : SomeObject {
    get {
      return self
    }
  }

  @objc var readwrite : SomeObject {
    get {
      return bareIvar
    }
    set {
      bareIvar = newValue
    }
  }

  @objc var bareIvar : SomeObject

  @objc(wobble) var wibble : SomeObject

  @objc init() { 
    bareIvar = SomeObject()
    wibble  = SomeObject()
  }

  @objc static var sharedProp: Int64 = 0
}

extension SomeObject {
  @objc var extensionProperty : SomeObject {
    get {
      return self
    }
    set {
      bareIvar = self
    }
  }

  @objc class var extensionClassProp : SomeObject.Type {
    return self
  }

  @objc static var extensionStoredStaticProp: Int64 = 0
}

// <rdar://problem/16952186> Crash with @lazy in @objc class
@objc
class LazyPropertyCrash  {
  @objc lazy var applicationFilesDirectory: LazyPropertyCrash = LazyPropertyCrash()
}

// <rdar://16909436>
@objc class Tree {
  @objc weak var parent: Tree?
}


// <rdar://problem/17127126> swift compiler segfaults trying to generate a setter for a @lazy property in a subclass of NSObject
func test17127126(f : Class17127126) {
  f.x = 2   // this is the problem
}

@objc
class Class17127126 {
  @objc lazy var x = 1
}

@objc protocol Proto {
  var value: Int { get }
  static var sharedInstance: AnyObject { get set }
}

@propertyWrapper
public struct SomeWrapper {
  private var value: Int


  public init(wrappedValue: Int) {
    value = wrappedValue
  }


  public var wrappedValue: Int {
    get { value }
    set { value = newValue }
  }
}

class SomeWrapperTests {
  @objc @SomeWrapper dynamic var someWrapper: Int = 0

  func testAssignment() {
    // This used to crash irgen.
    someWrapper = 1000
  }
}
// CHECK-NEW: [[SHARED_NAME:@.*]] = private unnamed_addr constant [11 x i8] c"sharedProp\00"
// CHECK-NEW: [[SHARED_ATTRS:@.*]] = private unnamed_addr constant [5 x i8] c"Tq,N\00"

// CHECK-NEW: @_CLASS_PROPERTIES__TtC15objc_properties10SomeObject = internal constant { {{.*}}] } {
// CHECK-NEW:   i32 16,
// CHECK-NEW:   i32 1,
// CHECK-NEW:   [1 x { ptr, ptr }] [{
// CHECK-NEW:     ptr [[SHARED_NAME]],
// CHECK-NEW:     ptr [[SHARED_ATTRS]]
// CHECK-NEW:   }]
// CHECK-NEW: }, section "__DATA, {{.*}}", align 8

// CHECK: @_METACLASS_DATA__TtC15objc_properties10SomeObject = internal constant { {{.*}} } {
// CHECK-SAME:   i32 {{[0-9]+}}, i32 {{[0-9]+}}, i32 {{[0-9]+}}, i32 {{[0-9]+}},
// CHECK-SAME:   ptr null,
// CHECK-SAME:   ptr {{@[^,]+}},
// CHECK-SAME:   ptr @_CLASS_METHODS__TtC15objc_properties10SomeObject{{(\.ptrauth)?}}
// CHECK-SAME:   ptr null, ptr null, ptr null,
// CHECK-NEW-SAME:   ptr @_CLASS_PROPERTIES__TtC15objc_properties10SomeObject
// CHECK-OLD-SAME:   ptr null
// CHECK-SAME: }, section "__DATA, {{.*}}", align 8

// CHECK: [[GETTER_SIGNATURE:@.*]] = private unnamed_addr constant [8 x i8] c"@16@0:8\00"
// CHECK: [[SETTER_SIGNATURE:@.*]] = private unnamed_addr constant [11 x i8] c"v24@0:8@16\00"

// CHECK: @_INSTANCE_METHODS__TtC15objc_properties10SomeObject = internal constant { {{.*}}] } {
// CHECK:   i32 24,
// CHECK:   i32 8,
// CHECK:   [8 x { ptr, ptr, ptr }] [{
// CHECK:     ptr @"\01L_selector_data(readonly)",
// CHECK:     ptr [[GETTER_SIGNATURE]],
// CHECK:     @"$s15objc_properties10SomeObjectC8readonlyACvgTo{{(.ptrauth)?}}"
// CHECK:   }, {
// CHECK:     ptr @"\01L_selector_data(readwrite)",
// CHECK:     ptr [[GETTER_SIGNATURE]],
// CHECK:     @"$s15objc_properties10SomeObjectC9readwriteACvgTo{{(.ptrauth)?}}"
// CHECK:   }, {
// CHECK:     ptr @"\01L_selector_data(setReadwrite:)",
// CHECK:     ptr [[SETTER_SIGNATURE]],
// CHECK:     @"$s15objc_properties10SomeObjectC9readwriteACvsTo{{(.ptrauth)?}}"
// CHECK:   }, {
// CHECK:     ptr @"\01L_selector_data(bareIvar)",
// CHECK:     ptr [[GETTER_SIGNATURE]],
// CHECK:     @"$s15objc_properties10SomeObjectC8bareIvarACvgTo{{(.ptrauth)?}}"
// CHECK:   }, {
// CHECK:     ptr @"\01L_selector_data(setBareIvar:)",
// CHECK:     ptr [[SETTER_SIGNATURE]],
// CHECK:     @"$s15objc_properties10SomeObjectC8bareIvarACvsTo{{(.ptrauth)?}}"
// CHECK:   }, {
// CHECK:     ptr @"\01L_selector_data(wobble)",
// CHECK:     ptr [[GETTER_SIGNATURE]],
// CHECK:     @"$s15objc_properties10SomeObjectC6wibbleACvgTo{{(.ptrauth)?}}"
// CHECK:   }, {
// CHECK:     ptr @"\01L_selector_data(setWobble:)",
// CHECK:     ptr [[SETTER_SIGNATURE]],
// CHECK:     @"$s15objc_properties10SomeObjectC6wibbleACvsTo{{(.ptrauth)?}}"
// CHECK:   }, {
// CHECK:     ptr @"\01L_selector_data(init)",
// CHECK:     ptr [[GETTER_SIGNATURE]],
// CHECK:     @"$s15objc_properties10SomeObjectCACycfcTo{{(.ptrauth)?}}"
// CHECK:   }]
// CHECK: }, section "__DATA, {{.*}}", align 8

// This appears earlier because it's also used in an ivar description.
// CHECK: [[BAREIVAR_NAME:@.*]] = private unnamed_addr constant [9 x i8] c"bareIvar\00"

// CHECK: [[READONLY_NAME:@.*]] = private unnamed_addr constant [9 x i8] c"readonly\00"
// CHECK: [[READONLY_ATTRS:@.*]] = private unnamed_addr constant [42 x i8] c"T@\22_TtC15objc_properties10SomeObject\22,N,R\00"

// CHECK: [[READWRITE_NAME:@.*]] = private unnamed_addr constant [10 x i8] c"readwrite\00"
// CHECK: [[READWRITE_ATTRS:@.*]] = private unnamed_addr constant [42 x i8] c"T@\22_TtC15objc_properties10SomeObject\22,N,&\00"

// CHECK: [[BAREIVAR_ATTRS:@.*]] = private unnamed_addr constant [52 x i8] c"T@\22_TtC15objc_properties10SomeObject\22,N,&,VbareIvar\00"

// CHECK: [[WIBBLE_NAME:@.*]] = private unnamed_addr constant [7 x i8] c"wobble\00"
// CHECK: [[WIBBLE_ATTRS:@.*]] = private unnamed_addr constant [50 x i8] c"T@\22_TtC15objc_properties10SomeObject\22,N,&,Vwibble\00"

// CHECK: @_PROPERTIES__TtC15objc_properties10SomeObject = internal constant { {{.*}}] } {
// CHECK:   i32 16,
// CHECK:   i32 4,
// CHECK:   [4 x { ptr, ptr }] [{
// CHECK:     ptr [[READONLY_NAME]],
// CHECK:     ptr [[READONLY_ATTRS]]
// CHECK:   }, {
// CHECK:     ptr [[READWRITE_NAME]],
// CHECK:     ptr [[READWRITE_ATTRS]]
// CHECK:   }, {
// CHECK:     ptr [[BAREIVAR_NAME]],
// CHECK:     ptr [[BAREIVAR_ATTRS]]
// CHECK:   }, {
// CHECK:     ptr [[WIBBLE_NAME]],
// CHECK:     ptr [[WIBBLE_ATTRS]]
// CHECK:   }]
// CHECK: }, section "__DATA, {{.*}}", align 8

// CHECK: @_DATA__TtC15objc_properties10SomeObject = internal constant { {{.+}} } {
// CHECK:   i32 {{[0-9]+}}, i32 {{[0-9]+}}, i32 {{[0-9]+}}, i32 {{[0-9]+}},
// CHECK:   ptr null,
// CHECK:   ptr {{@[^,]+}},
// CHECK:   ptr @_INSTANCE_METHODS__TtC15objc_properties10SomeObject{{(\.ptrauth)?}}
// CHECK:   ptr null,
// CHECK:   ptr @_IVARS__TtC15objc_properties10SomeObject,
// CHECK:   ptr null,
// CHECK:   ptr @_PROPERTIES__TtC15objc_properties10SomeObject
// CHECK: }, section "__DATA, {{.*}}", align 8

// CHECK: @"_CATEGORY_INSTANCE_METHODS__TtC15objc_properties10SomeObject_$_objc_properties" = internal constant { {{.*}}] } {
// CHECK:   i32 24,
// CHECK:   i32 2,
// CHECK:   [2 x { ptr, ptr, ptr }] [{
// CHECK:     { ptr @"\01L_selector_data(extensionProperty)",
// CHECK:     ptr [[GETTER_SIGNATURE]],
// CHECK:     @"$s15objc_properties10SomeObjectC17extensionPropertyACvgTo{{(.ptrauth)?}}"
// CHECK:   }, {
// CHECK:     ptr @"\01L_selector_data(setExtensionProperty:)",
// CHECK:     ptr [[SETTER_SIGNATURE]],
// CHECK:     @"$s15objc_properties10SomeObjectC17extensionPropertyACvsTo{{(.ptrauth)?}}"
// CHECK:   }]
// CHECK: }, section "__DATA, {{.*}}", align 8

// CHECK: [[EXTENSIONPROPERTY_NAME:@.*]] = private unnamed_addr constant [18 x i8] c"extensionProperty\00"

// CHECK: @"_CATEGORY_PROPERTIES__TtC15objc_properties10SomeObject_$_objc_properties" = internal constant { {{.*}}] } {
// CHECK:   i32 16,
// CHECK:   i32 1,
// CHECK:   [1 x { ptr, ptr }] [{
// CHECK:     ptr [[EXTENSIONPROPERTY_NAME]],
// CHECK:     ptr [[READWRITE_ATTRS]]
// CHECK:   }]
// CHECK: }, section "__DATA, {{.*}}", align 8

// CHECK-NEW: [[EXTENSIONCLASSPROPERTY_NAME:@.*]] = private unnamed_addr constant [19 x i8] c"extensionClassProp\00"
// CHECK-NEW: [[EXTENSIONCLASSPROPERTY_ATTRS:@.*]] = private unnamed_addr constant [7 x i8] c"T#,N,R\00"
// CHECK-NEW: [[EXTENSIONSTATICPROPERTY_NAME:@.*]] = private unnamed_addr constant [26 x i8] c"extensionStoredStaticProp\00"

// CHECK-NEW: @"_CATEGORY_CLASS_PROPERTIES__TtC15objc_properties10SomeObject_$_objc_properties" = internal constant { {{.*}}] } {
// CHECK-NEW:   i32 16,
// CHECK-NEW:   i32 2,
// CHECK-NEW:   [2 x { ptr, ptr }] [{
// CHECK-NEW:     ptr [[EXTENSIONCLASSPROPERTY_NAME]],
// CHECK-NEW:     ptr [[EXTENSIONCLASSPROPERTY_ATTRS]]
// CHECK-NEW:   }, {
// CHECK-NEW:	  ptr [[EXTENSIONSTATICPROPERTY_NAME]],
// CHECK-NEW:	  ptr [[SHARED_ATTRS]] }]
// CHECK-NEW: }, section "__DATA, {{.*}}", align 8

// CHECK: @"_CATEGORY__TtC15objc_properties10SomeObject_$_objc_properties" = internal constant { {{.+}} } {
// CHECK:   ptr {{@[^,]+}},
// CHECK:   @"$s15objc_properties10SomeObjectCMf", i32 0, i32 3
// CHECK:   ptr @"_CATEGORY_INSTANCE_METHODS__TtC15objc_properties10SomeObject_$_objc_properties{{(\.ptrauth)?}}"
// CHECK:   ptr @"_CATEGORY_CLASS_METHODS__TtC15objc_properties10SomeObject_$_objc_properties{{(\.ptrauth)?}}"
// CHECK:   ptr null,
// CHECK:   ptr @"_CATEGORY_PROPERTIES__TtC15objc_properties10SomeObject_$_objc_properties{{(\.ptrauth)?}}", 
// CHECK-NEW:   ptr @"_CATEGORY_CLASS_PROPERTIES__TtC15objc_properties10SomeObject_$_objc_properties",
// CHECK-OLD:   ptr null,
// CHECK:   i32 60
// CHECK: }, section "__DATA, {{.*}}", align 8


// CHECK: @_INSTANCE_METHODS__TtC15objc_properties4Tree =
// CHECK:    ptr @"\01L_selector_data(parent)",
// CHECK:    ptr [[GETTER_SIGNATURE]],
// CHECK:    @"$s15objc_properties4TreeC6parentACSgvgTo{{(.ptrauth)?}}"
// CHECK:    ptr @"\01L_selector_data(setParent:)",
// CHECK:    ptr [[SETTER_SIGNATURE]],
// CHECK:    @"$s15objc_properties4TreeC6parentACSgvsTo{{(.ptrauth)?}}"

// CHECK: @_PROTOCOL__TtP15objc_properties5Proto_ = weak hidden constant { {{.+}} } {
// CHECK:   ptr null,
// CHECK:   ptr {{@[^,]+}},
// CHECK:   ptr null,
// CHECK:   ptr @_PROTOCOL_INSTANCE_METHODS__TtP15objc_properties5Proto_,
// CHECK:   ptr @_PROTOCOL_CLASS_METHODS__TtP15objc_properties5Proto_,
// CHECK:   ptr null,
// CHECK:   ptr null,
// CHECK:   ptr @_PROTOCOL_PROPERTIES__TtP15objc_properties5Proto_,
// CHECK:   i32 96, i32 1,
// CHECK:   ptr @_PROTOCOL_METHOD_TYPES__TtP15objc_properties5Proto_,
// CHECK:   ptr null,
// CHECK-NEW:   ptr @_PROTOCOL_CLASS_PROPERTIES__TtP15objc_properties5Proto_
// CHECK-OLD:   ptr null
// CHECK: }, section "__DATA, {{.*}}", align 8


// CHECK: [[PROTOCOLPROPERTY_NAME:@.+]] = private unnamed_addr constant [6 x i8] c"value\00"
// CHECK: [[PROTOCOLPROPERTY_ATTRS:@.+]] = private unnamed_addr constant [7 x i8] c"Tq,N,R\00"

// CHECK: @_PROTOCOL_PROPERTIES__TtP15objc_properties5Proto_ = weak hidden constant { {{.*}}] } {
// CHECK:   i32 16,
// CHECK:   i32 1,
// CHECK:   [1 x { ptr, ptr }] [{
// CHECK:     ptr [[PROTOCOLPROPERTY_NAME]],
// CHECK:     ptr [[PROTOCOLPROPERTY_ATTRS]]
// CHECK:   }]
// CHECK: }, section "__DATA, {{.*}}", align 8

// CHECK-NEW: [[PROTOCOLCLASSPROPERTY_NAME:@.+]] = private unnamed_addr constant [15 x i8] c"sharedInstance\00"
// CHECK-NEW: [[PROTOCOLCLASSPROPERTY_ATTRS:@.+]] = private unnamed_addr constant [7 x i8] c"T@,N,&\00"

// CHECK-NEW: @_PROTOCOL_CLASS_PROPERTIES__TtP15objc_properties5Proto_ = weak hidden constant { {{.*}}] } {
// CHECK-NEW:   i32 16,
// CHECK-NEW:   i32 1,
// CHECK-NEW:   [1 x { ptr, ptr }] [{
// CHECK-NEW:     ptr [[PROTOCOLCLASSPROPERTY_NAME]],
// CHECK-NEW:     ptr [[PROTOCOLCLASSPROPERTY_ATTRS]]
// CHECK-NEW:   }]
// CHECK-NEW: }, section "__DATA, {{.*}}", align 8
