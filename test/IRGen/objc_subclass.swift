// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) %use_no_opaque_pointers -primary-file %s -emit-ir | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize %s
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -primary-file %s -emit-ir

// REQUIRES: objc_interop

// The order of the output seems to change between asserts/noasserts build of
// the stlib.
// REQUIRES: swift_stdlib_asserts

// CHECK: [[SGIZMO:T13objc_subclass10SwiftGizmoC]] = type
// CHECK: [[OBJC_CLASS:%objc_class]] = type
// CHECK: [[OPAQUE:%swift.opaque]] = type
// CHECK: [[INT:%TSi]] = type <{ [[LLVM_PTRSIZE_INT:i(32|64)]] }>
// CHECK: [[TYPE:%swift.type]] = type
// CHECK-DAG: [[GIZMO:%TSo5GizmoC]] = type <{ %objc_class* }>
// CHECK-DAG: [[OBJC:%objc_object]] = type opaque


// CHECK-32: @"$s13objc_subclass10SwiftGizmoC1xSivpWvd" = hidden global i32 4, align [[WORD_SIZE_IN_BYTES:4]]
// CHECK-64: @"$s13objc_subclass10SwiftGizmoC1xSivpWvd" = hidden global i64 8, align [[WORD_SIZE_IN_BYTES:8]]

// CHECK: @"OBJC_METACLASS_$__TtC13objc_subclass10SwiftGizmo" = hidden global [[OBJC_CLASS]] { [[OBJC_CLASS]]* @"OBJC_METACLASS_$_NSObject", [[OBJC_CLASS]]* @"OBJC_METACLASS_$_Gizmo", [[OPAQUE]]* @_objc_empty_cache, [[OPAQUE]]* null, [[LLVM_PTRSIZE_INT]] ptrtoint ({{.*}} @_METACLASS_DATA__TtC13objc_subclass10SwiftGizmo to [[LLVM_PTRSIZE_INT]]) }

// CHECK-32: @_METACLASS_DATA__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}* } {
// CHECK-32:   i32 129,
// CHECK-32:   i32 20,
// CHECK-32:   i32 20,
// CHECK-32:   i8* null,
// CHECK-32:   i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @.str.31._TtC13objc_subclass10SwiftGizmo, i32 0, i32 0),
// CHECK-32:   i8* null,
// CHECK-32:   i8* null,
// CHECK-32:   i8* null,
// CHECK-32:   i8* null,
// CHECK-32:   i8* null
// CHECK-32: }, section "__DATA, {{.*}}", align 4

// CHECK-64: @_METACLASS_DATA__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}* } {
// CHECK-64:   i32 129,
// CHECK-64:   i32 40,
// CHECK-64:   i32 40,
// CHECK-64:   i32 0,
// CHECK-64:   i8* null,
// CHECK-64:   i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @.str.31._TtC13objc_subclass10SwiftGizmo, i64 0, i64 0),
// CHECK-64:   i8* null,
// CHECK-64:   i8* null,
// CHECK-64:   i8* null,
// CHECK-64:   i8* null,
// CHECK-64:   i8* null
// CHECK-64: }, section "__DATA, {{.*}}", align 8

// CHECK-32: @_INSTANCE_METHODS__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}] } {
// CHECK-32:   i32 12,
// CHECK-32:   i32 11,
// CHECK-32:   [11 x { i8*, i8*, i8* }] [{ i8*, i8*, i8* } {
// CHECK-32:     i8* getelementptr inbounds ([2 x i8], [2 x i8]* @"\01L_selector_data(x)", i32 0, i32 0),
// CHECK-32:     i8* getelementptr inbounds ([7 x i8], [7 x i8]* @".str.6.l8@0:4", i32 0, i32 0),
// CHECK-32:     i8* bitcast {{.*}}* @"$s13objc_subclass10SwiftGizmoC1xSivgTo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:   }, { i8*, i8*, i8* } {
// CHECK-32:     i8* getelementptr inbounds ([6 x i8], [6 x i8]* @"\01L_selector_data(setX:)", i32 0, i32 0),
// CHECK-32:     i8* getelementptr inbounds ([10 x i8], [10 x i8]* @".str.9.v12@0:4l8", i32 0, i32 0),
// CHECK-32:     i8* bitcast {{.*}}* @"$s13objc_subclass10SwiftGizmoC1xSivsTo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:   }, { i8*, i8*, i8* } {
// CHECK-32:     i8* getelementptr inbounds ([5 x i8], [5 x i8]* @"\01L_selector_data(getX)", i32 0, i32 0),
// CHECK-32:     i8* getelementptr inbounds ([7 x i8], [7 x i8]* @".str.6.l8@0:4", i32 0, i32 0),
// CHECK-32:     i8* bitcast {{.*}}* @"$s13objc_subclass10SwiftGizmoC4getXSiyFTo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:   }, { i8*, i8*, i8* } {
// CHECK-32:     i8* getelementptr inbounds ([10 x i8], [10 x i8]* @"\01L_selector_data(duplicate)", i32 0, i32 0),
// CHECK-32:     i8* getelementptr inbounds ([7 x i8], [7 x i8]* @".str.6.@8@0:4", i32 0, i32 0),
// CHECK-32:     i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoC9duplicateSo0D0CyFTo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:   }, { i8*, i8*, i8* } {
// CHECK-32:     i8* getelementptr inbounds ([5 x i8], [5 x i8]* @"\01L_selector_data(init)", i32 0, i32 0),
// CHECK-32:     i8* getelementptr inbounds ([7 x i8], [7 x i8]* @".str.6.@8@0:4", i32 0, i32 0),
// CHECK-32:     i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoCACycfcTo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:   }, { i8*, i8*, i8* } {
// CHECK-32:     i8* getelementptr inbounds ([20 x i8], [20 x i8]* @"\01L_selector_data(initWithInt:string:)", i32 0, i32 0),
// CHECK-32:     i8* getelementptr inbounds ([13 x i8], [13 x i8]* @".str.12.@16@0:4l8@12", i32 0, i32 0),
// CHECK-32:     i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoC3int6stringACSi_SStcfcTo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:   }, { i8*, i8*, i8* } {
// CHECK-32:     i8* getelementptr inbounds ([8 x i8], [8 x i8]* @"\01L_selector_data(dealloc)", i32 0, i32 0),
// CHECK-32:     i8* getelementptr inbounds ([7 x i8], [7 x i8]* @".str.6.v8@0:4", i32 0, i32 0),
// CHECK-32:     i8* bitcast {{.*}}* @"$s13objc_subclass10SwiftGizmoCfDTo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:   }, { i8*, i8*, i8* } {
// CHECK-32:     i8* getelementptr inbounds ([10 x i8], [10 x i8]* @"\01L_selector_data(isEnabled)", i32 0, i32 0),
// CHECK-32:     i8* getelementptr inbounds ([7 x i8], [7 x i8]* {{@".str.[^"]+"}}, i32 0, i32 0),
// CHECK-32:     i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoC7enabledSbvgTo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:   }, { i8*, i8*, i8* } {
// CHECK-32:     i8* getelementptr inbounds ([14 x i8], [14 x i8]* @"\01L_selector_data(setIsEnabled:)", i32 0, i32 0),
// CHECK-32:     i8* getelementptr inbounds ([10 x i8], [10 x i8]* {{@".str.[^"]+"}}, i32 0, i32 0),
// CHECK-32:     i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoC7enabledSbvsTo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:   }, { i8*, i8*, i8* } {
// CHECK-32:     i8* getelementptr inbounds ([17 x i8], [17 x i8]* @"\01L_selector_data(initWithBellsOn:)", i32 0, i32 0),
// CHECK-32:     i8* getelementptr inbounds ([10 x i8], [10 x i8]* {{@".str.[^"]+"}}, i32 0, i32 0),
// CHECK-32:     i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoC7bellsOnACSgSi_tcfcTo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:   }, { i8*, i8*, i8* } {
// CHECK-32:     i8* getelementptr inbounds ([15 x i8], [15 x i8]* @"\01L_selector_data(.cxx_construct)", i32 0, i32 0),
// CHECK-32:     i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* {{@".str.[^"]+"}}, i32 0, i32 0),
// CHECK-32:     i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoCfeTo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:   }]
// CHECK-32: }, section "__DATA, {{.*}}", align 4

// CHECK-64: @_INSTANCE_METHODS__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}] } {
// CHECK-64:   i32 24,
// CHECK-64:   i32 11,
// CHECK-64:   [11 x { i8*, i8*, i8* }] [{
// CHECK-64:      i8* getelementptr inbounds ([2 x i8], [2 x i8]* @"\01L_selector_data(x)", i64 0, i64 0),
// CHECK-64:      i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.q16@0:8", i64 0, i64 0)
// CHECK-64:      i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoC1xSivgTo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:   }, {
// CHECK-64:      i8* getelementptr inbounds ([6 x i8], [6 x i8]* @"\01L_selector_data(setX:)", i64 0, i64 0),
// CHECK-64:      i8* getelementptr inbounds ([11 x i8], [11 x i8]* @".str.10.v24@0:8q16", i64 0, i64 0)
// CHECK-64:      i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoC1xSivsTo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:   }, {
// CHECK-64:      i8* getelementptr inbounds ([5 x i8], [5 x i8]* @"\01L_selector_data(getX)", i64 0, i64 0),
// CHECK-64:      i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.q16@0:8", i64 0, i64 0)
// CHECK-64:      i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoC4getXSiyFTo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:   }, {
// CHECK-64:     i8* getelementptr inbounds ([10 x i8], [10 x i8]* @"\01L_selector_data(duplicate)", i64 0, i64 0),
// CHECK-64:     i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.@16@0:8", i64 0, i64 0),
// CHECK-64:     i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoC9duplicateSo0D0CyFTo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:   }, {
// CHECK-64:     i8* getelementptr inbounds ([5 x i8], [5 x i8]* @"\01L_selector_data(init)", i64 0, i64 0),
// CHECK-64:     i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.@16@0:8", i64 0, i64 0),
// CHECK-64:     i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoCACycfcTo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:   }, {
// CHECK-64:     i8* getelementptr inbounds ([20 x i8], [20 x i8]* @"\01L_selector_data(initWithInt:string:)", i64 0, i64 0),
// CHECK-64:     i8* getelementptr inbounds ([14 x i8], [14 x i8]* @".str.13.@32@0:8q16@24", i64 0, i64 0),
// CHECK-64:     i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoC3int6stringACSi_SStcfcTo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:   }, {
// CHECK-64:     i8* getelementptr inbounds ([8 x i8], [8 x i8]* @"\01L_selector_data(dealloc)", i64 0, i64 0),
// CHECK-64:     i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0),
// CHECK-64:     i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoCfDTo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:   }, {
// CHECK-64:     i8* getelementptr inbounds ([10 x i8], [10 x i8]* @"\01L_selector_data(isEnabled)", i64 0, i64 0),
// CHECK-64:     i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.{{[Bc]}}16@0:8", i64 0, i64 0),
// CHECK-64:     i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoC7enabledSbvgTo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:   }, {
// CHECK-64:     i8* getelementptr inbounds ([14 x i8], [14 x i8]* @"\01L_selector_data(setIsEnabled:)", i64 0, i64 0),
// CHECK-64:     i8* getelementptr inbounds ([11 x i8], [11 x i8]* @".str.10.v20@0:8{{[Bc]}}16", i64 0, i64 0),
// CHECK-64:     i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoC7enabledSbvsTo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:   }, {
// CHECK-64:     i8* getelementptr inbounds ([17 x i8], [17 x i8]* @"\01L_selector_data(initWithBellsOn:)", i64 0, i64 0),
// CHECK-64:     i8* getelementptr inbounds ([11 x i8], [11 x i8]* @".str.10.@24@0:8q16", i64 0, i64 0),
// CHECK-64:     i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoC7bellsOnACSgSi_tcfcTo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:   }, {
// CHECK-64:     i8* getelementptr inbounds ([15 x i8], [15 x i8]* @"\01L_selector_data(.cxx_construct)", i64 0, i64 0),
// CHECK-64:     i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @".str.7.v16@0:8", i64 0, i64 0),
// CHECK-64:     i8* bitcast ({{.*}}* @"$s13objc_subclass10SwiftGizmoCfeTo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:   }]
// CHECK-64: }, section "__DATA, {{.*}}", align 8

// CHECK-32: @_IVARS__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}] } {
// CHECK-32:   i32 20,
// CHECK-32:   i32 1,
// CHECK-32:   [1 x { i32*, i8*, i8*, i32, i32 }] [{ i32*, i8*, i8*, i32, i32 } {
// CHECK-32:     i32* @"$s13objc_subclass10SwiftGizmoC1xSivpWvd",
// CHECK-32:     i8* getelementptr inbounds ([2 x i8], [2 x i8]* @.str.1.x, i32 0, i32 0),
// CHECK-32:     i8* getelementptr inbounds ([1 x i8], [1 x i8]* {{.*}}, i32 0, i32 0),
// CHECK-32:     i32 2,
// CHECK-32:     i32 4 }]
// CHECK-32: }, section "__DATA, {{.*}}", align 4

// CHECK-64: @_IVARS__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}] } {
// CHECK-64:   i32 32,
// CHECK-64:   i32 1,
// CHECK-64:   [1 x { i64*, i8*, i8*, i32, i32 }] [{ i64*, i8*, i8*, i32, i32 } {
// CHECK-64:     i64* @"$s13objc_subclass10SwiftGizmoC1xSivpWvd",
// CHECK-64:     i8* getelementptr inbounds ([2 x i8], [2 x i8]* @.str.1.x, i64 0, i64 0),
// CHECK-64:     i8* getelementptr inbounds ([1 x i8], [1 x i8]* @.str.0., i64 0, i64 0),
// CHECK-64:     i32 3,
// CHECK-64:     i32 8 }]
// CHECK-64: }, section "__DATA, {{.*}}", align 8

// CHECK-32: @_DATA__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}* } {
// CHECK-32:   i32 132,
// CHECK-32:   i32 4,
// CHECK-32:   i32 8,
// CHECK-32:   i8* null,
// CHECK-32:   i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @.str.31._TtC13objc_subclass10SwiftGizmo, i32 0, i32 0),
// CHECK-32:   @_INSTANCE_METHODS__TtC13objc_subclass10SwiftGizmo,
// CHECK-32:   i8* null,
// CHECK-32:   @_IVARS__TtC13objc_subclass10SwiftGizmo,
// CHECK-32:   i8* null,
// CHECK-32:   @_PROPERTIES__TtC13objc_subclass10SwiftGizmo
// CHECK-32: }, section "__DATA, {{.*}}", align 4

// CHECK-64: @_DATA__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}* } {
// CHECK-64:    i32 132,
// CHECK-64:    i32 8,
// CHECK-64:    i32 16,
// CHECK-64:    i32 0,
// CHECK-64:    i8* null,
// CHECK-64:    i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @.str.31._TtC13objc_subclass10SwiftGizmo, i64 0, i64 0),
// CHECK-64:    @_INSTANCE_METHODS__TtC13objc_subclass10SwiftGizmo,
// CHECK-64:    i8* null,
// CHECK-64:    @_IVARS__TtC13objc_subclass10SwiftGizmo,
// CHECK-64:    i8* null,
// CHECK-64:    @_PROPERTIES__TtC13objc_subclass10SwiftGizmo
// CHECK-64:  }, section "__DATA, {{.*}}", align 8

// CHECK-NOT: @_TMCSo13SwiftGizmo = {{.*NSObject}}

// CHECK: @_INSTANCE_METHODS__TtC13objc_subclass12GenericGizmo

// CHECK-32: [[SETTER_ENCODING:@.*]] = private unnamed_addr constant [10 x i8] c"v12@0:4@8\00"
// CHECK-64: [[SETTER_ENCODING:@.*]] = private unnamed_addr constant [11 x i8] c"v24@0:8@16\00"

// CHECK-32: @_INSTANCE_METHODS__TtC13objc_subclass11SwiftGizmo2 = internal constant { i32, i32, [5 x { i8*, i8*, i8* }] } {
// CHECK-32:   i32 12,
// CHECK-32:   i32 5,
// CHECK-32:   [5 x { i8*, i8*, i8* }] [
// CHECK-32:     {
// CHECK-32:       i8* getelementptr inbounds ([3 x i8], [3 x i8]* @"\01L_selector_data(sg)", i32 0, i32 0),
// CHECK-32:       i8* getelementptr inbounds ([7 x i8], [7 x i8]* @".str.6.@8@0:4", i32 0, i32 0),
// CHECK-32:       i8* bitcast ({{.*}}* @"$s13objc_subclass11SwiftGizmo2C2sgAA0C5GizmoCvgTo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:     }, {
// CHECK-32:       i8* getelementptr inbounds ([7 x i8], [7 x i8]* @"\01L_selector_data(setSg:)", i32 0, i32 0),
// CHECK-32:       i8* getelementptr inbounds ([10 x i8], [10 x i8]* [[SETTER_ENCODING]], i32 0, i32 0),
// CHECK-32:       i8* bitcast ({{.*}}* @"$s13objc_subclass11SwiftGizmo2C2sgAA0C5GizmoCvsTo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:     }, {
// CHECK-32:       i8* getelementptr inbounds ([5 x i8], [5 x i8]* @"\01L_selector_data(init)", i32 0, i32 0),
// CHECK-32:       i8* getelementptr inbounds ([7 x i8], [7 x i8]* @".str.6.@8@0:4", i32 0, i32 0),
// CHECK-32:       i8* bitcast ({{.*}}* @"$s13objc_subclass11SwiftGizmo2CACycfcTo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:     }, {
// CHECK-32:       i8* getelementptr inbounds ([17 x i8], [17 x i8]* @"\01L_selector_data(initWithBellsOn:)", i32 0, i32 0),
// CHECK-32:       i8* getelementptr inbounds ([10 x i8], [10 x i8]* {{@".str.[^"]+"}}, i32 0, i32 0),
// CHECK-32:       i8* bitcast ({{.*}}* @"$s13objc_subclass11SwiftGizmo2C7bellsOnACSgSi_tcfcTo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:     }, {
// CHECK-32:       i8* getelementptr inbounds ([14 x i8], [14 x i8]* @"\01L_selector_data(.cxx_destruct)", i32 0, i32 0),
// CHECK-32:       i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* {{@".str.[^"]+"}}, i32 0, i32 0),
// CHECK-32:       i8* bitcast ({{.*}}* @"$s13objc_subclass11SwiftGizmo2CfETo{{(\.ptrauth)?}}" to i8*)
// CHECK-32:     }
// CHECK-32:   ]
// CHECK-32: }, section "__DATA, {{.*}}", align 4

// CHECK-64: @_INSTANCE_METHODS__TtC13objc_subclass11SwiftGizmo2 = internal constant { i32, {{.*}}] } {
// CHECK-64:   i32 24,
// CHECK-64:   i32 5,
// CHECK-64:   [5 x { i8*, i8*, i8* }] [
// CHECK-64:     {
// CHECK-64:       i8* getelementptr inbounds ([3 x i8], [3 x i8]* @"\01L_selector_data(sg)", i64 0, i64 0),
// CHECK-64:       i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.@16@0:8", i64 0, i64 0),
// CHECK-64:       i8* bitcast ({{.*}}* @"$s13objc_subclass11SwiftGizmo2C2sgAA0C5GizmoCvgTo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:     }, {
// CHECK-64:       i8* getelementptr inbounds ([7 x i8], [7 x i8]* @"\01L_selector_data(setSg:)", i64 0, i64 0),
// CHECK-64:       i8* getelementptr inbounds ([11 x i8], [11 x i8]* [[SETTER_ENCODING]], i64 0, i64 0),
// CHECK-64:       i8* bitcast ({{.*}}* @"$s13objc_subclass11SwiftGizmo2C2sgAA0C5GizmoCvsTo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:     }, {
// CHECK-64:       i8* getelementptr inbounds ([5 x i8], [5 x i8]* @"\01L_selector_data(init)", i64 0, i64 0),
// CHECK-64:       i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.@16@0:8", i64 0, i64 0),
// CHECK-64:       i8* bitcast ({{.*}}* @"$s13objc_subclass11SwiftGizmo2CACycfcTo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:     }, {
// CHECK-64:       i8* getelementptr inbounds ([17 x i8], [17 x i8]* @"\01L_selector_data(initWithBellsOn:)", i64 0, i64 0),
// CHECK-64:       i8* getelementptr inbounds ([11 x i8], [11 x i8]* @".str.10.@24@0:8q16", i64 0, i64 0),
// CHECK-64:       i8* bitcast ({{.*}}* @"$s13objc_subclass11SwiftGizmo2C7bellsOnACSgSi_tcfcTo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:     }, {
// CHECK-64:       i8* getelementptr inbounds ([14 x i8], [14 x i8]* @"\01L_selector_data(.cxx_destruct)", i64 0, i64 0),
// CHECK-64:       i8* getelementptr inbounds ([{{[0-9]+}} x i8], [{{[0-9]+}} x i8]* @".str.7.v16@0:8", i64 0, i64 0)
// CHECK-64:       i8* bitcast ({{.*}}* @"$s13objc_subclass11SwiftGizmo2CfETo{{(\.ptrauth)?}}" to i8*)
// CHECK-64:     }
// CHECK-64: ] }


// CHECK: @"objc_classes_$s13objc_subclass10SwiftGizmoCN" = internal global i8* bitcast (%swift.type* @"$s13objc_subclass10SwiftGizmoCN" to i8*), section "__DATA,__objc_classlist,regular,no_dead_strip", no_sanitize_address, align [[WORD_SIZE_IN_BYTES]]
// CHECK: @"objc_classes_$s13objc_subclass11SwiftGizmo2CN" = internal global i8* bitcast (%swift.type* @"$s13objc_subclass11SwiftGizmo2CN" to i8*), section "__DATA,__objc_classlist,regular,no_dead_strip", no_sanitize_address, align [[WORD_SIZE_IN_BYTES]]

// CHECK: @objc_non_lazy_classes = internal global [1 x i8*] [i8* bitcast (%swift.type* @"$s13objc_subclass11SwiftGizmo2CN" to i8*)], section "__DATA,__objc_nlclslist,regular,no_dead_strip", no_sanitize_address, align [[WORD_SIZE_IN_BYTES]]

import Foundation
import gizmo

@requires_stored_property_inits
class SwiftGizmo : Gizmo {
  @objc var x = Int()

  @objc func getX() -> Int {
    return x
  }

  override func duplicate() -> Gizmo {
    return SwiftGizmo()
  }

  override init() {
    super.init(bellsOn:0)
  }

  @objc init(int i: Int, string str : String) {
    super.init(bellsOn:i)
  }

  deinit { var x = 10 }

  @objc var enabled: Bool {
    @objc(isEnabled) get {
      return true
    }

    @objc(setIsEnabled:) set {
    }
  }
}

class GenericGizmo<T> : Gizmo {
  @objc func foo() {}

  @objc var x : Int {
    return 0
  }

  var array : [T] = []
}
// CHECK: define hidden swiftcc [[LLVM_PTRSIZE_INT]] @"$s13objc_subclass12GenericGizmoC1xSivg"(

var sg = SwiftGizmo()
sg.duplicate()

@_objc_non_lazy_realization
class SwiftGizmo2 : Gizmo {
  @objc var sg : SwiftGizmo

  override init() {
    sg = SwiftGizmo()
    super.init()
  }

  deinit { }
}

