// Test doesn't pass on all platforms (rdar://101420862)
// REQUIRES: OS=macosx

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %S/Inputs/abi -F %clang-importer-sdk-path/frameworks %s -import-objc-header %S/Inputs/objc_implementation.h -emit-ir > %t.ir
// RUN: %FileCheck --input-file %t.ir %s
// RUN: %FileCheck --input-file %t.ir --check-prefix NEGATIVE %s
// REQUIRES: objc_interop

// CHECK: [[selector_data_mainMethod_:@[^, ]+]] = private global [12 x i8] c"mainMethod:\00", section "__TEXT,__objc_methname,cstring_literals", align 1

//
// @_objcImplementation class
//

// Metaclass
// CHECK: @"OBJC_METACLASS_$_ImplClass" = global %objc_class { %objc_class* @"OBJC_METACLASS_$_NSObject", %objc_class* @"OBJC_METACLASS_$_NSObject", %swift.opaque* @_objc_empty_cache, %swift.opaque* null, {{i64 ptrtoint|%swift.opaque\* bitcast}} ({ i32, i32, i32, i32, i8*, i8*, i8*, { i64, [1 x i8*] }*, i8*, i8*, i8* }* [[_METACLASS_DATA_ImplClass:@[^, ]+]] to {{i64|%swift.opaque\*}}) }, align 8
// CHECK: @_PROTOCOLS_ImplClass = internal constant { i64, [1 x i8*] } { i64 1, [1 x i8*] [i8* bitcast ({ i8*, i8*, i8*, { i32, i32, [2 x { i8*, i8*, i8* }] }*, i8*, i8*, i8*, i8*, i32, i32, [2 x i8*]*, i8*, i8* }* @_PROTOCOL_NSCopying to i8*)] }, section "__DATA, __objc_const", align 8
// CHECK: [[_METACLASS_DATA_ImplClass]] = internal constant { i32, i32, i32, i32, i8*, i8*, i8*, { i64, [1 x i8*] }*, i8*, i8*, i8* } { i32 129, i32 40, i32 40, i32 0, i8* null, i8* getelementptr inbounds ([10 x i8], [10 x i8]* @.str.9.ImplClass, i64 0, i64 0), i8* null, { i64, [1 x i8*] }* @_PROTOCOLS_ImplClass, i8* null, i8* null, i8* null }, section "__DATA, __objc_const", align 8
// TODO: Why the extra i32 field above?

// Class
// CHECK: [[selector_data_implProperty:@[^, ]+]] = private global [13 x i8] c"implProperty\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK: [[selector_data_setImplProperty_:@[^, ]+]] = private global [17 x i8] c"setImplProperty:\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK: [[selector_data__cxx_destruct:@[^, ]+]] = private global [14 x i8] c".cxx_destruct\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK: [[_INSTANCE_METHODS_ImplClass:@[^, ]+]] = internal constant { i32, i32, [7 x { i8*, i8*, i8* }] } { i32 24, i32 7, [7 x { i8*, i8*, i8* }] [{ i8*, i8*, i8* } { i8* getelementptr inbounds ([5 x i8], [5 x i8]* @"\01L_selector_data(init)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.@16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$sSo9ImplClassC19objc_implementationEABycfcTo{{(\.ptrauth)?}}" to i8*) }, { i8*, i8*, i8* } { i8* getelementptr inbounds ([13 x i8], [13 x i8]* [[selector_data_implProperty]], i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.i16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvgTo{{(\.ptrauth)?}}" to i8*) }, { i8*, i8*, i8* } { i8* getelementptr inbounds ([17 x i8], [17 x i8]* [[selector_data_setImplProperty_]], i64 0, i64 0), i8* getelementptr inbounds ([11 x i8], [11 x i8]* @".str.10.v20@0:8i16", i64 0, i64 0), i8* bitcast ({{.*}}* @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvsTo{{(\.ptrauth)?}}" to i8*) }, { i8*, i8*, i8* } { i8* getelementptr inbounds ([12 x i8], [12 x i8]* [[selector_data_mainMethod_]], i64 0, i64 0), i8* getelementptr inbounds ([11 x i8], [11 x i8]* @".str.10.v20@0:8i16", i64 0, i64 0), i8* bitcast ({{.*}}* @"$sSo9ImplClassC19objc_implementationE10mainMethodyys5Int32VFTo{{(\.ptrauth)?}}" to i8*) }, { i8*, i8*, i8* } { i8* getelementptr inbounds ([14 x i8], [14 x i8]* @"\01L_selector_data(copyWithZone:)", i64 0, i64 0), i8* getelementptr inbounds ([12 x i8], [12 x i8]* @".str.11.@24@0:8^v16", i64 0, i64 0), i8* bitcast ({{.*}}* @"$sSo9ImplClassC19objc_implementationE4copy4withyp10ObjectiveC6NSZoneV_tFTo{{(\.ptrauth)?}}" to i8*) }, { i8*, i8*, i8* } { i8* getelementptr inbounds ([8 x i8], [8 x i8]* @"\01L_selector_data(dealloc)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$sSo9ImplClassC19objc_implementationEfDTo{{(\.ptrauth)?}}" to i8*) }, { i8*, i8*, i8* } { i8* getelementptr inbounds ([14 x i8], [14 x i8]* [[selector_data__cxx_destruct]], i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$sSo9ImplClassCfETo{{(\.ptrauth)?}}" to i8*) }] }, section "__DATA, __objc_data", align 8
// CHECK: [[_IVARS_ImplClass:@[^, ]+]] = internal constant { i32, i32, [2 x { i64*, i8*, i8*, i32, i32 }] } { i32 32, i32 2, [2 x { i64*, i8*, i8*, i32, i32 }] [{ i64*, i8*, i8*, i32, i32 } { i64* @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvpWvd", i8* getelementptr inbounds ([13 x i8], [13 x i8]* @.str.12.implProperty, i64 0, i64 0), i8* getelementptr inbounds ([1 x i8], [1 x i8]* @.str.0., i64 0, i64 0), i32 2, i32 4 }, { i64*, i8*, i8*, i32, i32 } { i64* @"$sSo9ImplClassC19objc_implementationE13implProperty2So8NSObjectCSgvpWvd", i8* getelementptr inbounds ([14 x i8], [14 x i8]* @.str.13.implProperty2, i64 0, i64 0), i8* getelementptr inbounds ([1 x i8], [1 x i8]* @.str.0., i64 0, i64 0), i32 3, i32 8 }] }, section "__DATA, __objc_const", align 8
// CHECK: [[_PROPERTIES_ImplClass:@[^, ]+]] = internal constant { i32, i32, [1 x { i8*, i8* }] } { i32 16, i32 1, [1 x { i8*, i8* }] [{ i8*, i8* } { i8* getelementptr inbounds ([13 x i8], [13 x i8]* @.str.12.implProperty, i64 0, i64 0), i8* getelementptr inbounds ([19 x i8], [19 x i8]* @".str.18.Ti,N,VimplProperty", i64 0, i64 0) }] }, section "__DATA, __objc_const", align 8
// FIXME: Should just be @_PROTOCOLS_ImplClass, but an IRGen bug causes the protocol list to be emitted twice.
// CHECK: [[_DATA_ImplClass:@[^, ]+]] = internal constant { i32, i32, i32, i32, i8*, i8*, { i32, i32, [7 x { i8*, i8*, i8* }] }*, { i64, [1 x i8*] }*, { i32, i32, [2 x { i64*, i8*, i8*, i32, i32 }] }*, i8*, { i32, i32, [1 x { i8*, i8* }] }* } { i32 388, i32 8, i32 24, i32 0, i8* null, i8* getelementptr inbounds ([10 x i8], [10 x i8]* @.str.9.ImplClass, i64 0, i64 0), { i32, i32, [7 x { i8*, i8*, i8* }] }* [[_INSTANCE_METHODS_ImplClass]], { i64, [1 x i8*] }* @_PROTOCOLS_ImplClass.{{[0-9]+}}, { i32, i32, [2 x { i64*, i8*, i8*, i32, i32 }] }* [[_IVARS_ImplClass]], i8* null, { i32, i32, [1 x { i8*, i8* }] }* [[_PROPERTIES_ImplClass]] }, section "__DATA, __objc_data", align 8
// CHECK: @"OBJC_CLASS_$_ImplClass" = global <{ i64, %objc_class*, %swift.opaque*, %swift.opaque*, { i32, i32, i32, i32, i8*, i8*, { i32, i32, [7 x { i8*, i8*, i8* }] }*, { i64, [1 x i8*] }*, { i32, i32, [2 x { i64*, i8*, i8*, i32, i32 }] }*, i8*, { i32, i32, [1 x { i8*, i8* }] }* }* }> <{ i64 ptrtoint (%objc_class* @"OBJC_METACLASS_$_ImplClass" to i64), %objc_class* @"OBJC_CLASS_$_NSObject", %swift.opaque* @_objc_empty_cache, %swift.opaque* null, { i32, i32, i32, i32, i8*, i8*, { i32, i32, [7 x { i8*, i8*, i8* }] }*, { i64, [1 x i8*] }*, { i32, i32, [2 x { i64*, i8*, i8*, i32, i32 }] }*, i8*, { i32, i32, [1 x { i8*, i8* }] }* }* [[_DATA_ImplClass]] }>, section "__DATA,__objc_data, regular", align 8

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

  @objc(copyWithZone:)
  func copy(with zone: NSZone) -> Any {
    let copy = ImplClass()
    copy.implProperty = implProperty
    copy.implProperty2 = implProperty2
    return copy
  }
  
  deinit { print(implProperty) }
}

//
// @_objcImplementation category
//

// Category1
// CHECK: [[selector_data_category1Method_:@[^, ]+]] = private global [17 x i8] c"category1Method:\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK: [[_CATEGORY_INSTANCE_METHODS_ImplClass_Category1:@[^, ]+]] = internal constant { i32, i32, [1 x { i8*, i8*, i8* }] } { i32 24, i32 1, [1 x { i8*, i8*, i8* }] [{ i8*, i8*, i8* } { i8* getelementptr inbounds ([17 x i8], [17 x i8]* [[selector_data_category1Method_]], i64 0, i64 0), i8* getelementptr inbounds ([11 x i8], [11 x i8]* @".str.10.v20@0:8i16", i64 0, i64 0), i8* bitcast ({{.*}}* @"$sSo9ImplClassC19objc_implementationE15category1Methodyys5Int32VFTo{{(\.ptrauth)?}}" to i8*) }] }, section "__DATA, __objc_data", align 8
// CHECK: [[_CATEGORY_ImplClass_Category1:@[^, ]+]] = internal constant { i8*, %objc_class*, { i32, i32, [1 x { i8*, i8*, i8* }] }*, i8*, i8*, i8*, i8*, i32 } { i8* getelementptr inbounds ([10 x i8], [10 x i8]* @.str.9.Category1, i64 0, i64 0), %objc_class* bitcast (<{ i64, %objc_class*, %swift.opaque*, %swift.opaque*, { i32, i32, i32, i32, i8*, i8*, { i32, i32, [7 x { i8*, i8*, i8* }] }*, { i64, [1 x i8*] }*, { i32, i32, [2 x { i64*, i8*, i8*, i32, i32 }] }*, i8*, { i32, i32, [1 x { i8*, i8* }] }* }* }>* @"OBJC_CLASS_$_ImplClass" to %objc_class*), { i32, i32, [1 x { i8*, i8*, i8* }] }* [[_CATEGORY_INSTANCE_METHODS_ImplClass_Category1]], i8* null, i8* null, i8* null, i8* null, i32 60 }, section "__DATA, __objc_const", align 8
@_objcImplementation(Category1) extension ImplClass {
  @objc func category1Method(_: Int32) {
    print("category1Method")
  }
}

//
// Second @_objcImplementation class, inherited initializer
//

// CHECK: @"OBJC_METACLASS_$_NoInitImplClass" = global %objc_class { %objc_class* @"OBJC_METACLASS_$_NSObject", %objc_class* @"OBJC_METACLASS_$_NSObject", %swift.opaque* @_objc_empty_cache, %swift.opaque* null, i64 ptrtoint ({ i32, i32, i32, i32, i8*, i8*, i8*, i8*, i8*, i8*, i8* }* @_METACLASS_DATA_NoInitImplClass to i64) }, align 8
// CHECK: @_METACLASS_DATA_NoInitImplClass = internal constant { i32, i32, i32, i32, i8*, i8*, i8*, i8*, i8*, i8*, i8* } { i32 129, i32 40, i32 40, i32 0, i8* null, i8* getelementptr inbounds ([16 x i8], [16 x i8]* @.str.15.NoInitImplClass, i64 0, i64 0), i8* null, i8* null, i8* null, i8* null, i8* null }, section "__DATA, __objc_const", align 8
// CHECK: @_INSTANCE_METHODS_NoInitImplClass = internal constant { i32, i32, [9 x { i8*, i8*, i8* }] } { i32 24, i32 9, [9 x { i8*, i8*, i8* }] [{ i8*, i8*, i8* } { i8* getelementptr inbounds ([3 x i8], [3 x i8]* @"\01L_selector_data(s1)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.@16@0:8", i64 0, i64 0), i8* bitcast (%2* (%3*, i8*)* @"$sSo15NoInitImplClassC19objc_implementationE2s1SSvgTo" to i8*) }, { i8*, i8*, i8* } { i8* getelementptr inbounds ([3 x i8], [3 x i8]* @"\01L_selector_data(s2)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.@16@0:8", i64 0, i64 0), i8* bitcast (%2* (%3*, i8*)* @"$sSo15NoInitImplClassC19objc_implementationE2s2SSvgTo" to i8*) }, { i8*, i8*, i8* } { i8* getelementptr inbounds ([7 x i8], [7 x i8]* @"\01L_selector_data(setS2:)", i64 0, i64 0), i8* getelementptr inbounds ([11 x i8], [11 x i8]* @".str.10.v24@0:8@16", i64 0, i64 0), i8* bitcast (void (%3*, i8*, %2*)* @"$sSo15NoInitImplClassC19objc_implementationE2s2SSvsTo" to i8*) }, { i8*, i8*, i8* } { i8* getelementptr inbounds ([3 x i8], [3 x i8]* @"\01L_selector_data(s3)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.@16@0:8", i64 0, i64 0), i8* bitcast (%2* (%3*, i8*)* @"$sSo15NoInitImplClassC19objc_implementationE2s3SSvgTo" to i8*) }, { i8*, i8*, i8* } { i8* getelementptr inbounds ([3 x i8], [3 x i8]* @"\01L_selector_data(s4)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.@16@0:8", i64 0, i64 0), i8* bitcast (%2* (%3*, i8*)* @"$sSo15NoInitImplClassC19objc_implementationE2s4SSvgTo" to i8*) }, { i8*, i8*, i8* } { i8* getelementptr inbounds ([7 x i8], [7 x i8]* @"\01L_selector_data(setS4:)", i64 0, i64 0), i8* getelementptr inbounds ([11 x i8], [11 x i8]* @".str.10.v24@0:8@16", i64 0, i64 0), i8* bitcast (void (%3*, i8*, %2*)* @"$sSo15NoInitImplClassC19objc_implementationE2s4SSvsTo" to i8*) }, { i8*, i8*, i8* } { i8* getelementptr inbounds ([8 x i8], [8 x i8]* @"\01L_selector_data(dealloc)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast (void (%3*, i8*)* @"$sSo15NoInitImplClassC19objc_implementationEfDTo" to i8*) }, { i8*, i8*, i8* } { i8* getelementptr inbounds ([5 x i8], [5 x i8]* @"\01L_selector_data(init)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.@16@0:8", i64 0, i64 0), i8* bitcast (%3* (%3*, i8*)* @"$sSo15NoInitImplClassC19objc_implementationEABycfcTo" to i8*) }, { i8*, i8*, i8* } { i8* getelementptr inbounds ([14 x i8], [14 x i8]* @"\01L_selector_data(.cxx_destruct)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast (void (%3*, i8*)* @"$sSo15NoInitImplClassCfETo" to i8*) }] }, section "__DATA, __objc_data", align 8
// CHECK: @_IVARS_NoInitImplClass = internal constant { i32, i32, [4 x { i64*, i8*, i8*, i32, i32 }] } { i32 32, i32 4, [4 x { i64*, i8*, i8*, i32, i32 }] [{ i64*, i8*, i8*, i32, i32 } { i64* @"$sSo15NoInitImplClassC19objc_implementationE2s1SSvpWvd", i8* getelementptr inbounds ([3 x i8], [3 x i8]* @.str.2.s1, i64 0, i64 0), i8* getelementptr inbounds ([1 x i8], [1 x i8]* @.str.0., i64 0, i64 0), i32 3, i32 16 }, { i64*, i8*, i8*, i32, i32 } { i64* @"$sSo15NoInitImplClassC19objc_implementationE2s2SSvpWvd", i8* getelementptr inbounds ([3 x i8], [3 x i8]* @.str.2.s2, i64 0, i64 0), i8* getelementptr inbounds ([1 x i8], [1 x i8]* @.str.0., i64 0, i64 0), i32 3, i32 16 }, { i64*, i8*, i8*, i32, i32 } { i64* @"$sSo15NoInitImplClassC19objc_implementationE2s3SSvpWvd", i8* getelementptr inbounds ([3 x i8], [3 x i8]* @.str.2.s3, i64 0, i64 0), i8* getelementptr inbounds ([1 x i8], [1 x i8]* @.str.0., i64 0, i64 0), i32 3, i32 16 }, { i64*, i8*, i8*, i32, i32 } { i64* @"$sSo15NoInitImplClassC19objc_implementationE2s4SSvpWvd", i8* getelementptr inbounds ([3 x i8], [3 x i8]* @.str.2.s4, i64 0, i64 0), i8* getelementptr inbounds ([1 x i8], [1 x i8]* @.str.0., i64 0, i64 0), i32 3, i32 16 }] }, section "__DATA, __objc_const", align 8
// CHECK: @_PROPERTIES_NoInitImplClass = internal constant { i32, i32, [4 x { i8*, i8* }] } { i32 16, i32 4, [4 x { i8*, i8* }] [{ i8*, i8* } { i8* getelementptr inbounds ([3 x i8], [3 x i8]* @.str.2.s1, i64 0, i64 0), i8* getelementptr inbounds ([17 x i8], [17 x i8]* @".str.16.T@\22NSString\22,N,R", i64 0, i64 0) }, { i8*, i8* } { i8* getelementptr inbounds ([3 x i8], [3 x i8]* @.str.2.s2, i64 0, i64 0), i8* getelementptr inbounds ([17 x i8], [17 x i8]* @".str.16.T@\22NSString\22,N,C", i64 0, i64 0) }, { i8*, i8* } { i8* getelementptr inbounds ([3 x i8], [3 x i8]* @.str.2.s3, i64 0, i64 0), i8* getelementptr inbounds ([17 x i8], [17 x i8]* @".str.16.T@\22NSString\22,N,R", i64 0, i64 0) }, { i8*, i8* } { i8* getelementptr inbounds ([3 x i8], [3 x i8]* @.str.2.s4, i64 0, i64 0), i8* getelementptr inbounds ([17 x i8], [17 x i8]* @".str.16.T@\22NSString\22,N,C", i64 0, i64 0) }] }, section "__DATA, __objc_const", align 8
// CHECK: @_DATA_NoInitImplClass = internal constant { i32, i32, i32, i32, i8*, i8*, { i32, i32, [9 x { i8*, i8*, i8* }] }*, i8*, { i32, i32, [4 x { i64*, i8*, i8*, i32, i32 }] }*, i8*, { i32, i32, [4 x { i8*, i8* }] }* } { i32 388, i32 8, i32 72, i32 0, i8* null, i8* getelementptr inbounds ([16 x i8], [16 x i8]* @.str.15.NoInitImplClass, i64 0, i64 0), { i32, i32, [9 x { i8*, i8*, i8* }] }* @_INSTANCE_METHODS_NoInitImplClass, i8* null, { i32, i32, [4 x { i64*, i8*, i8*, i32, i32 }] }* @_IVARS_NoInitImplClass, i8* null, { i32, i32, [4 x { i8*, i8* }] }* @_PROPERTIES_NoInitImplClass }, section "__DATA, __objc_data", align 8
// CHECK: @"OBJC_CLASS_$_NoInitImplClass" = global <{ i64, %objc_class*, %swift.opaque*, %swift.opaque*, { i32, i32, i32, i32, i8*, i8*, { i32, i32, [9 x { i8*, i8*, i8* }] }*, i8*, { i32, i32, [4 x { i64*, i8*, i8*, i32, i32 }] }*, i8*, { i32, i32, [4 x { i8*, i8* }] }* }* }> <{ i64 ptrtoint (%objc_class* @"OBJC_METACLASS_$_NoInitImplClass" to i64), %objc_class* @"OBJC_CLASS_$_NSObject", %swift.opaque* @_objc_empty_cache, %swift.opaque* null, { i32, i32, i32, i32, i8*, i8*, { i32, i32, [9 x { i8*, i8*, i8* }] }*, i8*, { i32, i32, [4 x { i64*, i8*, i8*, i32, i32 }] }*, i8*, { i32, i32, [4 x { i8*, i8* }] }* }* @_DATA_NoInitImplClass }>, section "__DATA,__objc_data, regular", align 8
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
// CHECK: @"OBJC_METACLASS_$__TtC19objc_implementation13SwiftSubclass" = global %objc_class { %objc_class* @"OBJC_METACLASS_$_NSObject", %objc_class* @"OBJC_METACLASS_$_ImplClass", %swift.opaque* @_objc_empty_cache, %swift.opaque* null, {{i64 ptrtoint|%swift.opaque\* bitcast}} ({ i32, i32, i32, i32, i8*, i8*, i8*, i8*, i8*, i8*, i8* }* [[_METACLASS_DATA_SwiftSubclass:@[^, ]+]] to {{i64|%swift.opaque\*}}) }, align 8
// CHECK: [[_METACLASS_DATA_SwiftSubclass]] = internal constant { i32, i32, i32, i32, i8*, i8*, i8*, i8*, i8*, i8*, i8* } { i32 129, i32 40, i32 40, i32 0, i8* null, i8* getelementptr inbounds ([41 x i8], [41 x i8]* @.str.40._TtC19objc_implementation13SwiftSubclass, i64 0, i64 0), i8* null, i8* null, i8* null, i8* null, i8* null }, section "__DATA, __objc_const", align 8

// Class
// CHECK: [[_INSTANCE_METHODS_SwiftSubclass:@[^, ]+]] = internal constant { i32, i32, [2 x { i8*, i8*, i8* }] } { i32 24, i32 2, [2 x { i8*, i8*, i8* }] [{ i8*, i8*, i8* } { i8* getelementptr inbounds ([12 x i8], [12 x i8]* [[selector_data_mainMethod_]], i64 0, i64 0), i8* getelementptr inbounds ([11 x i8], [11 x i8]* @".str.10.v20@0:8i16", i64 0, i64 0), i8* bitcast ({{.*}}* @"$s19objc_implementation13SwiftSubclassC10mainMethodyys5Int32VFTo{{(\.ptrauth)?}}" to i8*) }, { i8*, i8*, i8* } { i8* getelementptr inbounds ([5 x i8], [5 x i8]* @"\01L_selector_data(init)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.@16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$s19objc_implementation13SwiftSubclassCACycfcTo{{(\.ptrauth)?}}" to i8*) }] }, section "__DATA, __objc_data", align 8
// CHECK: [[_DATA_SwiftSubclass:@[^, ]+]] = internal constant { i32, i32, i32, i32, i8*, i8*, { i32, i32, [2 x { i8*, i8*, i8* }] }*, i8*, i8*, i8*, i8* } { i32 128, i32 24, i32 24, i32 0, i8* null, i8* getelementptr inbounds ([41 x i8], [41 x i8]* @.str.40._TtC19objc_implementation13SwiftSubclass, i64 0, i64 0), { i32, i32, [2 x { i8*, i8*, i8* }] }* [[_INSTANCE_METHODS_SwiftSubclass]], i8* null, i8* null, i8* null, i8* null }, section "__DATA, __objc_data", align 8

// Swift metadata
// CHECK: @"$s19objc_implementationMXM" = linkonce_odr hidden constant <{ i32, i32, i32 }> <{ i32 0, i32 0, i32 trunc (i64 sub (i64 ptrtoint ([20 x i8]* @.str.19.objc_implementation to i64), i64 ptrtoint (i32* getelementptr inbounds (<{ i32, i32, i32 }>, <{ i32, i32, i32 }>* @"$s19objc_implementationMXM", i32 0, i32 2) to i64)) to i32) }>, section "__TEXT,__constg_swiftt", align 4
// CHECK: @"symbolic So9ImplClassC" = linkonce_odr hidden constant <{ [13 x i8], i8 }> <{ [13 x i8] c"So9ImplClassC", i8 0 }>, section "__TEXT,__swift5_typeref, regular"{{.*}}, align 2
// CHECK: @"$s19objc_implementation13SwiftSubclassCMn" = constant <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }> <{ i32 80, i32 trunc (i64 sub (i64 ptrtoint (<{ i32, i32, i32 }>* @"$s19objc_implementationMXM" to i64), i64 ptrtoint (i32* getelementptr inbounds (<{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>, <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>* @"$s19objc_implementation13SwiftSubclassCMn", i32 0, i32 1) to i64)) to i32), i32 trunc (i64 sub (i64 ptrtoint ([14 x i8]* @.str.13.SwiftSubclass to i64), i64 ptrtoint (i32* getelementptr inbounds (<{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>, <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>* @"$s19objc_implementation13SwiftSubclassCMn", i32 0, i32 2) to i64)) to i32), i32 trunc (i64 sub (i64 ptrtoint (%swift.metadata_response (i64)* @"$s19objc_implementation13SwiftSubclassCMa" to i64), i64 ptrtoint (i32* getelementptr inbounds (<{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>, <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>* @"$s19objc_implementation13SwiftSubclassCMn", i32 0, i32 3) to i64)) to i32), i32 trunc (i64 sub (i64 ptrtoint ({ i32, i32, i16, i16, i32 }* @"$s19objc_implementation13SwiftSubclassCMF" to i64), i64 ptrtoint (i32* getelementptr inbounds (<{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>, <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>* @"$s19objc_implementation13SwiftSubclassCMn", i32 0, i32 4) to i64)) to i32), i32 trunc (i64 sub (i64 ptrtoint (<{ [13 x i8], i8 }>* @"symbolic So9ImplClassC" to i64), i64 ptrtoint (i32* getelementptr inbounds (<{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>, <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>* @"$s19objc_implementation13SwiftSubclassCMn", i32 0, i32 5) to i64)) to i32), i32 3, i32 10, i32 0, i32 0, i32 10 }>, section "__TEXT,__constg_swiftt", align 4
// CHECK: @"$s19objc_implementation13SwiftSubclassCMf" = internal global <{ i8*, void (%T19objc_implementation13SwiftSubclassC*)*, i8**, i64, %objc_class*, %swift.opaque*, %swift.opaque*, {{i64|%swift.opaque\*}}, i32, i32, i32, i16, i16, i32, i32, <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>*, i8* }> <{ i8* null, {{.*}}* @"$s19objc_implementation13SwiftSubclassCfD{{(\.ptrauth)?}}"{{( to void \(.*\)\*\))?}}, i8** @"$sBOWV", i64 ptrtoint (%objc_class* @"OBJC_METACLASS_$__TtC19objc_implementation13SwiftSubclass" to i64), %objc_class* bitcast (<{ i64, %objc_class*, %swift.opaque*, %swift.opaque*, { i32, i32, i32, i32, i8*, i8*, { i32, i32, [7 x { i8*, i8*, i8* }] }*, { i64, [1 x i8*] }*, { i32, i32, [2 x { i64*, i8*, i8*, i32, i32 }] }*, i8*, { i32, i32, [1 x { i8*, i8* }] }* }* }>* @"OBJC_CLASS_$_ImplClass" to %objc_class*), %swift.opaque* @_objc_empty_cache, %swift.opaque* null, {{i64 add \(i64 ptrtoint|%swift.opaque\* bitcast \(i8\* getelementptr \(i8, i8\* bitcast}} ({ i32, i32, i32, i32, i8*, i8*, { i32, i32, [2 x { i8*, i8*, i8* }] }*, i8*, i8*, i8*, i8* }* @_DATA__TtC19objc_implementation13SwiftSubclass to {{i64\), i64 1|i8\*\), i64 1\) to %swift.opaque\*}}), i32 0, i32 0, i32 24, i16 7, i16 0, i32 104, i32 24, <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>* {{(bitcast \(\{ i8\*, i32, i64, i64 \}\* )?}}@"$s19objc_implementation13SwiftSubclassCMn{{(\.ptrauth)?}}"{{( to <\{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 \}>\*\))?}}, i8* null }>, section "__DATA,__objc_data, regular", align 8
// CHECK: @"symbolic _____ 19objc_implementation13SwiftSubclassC" = linkonce_odr hidden constant <{ i8, i32, i8 }> <{ i8 1, i32 trunc (i64 sub (i64 ptrtoint (<{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>* @"$s19objc_implementation13SwiftSubclassCMn" to i64), i64 ptrtoint (i32* getelementptr inbounds (<{ i8, i32, i8 }>, <{ i8, i32, i8 }>* @"symbolic _____ 19objc_implementation13SwiftSubclassC", i32 0, i32 1) to i64)) to i32), i8 0 }>, section "__TEXT,__swift5_typeref, regular"{{.*}}, align 2
// CHECK: @"$s19objc_implementation13SwiftSubclassCMF" = internal constant { i32, i32, i16, i16, i32 } { i32 trunc (i64 sub (i64 ptrtoint (<{ i8, i32, i8 }>* @"symbolic _____ 19objc_implementation13SwiftSubclassC" to i64), i64 ptrtoint ({ i32, i32, i16, i16, i32 }* @"$s19objc_implementation13SwiftSubclassCMF" to i64)) to i32), i32 trunc (i64 sub (i64 ptrtoint (<{ [13 x i8], i8 }>* @"symbolic So9ImplClassC" to i64), i64 ptrtoint (i32* getelementptr inbounds ({ i32, i32, i16, i16, i32 }, { i32, i32, i16, i16, i32 }* @"$s19objc_implementation13SwiftSubclassCMF", i32 0, i32 1) to i64)) to i32), i16 7, i16 12, i32 0 }, section "__TEXT,__swift5_fieldmd, regular"{{.*}}, align 4
open class SwiftSubclass: ImplClass {
  override open func mainMethod(_: Int32) {
    print("subclass mainMethod")
  }
}

//
// Epilogue
//

// CHECK: @"objc_classes_OBJC_CLASS_$_ImplClass" = internal global i8* bitcast (<{ i64, %objc_class*, %swift.opaque*, %swift.opaque*, { i32, i32, i32, i32, i8*, i8*, { i32, i32, [7 x { i8*, i8*, i8* }] }*, { i64, [1 x i8*] }*, { i32, i32, [2 x { i64*, i8*, i8*, i32, i32 }] }*, i8*, { i32, i32, [1 x { i8*, i8* }] }* }* }>* @"OBJC_CLASS_$_ImplClass" to i8*), section "__DATA,__objc_classlist,regular,no_dead_strip"{{.*}}, align 8
// CHECK: @"objc_classes_$s19objc_implementation13SwiftSubclassCN" = internal global i8* bitcast (%swift.type* @"$s19objc_implementation13SwiftSubclassCN" to i8*), section "__DATA,__objc_classlist,regular,no_dead_strip"{{.*}}, align 8
// CHECK: @objc_categories = internal global [1 x i8*] [i8* bitcast ({ i8*, %objc_class*, { i32, i32, [1 x { i8*, i8*, i8* }] }*, i8*, i8*, i8*, i8*, i32 }* [[_CATEGORY_ImplClass_Category1]] to i8*)], section "__DATA,__objc_catlist,regular,no_dead_strip"{{.*}}, align 8

// CHECK: @"$s19objc_implementation13SwiftSubclassCN" = alias %swift.type, bitcast (i64* getelementptr inbounds (<{ i8*, void (%T19objc_implementation13SwiftSubclassC*)*, i8**, i64, %objc_class*, %swift.opaque*, %swift.opaque*, {{i64|%swift.opaque\*}}, i32, i32, i32, i16, i16, i32, i32, <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>*, i8* }>, <{ i8*, void (%T19objc_implementation13SwiftSubclassC*)*, i8**, i64, %objc_class*, %swift.opaque*, %swift.opaque*, {{i64|%swift.opaque\*}}, i32, i32, i32, i16, i16, i32, i32, <{ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }>*, i8* }>* @"$s19objc_implementation13SwiftSubclassCMf", i32 0, i32 3) to %swift.type*)
// CHECK: @"OBJC_CLASS_$__TtC19objc_implementation13SwiftSubclass" = alias %swift.type, %swift.type* @"$s19objc_implementation13SwiftSubclassCN"

//
// Functions
//

public func fn(impl: ImplClass, swiftSub: SwiftSubclass) {
  impl.mainMethod(0)
  swiftSub.mainMethod(1)
}

// Swift calling convention -[ImplClass init]
// CHECK-LABEL: define hidden swiftcc %TSo9ImplClassC* @"$sSo9ImplClassC19objc_implementationEABycfc"
  // Access in this function should be DirectToStorage
  // CHECK-NOT: @"\01L_selector(implProperty)"
  // CHECK: @"$sSo9ImplClassC19objc_implementationE12implPropertys5Int32VvpWvd"
  // CHECK-NOT: @"\01L_selector(implProperty)"

// ObjC calling convention -[ImplClass init]
// CHECK-LABEL: define internal {{%.*}}* @"$sSo9ImplClassC19objc_implementationEABycfcTo"

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

// Swift calling convention -[ImplClass copyWithZone:]
// CHECK-LABEL: define hidden swiftcc void @"$sSo9ImplClassC19objc_implementationE4copy4withyp10ObjectiveC6NSZoneV_tF"

// ObjC calling convention -[ImplClass copyWithZone:]
// CHECK-LABEL: define internal i8* @"$sSo9ImplClassC19objc_implementationE4copy4withyp10ObjectiveC6NSZoneV_tFTo"

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
// CHECK: call {{.*}} @"$sSo8NSObjectCSgWOh"({{.*}} %.implProperty2)
// CHECK: ret

// Swift calling convention -[ImplClass(Category1) category1Method:]
// CHECK-LABEL: define hidden swiftcc void @"$sSo9ImplClassC19objc_implementationE15category1Methodyys5Int32VF"

// ObjC calling convention -[ImplClass(Category1) category1Method:]
// CHECK-LABEL: define internal void @"$sSo9ImplClassC19objc_implementationE15category1Methodyys5Int32VFTo"

// Swift calling convention -[NoInitImplClass init]
// CHECK-LABEL: define swiftcc %TSo15NoInitImplClassC* @"$sSo15NoInitImplClassC19objc_implementationEABycfc"
  // CHECK-DAG:   load i64, i64* @"$sSo15NoInitImplClassC19objc_implementationE2s1SSvpWvd", align 8
  // CHECK-DAG:   load i64, i64* @"$sSo15NoInitImplClassC19objc_implementationE2s2SSvpWvd", align 8
  // CHECK-DAG:   load i64, i64* @"$sSo15NoInitImplClassC19objc_implementationE2s3SSvpWvd", align 8
  // CHECK-DAG:   load i64, i64* @"$sSo15NoInitImplClassC19objc_implementationE2s4SSvpWvd", align 8
  // CHECK:       [[SEL_init:%[^ ]+]] = load i8*, i8** @"\01L_selector(init)", align 8
  // CHECK:       call {{%.*}}* bitcast (void ()* @objc_msgSendSuper{{.*}} [[SEL_init]])

// ObjC calling convention -[NoInitImplClass init]
// CHECK-LABEL: define internal %3* @"$sSo15NoInitImplClassC19objc_implementationEABycfcTo"

// Swift calling convention SwiftSubclass.mainMethod(_:)
// CHECK-LABEL: define swiftcc void @"$s19objc_implementation13SwiftSubclassC10mainMethodyys5Int32VF"

// ObjC calling convention SwiftSubclass.mainMethod(_:)
// CHECK-LABEL: define internal void @"$s19objc_implementation13SwiftSubclassC10mainMethodyys5Int32VFTo"

// Swift calling convention SwiftSubclass.deinit (deallocating)
// CHECK-LABEL: define swiftcc void @"$s19objc_implementation13SwiftSubclassCfD"

// fn(impl:swiftSub:)
// CHECK-LABEL: define swiftcc void @"$s19objc_implementation2fn4impl8swiftSubySo9ImplClassC_AA13SwiftSubclassCtF"
// CHECK:   [[SEL_1:%[^ ]+]] = load i8*, i8** @"\01L_selector(mainMethod:)", align 8
// CHECK:   [[PARAM_impl:%[^ ]+]] = bitcast %TSo9ImplClassC* {{%.*}} to {{%.*}}*
// CHECK:   call void bitcast (void ()* @objc_msgSend to void ({{%.*}}*, i8*, i32)*)({{%.*}}* [[PARAM_impl]], i8* [[SEL_1]], i32 0)
// CHECK:   [[SEL_2:%[^ ]+]] = load i8*, i8** @"\01L_selector(mainMethod:)", align 8
// CHECK:   [[PARAM_swiftSub:%[^ ]+]] = bitcast %T19objc_implementation13SwiftSubclassC* {{%.*}} to [[SEL_1]]*
// CHECK:   call void bitcast (void ()* @objc_msgSend to void ([[SEL_1]]*, i8*, i32)*)([[SEL_1]]* [[PARAM_swiftSub]], i8* [[SEL_2]], i32 1)
// CHECK:   ret void
// CHECK: }

// FIXME: Do we actually want a public type metadata accessor? I'm guessing no.
// CHECK-LABEL: define swiftcc %swift.metadata_response @"$sSo9ImplClassCMa"

//
// Not implemented in Swift
// 

// NEGATIVE-NOT: Category2
// NEGATIVE-NOT: NoImplClass
