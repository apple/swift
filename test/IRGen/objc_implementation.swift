// Test doesn't pass on all platforms (rdar://101420862)
// REQUIRES: OS=macosx

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -enable-experimental-feature CImplementation -I %S/Inputs/abi -F %clang-importer-sdk-path/frameworks %s -import-objc-header %S/Inputs/objc_implementation.h -emit-ir -target %target-future-triple > %t.ir
// RUN: %FileCheck --input-file %t.ir %s
// RUN: %FileCheck --input-file %t.ir --check-prefix NEGATIVE %s
// REQUIRES: objc_interop

// CHECK-DAG: @"$sSo36ImplClassWithResilientStoredPropertyC19objc_implementationE9beforeInts5Int32VvpWvd" = hidden global i64 8, align 8
// CHECK-DAG: @"$sSo36ImplClassWithResilientStoredPropertyC19objc_implementationE6mirrors6MirrorVSgvpWvd" = hidden global i64 0, align 8
// CHECK-DAG: @"$sSo36ImplClassWithResilientStoredPropertyC19objc_implementationE13afterIntFinals5Int32VvpWvd" = hidden global i64 0, align 8
// CHECK-DAG: @"$sSo36ImplClassWithResilientStoredPropertyC19objc_implementationE8afterInts5Int32VvpWvd" = hidden global i64 0, align 8

//
// @_objcImplementation class
//

// Metaclass
// CHECK: @"OBJC_METACLASS_$_ImplClass" = global %objc_class { ptr @"OBJC_METACLASS_$_NSObject", ptr @"OBJC_METACLASS_$_NSObject", ptr @_objc_empty_cache, ptr null, {{.*}}ptr [[_METACLASS_DATA_ImplClass:@[^, ]+]]{{.*}}, align 8
// CHECK: @_PROTOCOLS_ImplClass = internal constant { i64, [2 x ptr] } { i64 2, [2 x ptr] [ptr @"_OBJC_PROTOCOL_$_NSCopying", ptr @"_OBJC_PROTOCOL_$_NSMutableCopying"] }, section "__DATA, __objc_const", align 8
// CHECK: [[_METACLASS_DATA_ImplClass]] = internal constant { i32, i32, i32, i32, ptr, ptr, ptr, ptr, ptr, ptr, ptr } { i32 129, i32 40, i32 40, i32 0, ptr null, ptr @.str.9.ImplClass, ptr null, ptr @_PROTOCOLS_ImplClass, ptr null, ptr null, ptr null }, section "__DATA, __objc_const", align 8
// TODO: Why the extra i32 field above?

// Class
// CHECK: [[selector_data__cxx_destruct:@[^, ]+]] = private global [14 x i8] c".cxx_destruct\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK-LABEL: @_INSTANCE_METHODS_ImplClass = internal constant { i32, i32, [10 x { ptr, ptr, ptr }] } { i32 24, i32 10, [10 x { ptr, ptr, ptr }] [
// CHECK-SAME: { ptr, ptr, ptr } { ptr @"\01L_selector_data(init)", ptr @".str.7.@16@0:8", ptr @"$sSo9ImplClassC19objc_implementationEABycfcTo{{(\.ptrauth)?}}" }
// CHECK-SAME: { ptr, ptr, ptr } { ptr @"\01L_selector_data(implProperty)", ptr @".str.7.i16@0:8", ptr @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvgTo{{(\.ptrauth)?}}" }
// CHECK-SAME: { ptr, ptr, ptr } { ptr @"\01L_selector_data(setImplProperty:)", ptr @".str.10.v20@0:8i16", ptr @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvsTo{{(\.ptrauth)?}}" }
// CHECK-SAME: { ptr, ptr, ptr } { ptr @"\01L_selector_data(mainMethod:)", ptr @".str.10.v20@0:8i16", ptr @"$sSo9ImplClassC19objc_implementationE10mainMethodyys5Int32VFTo{{(\.ptrauth)?}}" }
// CHECK-SAME: { ptr, ptr, ptr } { ptr @"\01L_selector_data(asyncMethodWithCompletionHandler:)", ptr @".str.16.v24@0:8@?<v@?>16", ptr @"$sSo9ImplClassC19objc_implementationE11asyncMethodyyYaFTo{{(\.ptrauth)?}}" }
// CHECK-SAME: { ptr, ptr, ptr } { ptr @"\01L_selector_data(extensionMethod:)", ptr @".str.10.v20@0:8i16", ptr @"$sSo9ImplClassC19objc_implementationE15extensionMethodyys5Int32VFTo{{(\.ptrauth)?}}" }
// CHECK-SAME: { ptr, ptr, ptr } { ptr @"\01L_selector_data(copyWithZone:)", ptr @".str.11.@24@0:8^v16", ptr @"$sSo9ImplClassC19objc_implementationE4copy4withypSg10ObjectiveC6NSZoneVSg_tFTo{{(\.ptrauth)?}}" }
// CHECK-SAME: { ptr, ptr, ptr } { ptr @"\01L_selector_data(mutableCopyWithZone:)", ptr @".str.11.@24@0:8^v16", ptr @"$sSo9ImplClassC19objc_implementationE11mutableCopy4withypSg10ObjectiveC6NSZoneVSg_tFTo{{(\.ptrauth)?}}" }
// CHECK-SAME: { ptr, ptr, ptr } { ptr @"\01L_selector_data(dealloc)", ptr @".str.7.v16@0:8", ptr @"$sSo9ImplClassC19objc_implementationEfDTo{{(\.ptrauth)?}}" }, { ptr, ptr, ptr } { ptr [[selector_data__cxx_destruct]], ptr @".str.7.v16@0:8", ptr @"$sSo9ImplClassCfETo{{(\.ptrauth)?}}" }
// CHECK-LABEL: @_IVARS_ImplClass = internal constant { i32, i32, [2 x { ptr, ptr, ptr, i32, i32 }] } { i32 32, i32 2, [2 x { ptr, ptr, ptr, i32, i32 }] [
// CHECK-SAME: { ptr, ptr, ptr, i32, i32 } { ptr @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvpWvd", ptr @.str.12.implProperty, ptr @.str.0., i32 2, i32 4 }
// CHECK-SAME: { ptr, ptr, ptr, i32, i32 } { ptr @"$sSo9ImplClassC19objc_implementationE13implProperty2So8NSObjectCSgvpWvd", ptr @.str.13.implProperty2, ptr @.str.0., i32 3, i32 8 }
// CHECK-LABEL: @_PROPERTIES_ImplClass = internal constant { i32, i32, [1 x { ptr, ptr }] } { i32 16, i32 1, [1 x { ptr, ptr }] [
// CHECK-SAME: { ptr, ptr } { ptr @.str.12.implProperty, ptr @".str.18.Ti,N,VimplProperty" }
// FIXME: Should just be @_PROTOCOLS_ImplClass, but an IRGen bug causes the protocol list to be emitted twice.
// CHECK: [[_DATA_ImplClass:@[^, ]+]] = internal constant { i32, i32, i32, i32, ptr, ptr, ptr, ptr, ptr, ptr, ptr } { i32 388, i32 8, i32 24, i32 0, ptr null, ptr @.str.9.ImplClass, ptr @_INSTANCE_METHODS_ImplClass, ptr @_PROTOCOLS_ImplClass.{{[0-9]+}}, ptr @_IVARS_ImplClass, ptr null, ptr @_PROPERTIES_ImplClass }, section "__DATA, __objc_data", align 8
// CHECK: @"OBJC_CLASS_$_ImplClass" = global <{ i64, ptr, ptr, ptr, i64 }> <{ i64 ptrtoint (ptr @"OBJC_METACLASS_$_ImplClass" to i64), ptr @"OBJC_CLASS_$_NSObject", ptr @_objc_empty_cache, ptr null, i64 ptrtoint (ptr [[_DATA_ImplClass]] to i64) }>, section "__DATA,__objc_data, regular", align 8

// Swift metadata
// NEGATIVE-NOT: OBJC_CLASS_$_ImplClass.{{[0-9]}}
// NEGATIVE-NOT: $sSo9ImplClassCMn
// NEGATIVE-NOT: $sSo9ImplClassCHn
// NEGATIVE-NOT: $sSo9ImplClassCN
// NEGATIVE-NOT: $sSo9ImplClassCMf
@_objcImplementation extension ImplClass {
  @objc override init() {
    implProperty = 42
    implProperty2 = NSObject()
    super.init()
  }
  
  @objc var implProperty: Int32 {
    didSet { print(implProperty) }
  }
  
  final var implProperty2: NSObject?
  
  @objc func mainMethod(_: Int32) { print(implProperty) }
  @objc func asyncMethod() async {}
  @objc func extensionMethod(_: Int32) {}

  @objc(copyWithZone:)
  func copy(with zone: NSZone?) -> Any? {
    let copy = ImplClass()
    copy.implProperty = implProperty
    copy.implProperty2 = implProperty2
    return copy
  }
  
  @objc(mutableCopyWithZone:)
  func mutableCopy(with zone: NSZone?) -> Any? {
    return copy(with: zone)
  }

  deinit { print(implProperty) }
}

//
// @_objcImplementation category
//

// Category1
// CHECK: [[selector_data_category1Method_:@[^, ]+]] = private global [17 x i8] c"category1Method:\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK: [[_CATEGORY_INSTANCE_METHODS_ImplClass_Category1:@[^, ]+]] = internal constant { i32, i32, [1 x { ptr, ptr, ptr }] } { i32 24, i32 1, [1 x { ptr, ptr, ptr }] [{ ptr, ptr, ptr } { ptr [[selector_data_category1Method_]], ptr @".str.10.v20@0:8i16", ptr @"$sSo9ImplClassC19objc_implementationE15category1Methodyys5Int32VFTo{{(\.ptrauth)?}}" }] }, section "__DATA, __objc_data", align 8
// CHECK: [[_CATEGORY_ImplClass_Category1:@[^, ]+]] = internal constant { ptr, ptr, ptr, ptr, ptr, ptr, ptr, i32 } { ptr @.str.9.Category1, ptr @"OBJC_CLASS_$_ImplClass", ptr [[_CATEGORY_INSTANCE_METHODS_ImplClass_Category1]], ptr null, ptr null, ptr null, ptr null, i32 60 }, section "__DATA, __objc_const", align 8
@_objcImplementation(Category1) extension ImplClass {
  @objc func category1Method(_: Int32) {
    print("category1Method")
  }
}

//
// Second @_objcImplementation class, inherited initializer
//

// CHECK: @"OBJC_METACLASS_$_NoInitImplClass" = global %objc_class { ptr @"OBJC_METACLASS_$_NSObject", ptr @"OBJC_METACLASS_$_NSObject", ptr @_objc_empty_cache, ptr null, i64 ptrtoint (ptr @_METACLASS_DATA_NoInitImplClass to i64) }, align 8
// CHECK: @_METACLASS_DATA_NoInitImplClass = internal constant { i32, i32, i32, i32, ptr, ptr, ptr, ptr, ptr, ptr, ptr } { i32 129, i32 40, i32 40, i32 0, ptr null, ptr @.str.15.NoInitImplClass, ptr null, ptr null, ptr null, ptr null, ptr null }, section "__DATA, __objc_const", align 8
// CHECK: @_INSTANCE_METHODS_NoInitImplClass = internal constant { i32, i32, [9 x { ptr, ptr, ptr }] } { i32 24, i32 9, [9 x { ptr, ptr, ptr }] [{ ptr, ptr, ptr } { ptr @"\01L_selector_data(s1)", ptr @".str.7.@16@0:8", ptr @"$sSo15NoInitImplClassC19objc_implementationE2s1SSvgTo" }, { ptr, ptr, ptr } { ptr @"\01L_selector_data(s2)", ptr @".str.7.@16@0:8", ptr @"$sSo15NoInitImplClassC19objc_implementationE2s2SSvgTo" }, { ptr, ptr, ptr } { ptr @"\01L_selector_data(setS2:)", ptr @".str.10.v24@0:8@16", ptr @"$sSo15NoInitImplClassC19objc_implementationE2s2SSvsTo" }, { ptr, ptr, ptr } { ptr @"\01L_selector_data(s3)", ptr @".str.7.@16@0:8", ptr @"$sSo15NoInitImplClassC19objc_implementationE2s3SSvgTo" }, { ptr, ptr, ptr } { ptr @"\01L_selector_data(s4)", ptr @".str.7.@16@0:8", ptr @"$sSo15NoInitImplClassC19objc_implementationE2s4SSvgTo" }, { ptr, ptr, ptr } { ptr @"\01L_selector_data(setS4:)", ptr @".str.10.v24@0:8@16", ptr @"$sSo15NoInitImplClassC19objc_implementationE2s4SSvsTo" }, { ptr, ptr, ptr } { ptr @"\01L_selector_data(dealloc)", ptr @".str.7.v16@0:8", ptr @"$sSo15NoInitImplClassC19objc_implementationEfDTo" }, { ptr, ptr, ptr } { ptr @"\01L_selector_data(init)", ptr @".str.7.@16@0:8", ptr @"$sSo15NoInitImplClassC19objc_implementationEABycfcTo" }, { ptr, ptr, ptr } { ptr @"\01L_selector_data(.cxx_destruct)", ptr @".str.7.v16@0:8", ptr @"$sSo15NoInitImplClassCfETo" }] }, section "__DATA, __objc_data", align 8
// CHECK: @_IVARS_NoInitImplClass = internal constant { i32, i32, [4 x { ptr, ptr, ptr, i32, i32 }] } { i32 32, i32 4, [4 x { ptr, ptr, ptr, i32, i32 }] [{ ptr, ptr, ptr, i32, i32 } { ptr @"$sSo15NoInitImplClassC19objc_implementationE2s1SSvpWvd", ptr @.str.2.s1, ptr @.str.0., i32 3, i32 16 }, { ptr, ptr, ptr, i32, i32 } { ptr @"$sSo15NoInitImplClassC19objc_implementationE2s2SSvpWvd", ptr @.str.2.s2, ptr @.str.0., i32 3, i32 16 }, { ptr, ptr, ptr, i32, i32 } { ptr @"$sSo15NoInitImplClassC19objc_implementationE2s3SSvpWvd", ptr @.str.2.s3, ptr @.str.0., i32 3, i32 16 }, { ptr, ptr, ptr, i32, i32 } { ptr @"$sSo15NoInitImplClassC19objc_implementationE2s4SSvpWvd", ptr @.str.2.s4, ptr @.str.0., i32 3, i32 16 }] }, section "__DATA, __objc_const", align 8
// CHECK: @_PROPERTIES_NoInitImplClass = internal constant { i32, i32, [4 x { ptr, ptr }] } { i32 16, i32 4, [4 x { ptr, ptr }] [{ ptr, ptr } { ptr @.str.2.s1, ptr @".str.16.T@\22NSString\22,N,R" }, { ptr, ptr } { ptr @.str.2.s2, ptr @".str.16.T@\22NSString\22,N,C" }, { ptr, ptr } { ptr @.str.2.s3, ptr @".str.16.T@\22NSString\22,N,R" }, { ptr, ptr } { ptr @.str.2.s4, ptr @".str.16.T@\22NSString\22,N,C" }] }, section "__DATA, __objc_const", align 8
// CHECK: @_DATA_NoInitImplClass = internal constant { i32, i32, i32, i32, ptr, ptr, ptr, ptr, ptr, ptr, ptr } { i32 388, i32 8, i32 72, i32 0, ptr null, ptr @.str.15.NoInitImplClass, ptr @_INSTANCE_METHODS_NoInitImplClass, ptr null, ptr @_IVARS_NoInitImplClass, ptr null, ptr @_PROPERTIES_NoInitImplClass }, section "__DATA, __objc_data", align 8
// CHECK: @"OBJC_CLASS_$_NoInitImplClass" = global <{ i64, ptr, ptr, ptr, i64 }> <{ i64 ptrtoint (ptr @"OBJC_METACLASS_$_NoInitImplClass" to i64), ptr @"OBJC_CLASS_$_NSObject", ptr @_objc_empty_cache, ptr null, i64 ptrtoint (ptr @_DATA_NoInitImplClass to i64) }>, section "__DATA,__objc_data, regular", align 8
@_objcImplementation extension NoInitImplClass {
  @objc let s1 = "s1v"
  @objc var s2 = "s2v"
  @objc(s3) let s3 = "s3v"
  @objc(s4) var s4 = "s4v"

  deinit {
    s2.removeAll()
    s4.removeAll()
  }
}

//
// @objc subclass of @_objcImplementation class
//

// Metaclass
// CHECK: @"OBJC_METACLASS_$__TtC19objc_implementation13SwiftSubclass" = global %objc_class { ptr @"OBJC_METACLASS_$_NSObject", ptr @"OBJC_METACLASS_$_ImplClass", ptr @_objc_empty_cache, ptr null, {{i64 ptrtoint|%swift.opaque\* bitcast}} (ptr [[_METACLASS_DATA_SwiftSubclass:@[^, ]+]] to {{i64|%swift.opaque\*}}) }, align 8
// CHECK: [[_METACLASS_DATA_SwiftSubclass]] = internal constant { i32, i32, i32, i32, ptr, ptr, ptr, ptr, ptr, ptr, ptr } { i32 129, i32 40, i32 40, i32 0, ptr null, ptr @.str.40._TtC19objc_implementation13SwiftSubclass, ptr null, ptr null, ptr null, ptr null, ptr null }, section "__DATA, __objc_const", align 8

// Class
// CHECK: [[_INSTANCE_METHODS_SwiftSubclass:@[^, ]+]] = internal constant { i32, i32, [2 x { ptr, ptr, ptr }] } { i32 24, i32 2, [2 x { ptr, ptr, ptr }] [{ ptr, ptr, ptr } { ptr @"\01L_selector_data(mainMethod:)", ptr @".str.10.v20@0:8i16", ptr @"$s19objc_implementation13SwiftSubclassC10mainMethodyys5Int32VFTo{{(\.ptrauth)?}}" }, { ptr, ptr, ptr } { ptr @"\01L_selector_data(init)", ptr @".str.7.@16@0:8", ptr @"$s19objc_implementation13SwiftSubclassCACycfcTo{{(\.ptrauth)?}}" }] }, section "__DATA, __objc_data", align 8
// CHECK: [[_DATA_SwiftSubclass:@[^, ]+]] = internal constant { i32, i32, i32, i32, ptr, ptr, ptr, ptr, ptr, ptr, ptr } { i32 128, i32 24, i32 24, i32 0, ptr null, ptr @.str.40._TtC19objc_implementation13SwiftSubclass, ptr [[_INSTANCE_METHODS_SwiftSubclass]], ptr null, ptr null, ptr null, ptr null }, section "__DATA, __objc_data", align 8

// Swift metadata
// CHECK: @"$s19objc_implementationMXM" = linkonce_odr hidden constant <{ i32, i32, i32 }> <{ i32 0, i32 0, i32 trunc (i64 sub (i64 ptrtoint (ptr @.str.19.objc_implementation to i64), i64 ptrtoint (ptr getelementptr inbounds (<{ i32, i32, i32 }>, ptr @"$s19objc_implementationMXM", i32 0, i32 2) to i64)) to i32) }>, section "__TEXT,__constg_swiftt", align 4
// CHECK: @"$s19objc_implementation13SwiftSubclassCMn" = constant <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }> <{ i32 80, i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s19objc_implementationMXM" to i64), i64 ptrtoint (ptr getelementptr inbounds (<{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>, ptr @"$s19objc_implementation13SwiftSubclassCMn", i32 0, i32 1) to i64)) to i32), i32 trunc (i64 sub (i64 ptrtoint (ptr @.str.13.SwiftSubclass to i64), i64 ptrtoint (ptr getelementptr inbounds (<{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>, ptr @"$s19objc_implementation13SwiftSubclassCMn", i32 0, i32 2) to i64)) to i32), i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s19objc_implementation13SwiftSubclassCMa" to i64), i64 ptrtoint (ptr getelementptr inbounds (<{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>, ptr @"$s19objc_implementation13SwiftSubclassCMn", i32 0, i32 3) to i64)) to i32), i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s19objc_implementation13SwiftSubclassCMF" to i64), i64 ptrtoint (ptr getelementptr inbounds (<{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>, ptr @"$s19objc_implementation13SwiftSubclassCMn", i32 0, i32 4) to i64)) to i32), i32 trunc (i64 sub (i64 ptrtoint (ptr @"symbolic So9ImplClassC" to i64), i64 ptrtoint (ptr getelementptr inbounds (<{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>, ptr @"$s19objc_implementation13SwiftSubclassCMn", i32 0, i32 5) to i64)) to i32), i32 3, i32 10, i32 0, i32 0, i32 10 }>, section "__TEXT,__constg_swiftt", align 4
// CHECK: @"$s19objc_implementation13SwiftSubclassCMf" = internal global <{ ptr, ptr, ptr, i64, ptr, ptr, ptr, i64, i32, i32, i32, i16, i16, i32, i32, ptr, ptr }> <{ ptr null, ptr @"$s19objc_implementation13SwiftSubclassCfD", ptr @"$sBOWV", i64 ptrtoint (ptr @"OBJC_METACLASS_$__TtC19objc_implementation13SwiftSubclass" to i64), ptr @"OBJC_CLASS_$_ImplClass", ptr @_objc_empty_cache, ptr null, i64 add (i64 ptrtoint (ptr @_DATA__TtC19objc_implementation13SwiftSubclass to i64), i64 2), i32 0, i32 0, i32 24, i16 7, i16 0, i32 104, i32 24, ptr @"$s19objc_implementation13SwiftSubclassCMn", ptr null }>, section "__DATA,__objc_data, regular", align 8
// CHECK: @"symbolic _____ 19objc_implementation13SwiftSubclassC" = linkonce_odr hidden constant <{ i8, i32, i8 }> <{ i8 1, i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s19objc_implementation13SwiftSubclassCMn" to i64), i64 ptrtoint (ptr getelementptr inbounds (<{ i8, i32, i8 }>, ptr @"symbolic _____ 19objc_implementation13SwiftSubclassC", i32 0, i32 1) to i64)) to i32), i8 0 }>, section "__TEXT,__swift5_typeref, regular"{{.*}}, align 2
// CHECK: @"$s19objc_implementation13SwiftSubclassCMF" = internal constant { i32, i32, i16, i16, i32 } { i32 trunc (i64 sub (i64 ptrtoint (ptr @"symbolic _____ 19objc_implementation13SwiftSubclassC" to i64), i64 ptrtoint (ptr @"$s19objc_implementation13SwiftSubclassCMF" to i64)) to i32), i32 trunc (i64 sub (i64 ptrtoint (ptr @"symbolic So9ImplClassC" to i64), i64 ptrtoint (ptr getelementptr inbounds ({ i32, i32, i16, i16, i32 }, ptr @"$s19objc_implementation13SwiftSubclassCMF", i32 0, i32 1) to i64)) to i32), i16 7, i16 12, i32 0 }, section "__TEXT,__swift5_fieldmd, regular"{{.*}}, align 4
open class SwiftSubclass: ImplClass {
  override open func mainMethod(_: Int32) {
    print("subclass mainMethod")
  }
}

//
// @_objcImplementation class with a resilient stored property
//

// Metaclass
// CHECK: @"OBJC_METACLASS_$_ImplClassWithResilientStoredProperty" = global %objc_class { ptr @"OBJC_METACLASS_$_NSObject", ptr @"OBJC_METACLASS_$_NSObject", ptr @_objc_empty_cache, ptr null, {{.*}}ptr @_METACLASS_DATA_ImplClassWithResilientStoredProperty{{.*}}, align 8
// CHECK: @_METACLASS_DATA_ImplClassWithResilientStoredProperty = internal constant { i32, i32, i32, i32, ptr, ptr, ptr, ptr, ptr, ptr, ptr } { i32 129, i32 40, i32 40, i32 0, ptr null, ptr @.str.36.ImplClassWithResilientStoredProperty, ptr null, ptr null, ptr null, ptr null, ptr null }, section "__DATA, __objc_const", align 8
// TODO: Why the extra i32 field above?

// Class
// CHECK: @_IVARS_ImplClassWithResilientStoredProperty = internal constant { i32, i32, [4 x { ptr, ptr, ptr, i32, i32 }] } { i32 32, i32 4, [4 x { ptr, ptr, ptr, i32, i32 }] [{ ptr, ptr, ptr, i32, i32 } { ptr @"$sSo36ImplClassWithResilientStoredPropertyC19objc_implementationE9beforeInts5Int32VvpWvd", ptr @.str.9.beforeInt, ptr @.str.0., i32 2, i32 4 }, { ptr, ptr, ptr, i32, i32 } { ptr @"$sSo36ImplClassWithResilientStoredPropertyC19objc_implementationE6mirrors6MirrorVSgvpWvd", ptr @.str.6.mirror, ptr @.str.0., i32 0, i32 0 }, { ptr, ptr, ptr, i32, i32 } { ptr @"$sSo36ImplClassWithResilientStoredPropertyC19objc_implementationE13afterIntFinals5Int32VvpWvd", ptr @.str.13.afterIntFinal, ptr @.str.0., i32 2, i32 4 }, { ptr, ptr, ptr, i32, i32 } { ptr @"$sSo36ImplClassWithResilientStoredPropertyC19objc_implementationE8afterInts5Int32VvpWvd", ptr @.str.8.afterInt, ptr @.str.0., i32 2, i32 4 }] }, section "__DATA, __objc_const", align 8
// CHECK: @_DATA_ImplClassWithResilientStoredProperty = internal constant { i32, i32, i32, i32, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr } { i32 452, i32 8, i32 20, i32 0, ptr null, ptr @.str.36.ImplClassWithResilientStoredProperty, ptr @_INSTANCE_METHODS_ImplClassWithResilientStoredProperty, ptr null, ptr @_IVARS_ImplClassWithResilientStoredProperty, ptr null, ptr @_PROPERTIES_ImplClassWithResilientStoredProperty, ptr @"$sSo36ImplClassWithResilientStoredPropertyCMU" }, section "__DATA, __objc_data", align 8
// CHECK: @"OBJC_CLASS_$_ImplClassWithResilientStoredProperty" = global <{ i64, ptr, ptr, ptr, i64 }> <{ i64 ptrtoint (ptr @"OBJC_METACLASS_$_ImplClassWithResilientStoredProperty" to i64), ptr @"OBJC_CLASS_$_NSObject", ptr @_objc_empty_cache, ptr null, i64 ptrtoint (ptr @_DATA_ImplClassWithResilientStoredProperty to i64) }>, section "__DATA,__objc_data, regular", align 8

@_objcImplementation extension ImplClassWithResilientStoredProperty {
  @objc var beforeInt: Int32 = 0
  final var mirror: Mirror?
  final var afterIntFinal: Int32 = 0
  @objc var afterInt: Int32 = 0
}

//
// Epilogue
//

// CHECK: @"objc_classes_OBJC_CLASS_$_ImplClass" = internal global ptr @"OBJC_CLASS_$_ImplClass", section "__DATA,__objc_classlist,regular,no_dead_strip"{{.*}}, align 8
// CHECK: @"objc_classes_$s19objc_implementation13SwiftSubclassCN" = internal global ptr @"$s19objc_implementation13SwiftSubclassCN", section "__DATA,__objc_classlist,regular,no_dead_strip"{{.*}}, align 8
// CHECK: @objc_categories = internal global [1 x ptr] [ptr [[_CATEGORY_ImplClass_Category1]]], section "__DATA,__objc_catlist,regular,no_dead_strip"{{.*}}, align 8

// CHECK: @"$s19objc_implementation13SwiftSubclassCN" = alias %swift.type, getelementptr inbounds (<{ ptr, ptr, ptr, i64, ptr, ptr, ptr, i64, i32, i32, i32, i16, i16, i32, i32, ptr, ptr }>, ptr @"$s19objc_implementation13SwiftSubclassCMf", i32 0, i32 3)
// CHECK: @"OBJC_CLASS_$__TtC19objc_implementation13SwiftSubclass" = alias %swift.type, ptr @"$s19objc_implementation13SwiftSubclassCN"

//
// Functions
//

@_objcImplementation @_cdecl("implFunc")
public func implFunc(_ param: Int32) {}

@_objcImplementation @_cdecl("implFuncCName")
public func implFuncCName(_ param: Int32) {}

public func fn(impl: ImplClass, swiftSub: SwiftSubclass) {
  impl.mainMethod(0)
  swiftSub.mainMethod(1)
  implFunc(2)
  implFuncCName(3)
}

// Swift calling convention -[ImplClass init]
// CHECK-LABEL: define hidden swiftcc ptr @"$sSo9ImplClassC19objc_implementationEABycfc"
  // Access in this function should be DirectToStorage
  // CHECK-NOT: @"\01L_selector(implProperty)"
  // CHECK: @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvpWvd"
  // CHECK-NOT: @"\01L_selector(implProperty)"

// ObjC calling convention -[ImplClass init]
// CHECK-LABEL: define internal ptr @"$sSo9ImplClassC19objc_implementationEABycfcTo"

// ObjC calling convention -[ImplClass implProperty]
// CHECK-LABEL: define internal i32 @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvgTo"

// Swift calling convention -[ImplClass implProperty]
// CHECK-LABEL: define hidden swiftcc i32 @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32Vvg"
  // Access in this function should be DirectToStorage
  // CHECK-NOT: @"\01L_selector(implProperty)"
  // CHECK: @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvpWvd"
  // CHECK-NOT: @"\01L_selector(implProperty)"

// ObjC calling convention -[ImplClass setImplProperty:]
// CHECK-LABEL: define internal void @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvsTo"

// Swift calling convention -[ImplClass setImplProperty:]
// CHECK-LABEL: define hidden swiftcc void @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32Vvs"
  // Access in this function should be DirectToStorage
  // CHECK-NOT: @"\01L_selector(implProperty)"
  // CHECK: @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvpWvd"
  // CHECK-NOT: @"\01L_selector(implProperty)"

// Swift calling convention -[ImplClass setImplProperty:] didSet
// CHECK-LABEL: define internal swiftcc void @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvW"
  // Access in this function should be DirectToStorage
  // CHECK-NOT: @"\01L_selector(implProperty)"
  // CHECK: @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvpWvd"
  // CHECK-NOT: @"\01L_selector(implProperty)"

// Swift calling convention -[ImplClass mainMethod:]
// CHECK-LABEL: define hidden swiftcc void @"$sSo9ImplClassC19objc_implementationE10mainMethodyys5Int32VF"
  // Access in this function should be via message send
  // CHECK-NOT: @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvpWvd"
  // CHECK: @"\01L_selector(implProperty)"
  // CHECK-NOT: @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvpWvd"

// ObjC calling convention -[ImplClass mainMethod:]
// CHECK-LABEL: define internal void @"$sSo9ImplClassC19objc_implementationE10mainMethodyys5Int32VFTo"

// Swift calling convention -[ImplClass asyncMethodWithCompletion:]
// CHECK-LABEL: define hidden swifttailcc void @"$sSo9ImplClassC19objc_implementationE11asyncMethodyyYaF"

// ObjC calling convention -[ImplClass asyncMethodWithCompletion:]
// CHECK-LABEL: define internal void @"$sSo9ImplClassC19objc_implementationE11asyncMethodyyYaFTo"
// CHECK: call swiftcc void @"$ss29_runTaskForBridgedAsyncMethodyyyyYaYbcnF"(ptr @"$sSo9ImplClassC19objc_implementationE11asyncMethodyyYaFyyYacfU_To{{[^"]*}}"

// ObjC calling convention -[ImplClass asyncMethodWithCompletion:] (start of helper closure)
// CHECK-LABEL: define linkonce_odr hidden swifttailcc void @"$sSo9ImplClassC19objc_implementationE11asyncMethodyyYaFyyYacfU_To"
// CHECK: musttail call swifttailcc void @"$sSo9ImplClassC19objc_implementationE11asyncMethodyyYaF"

// ObjC calling convention -[ImplClass asyncMethodWithCompletion:] (end of helper closure)
// CHECK-LABEL: define internal swifttailcc void @"$sSo9ImplClassC19objc_implementationE11asyncMethodyyYaFyyYacfU_ToTQ0_"

// Make sure this function incorporates a nil check
// CHECK: [[IS_COMPLETION_NULL:%[0-9]+]] = icmp eq i64 {{%\.reload[0-9+]}}, 0
// CHECK: br i1 [[IS_COMPLETION_NULL]], label %[[FINISH:[^,]+]], label %[[RUN_COMPLETION:[^,]+]]
// CHECK: [[RUN_COMPLETION]]:

// The actual call to the block
// CHECK: call void {{%[0-9]+}}

// Final control flow for the nil check
// CHECK: br label %[[FINISH]]
// CHECK: [[FINISH]]:
// CHECK: ret void

// Swift calling convention -[ImplClass extensionMethod:]
// CHECK-LABEL: define hidden swiftcc void @"$sSo9ImplClassC19objc_implementationE15extensionMethodyys5Int32VF"

// ObjC calling convention -[ImplClass extensionMethod:]
// CHECK-LABEL: define internal void @"$sSo9ImplClassC19objc_implementationE15extensionMethodyys5Int32VFTo"

// Swift calling convention -[ImplClass copyWithZone:]
// CHECK-LABEL: define hidden swiftcc void @"$sSo9ImplClassC19objc_implementationE4copy4withypSg10ObjectiveC6NSZoneVSg_tF"

// FIXME: Do we actually want a public type metadata accessor? I'm guessing no.
// CHECK-LABEL: define swiftcc %swift.metadata_response @"$sSo9ImplClassCMa"

// ObjC calling convention -[ImplClass copyWithZone:]
// CHECK-LABEL: define internal ptr @"$sSo9ImplClassC19objc_implementationE4copy4withypSg10ObjectiveC6NSZoneVSg_tFTo"

// Swift calling convention -[ImplClass mutableCopyWithZone:]
// CHECK-LABEL: define hidden swiftcc void @"$sSo9ImplClassC19objc_implementationE11mutableCopy4withypSg10ObjectiveC6NSZoneVSg_tF"

// ObjC calling convention -[ImplClass mutableCopyWithZone:]
// CHECK-LABEL: define internal ptr @"$sSo9ImplClassC19objc_implementationE11mutableCopy4withypSg10ObjectiveC6NSZoneVSg_tFTo"

// Swift calling convention -[ImplClass dealloc]
// CHECK-LABEL: define linkonce_odr hidden swiftcc void @"$sSo9ImplClassC19objc_implementationEfD"
  // Access in this function should be DirectToStorage
  // CHECK-NOT: @"\01L_selector(implProperty)"
  // CHECK: @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvpWvd"
  // CHECK-NOT: @"\01L_selector(implProperty)"

// ObjC calling convention -[ImplClass dealloc]
// CHECK-LABEL: define internal void @"$sSo9ImplClassC19objc_implementationEfDTo"

// ObjC calling convention -[ImplClass .cxx_destruct]
// CHECK-LABEL: define hidden void @"$sSo9ImplClassCfETo"
// CHECK: call {{.*}} @"$sSo8NSObjectCSgWOh"
// CHECK: ret

// Swift calling convention -[ImplClass(Category1) category1Method:]
// CHECK-LABEL: define hidden swiftcc void @"$sSo9ImplClassC19objc_implementationE15category1Methodyys5Int32VF"

// ObjC calling convention -[ImplClass(Category1) category1Method:]
// CHECK-LABEL: define internal void @"$sSo9ImplClassC19objc_implementationE15category1Methodyys5Int32VFTo"

// Swift calling convention -[NoInitImplClass init]
// CHECK-LABEL: define swiftcc ptr @"$sSo15NoInitImplClassC19objc_implementationEABycfc"
  // CHECK-DAG:   load i64, ptr @"$sSo15NoInitImplClassC19objc_implementationE2s1SSvpWvd", align 8
  // CHECK-DAG:   load i64, ptr @"$sSo15NoInitImplClassC19objc_implementationE2s2SSvpWvd", align 8
  // CHECK-DAG:   load i64, ptr @"$sSo15NoInitImplClassC19objc_implementationE2s3SSvpWvd", align 8
  // CHECK-DAG:   load i64, ptr @"$sSo15NoInitImplClassC19objc_implementationE2s4SSvpWvd", align 8
  // CHECK:       [[SEL_init:%[^ ]+]] = load ptr, ptr @"\01L_selector(init)", align 8
  // CHECK:       call ptr @objc_msgSendSuper{{.*}} [[SEL_init]])

// ObjC calling convention -[NoInitImplClass init]
// CHECK-LABEL: define internal ptr @"$sSo15NoInitImplClassC19objc_implementationEABycfcTo"

// Swift calling convention SwiftSubclass.mainMethod(_:)
// CHECK-LABEL: define swiftcc void @"$s19objc_implementation13SwiftSubclassC10mainMethodyys5Int32VF"

// ObjC calling convention SwiftSubclass.mainMethod(_:)
// CHECK-LABEL: define internal void @"$s19objc_implementation13SwiftSubclassC10mainMethodyys5Int32VFTo"

// Swift calling convention SwiftSubclass.deinit (deallocating)
// CHECK-LABEL: define swiftcc void @"$s19objc_implementation13SwiftSubclassCfD"

// inplFunc(_:)
// CHECK-LABEL: define void @implFunc
// FIXME: We'd like this to be internal or hidden, not public.
// CHECK: define swiftcc void @"$s19objc_implementation8implFuncyys5Int32VF"

// inplFuncCName(_:)
// CHECK-LABEL: define void @"\01_implFuncAsmName"
// FIXME: We'd like this to be internal or hidden, not public.
// CHECK: define swiftcc void @"$s19objc_implementation13implFuncCNameyys5Int32VF"

// fn(impl:swiftSub:)
// CHECK-LABEL: define swiftcc void @"$s19objc_implementation2fn4impl8swiftSubySo9ImplClassC_AA13SwiftSubclassCtF"
// CHECK:   [[SEL_1:%[^ ]+]] = load ptr, ptr @"\01L_selector(mainMethod:)", align 8
// CHECK:   call void @objc_msgSend(ptr {{.*}}, ptr [[SEL_1]], i32 0)
// CHECK:   [[SEL_2:%[^ ]+]] = load ptr, ptr @"\01L_selector(mainMethod:)", align 8
// CHECK:   call void @objc_msgSend(ptr {{.*}}, ptr [[SEL_2]], i32 1)
// CHECK:   call void @implFunc
// CHECK:   ret void
// CHECK: }

// ImplClassWithResilientStoredProperty ObjC metadata update function
// CHECK-LABEL: define internal ptr @"$sSo36ImplClassWithResilientStoredPropertyCMU"
//
// This function should not invoke the metadata accessor.
// CHECK-NOT:     sSo36ImplClassWithResilientStoredPropertyCMa
//
// This function should directly gather the field sizes and invoke the metadata update.
// CHECK:         %classFields = alloca [4 x ptr]
// CHECK:         [[FIELDS_ARRAY:%[0-9]+]] = getelementptr inbounds [4 x ptr], ptr %classFields, i32 0, i32 0
// CHECK:         store ptr getelementptr inbounds (ptr, ptr @"$sBi32_WV", i32 8), ptr {{%[0-9]+}}
// CHECK:         {{%[0-9]+}} = call swiftcc %swift.metadata_response @"$ss6MirrorVSgMa"(i64 63)
// CHECK:         store ptr {{%[0-9]+}}, ptr {{%[0-9]+}}
// CHECK:         store ptr getelementptr inbounds (ptr, ptr @"$sBi32_WV", i32 8), ptr {{%[0-9]+}}
// CHECK:         store ptr getelementptr inbounds (ptr, ptr @"$sBi32_WV", i32 8), ptr {{%[0-9]+}}
// CHECK:         {{%[0-9]+}} = call swiftcc ptr @swift_updatePureObjCClassMetadata(ptr @"OBJC_CLASS_$_ImplClassWithResilientStoredProperty", i64 256, i64 4, ptr [[FIELDS_ARRAY]])
//
// This function should not invoke the metadata accessor.
// CHECK-NOT:     sSo36ImplClassWithResilientStoredPropertyCMa
//
// CHECK:         ret ptr @"OBJC_CLASS_$_ImplClassWithResilientStoredProperty"
// CHECK:       }

// ImplClassWithResilientStoredProperty type metadata accessor
// CHECK-LABEL: define swiftcc %swift.metadata_response @"$sSo36ImplClassWithResilientStoredPropertyCMa"
//
// This function should not use the runtime call for Swift class metadata
// CHECK-NOT:     swift_getSingletonMetadata
//
// This function should realize the ObjC class and then convert it to metadata.
// CHECK:         {{%[0-9]+}} = call ptr @objc_opt_self(ptr @"OBJC_CLASS_$_ImplClassWithResilientStoredProperty")
// CHECK-NEXT:    {{%[0-9]+}} = call ptr @swift_getObjCClassMetadata(ptr {{%[0-9]+}})
//
// This function should not use the runtime call for Swift class metadata
// CHECK-NOT:     swift_getSingletonMetadata
//
// CHECK:         ret
// CHECK:       }

// ImplClassWithResilientStoredProperty Swift metadata completion function
// This function shouldn't exist or be referenced
// NEGATIVE-NOT: sSo36ImplClassWithResilientStoredPropertyCMr

//
// Not implemented in Swift
// 

// NEGATIVE-NOT: Category2
// NEGATIVE-NOT: NoImplClass
