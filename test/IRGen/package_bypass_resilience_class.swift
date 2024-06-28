// RUN: %empty-directory(%t)
// RUN: split-file %s %t

/// Build with -experimental-allow-non-resilient-access
// RUN: %target-build-swift %t/Core.swift \
// RUN: -module-name=Core -package-name Pkg \
// RUN: -Xfrontend -experimental-allow-non-resilient-access \
// RUN: -enable-library-evolution -O -wmo \
// RUN: -emit-ir -o %t/Core.ir \
// RUN: -emit-tbd -emit-tbd-path %t/libCore.tbd \
// RUN: -Xfrontend -tbd-install_name=libCore.dylib -Xfrontend -validate-tbd-against-ir=all

/// Build without -experimental-allow-non-resilient-access
// RUN: %target-build-swift %t/Core.swift \
// RUN: -module-name=Core -package-name Pkg \
// RUN: -enable-library-evolution -O -wmo \
// RUN: -emit-ir -o %t/CoreRes.ir \
// RUN: -emit-tbd -emit-tbd-path %t/libCoreRes.tbd \
// RUN: -Xfrontend -tbd-install_name=libCoreRes.dylib -Xfrontend -validate-tbd-against-ir=all

// RUN: %FileCheck %s --check-prefixes=CHECK-COMMON,CHECK-OPT < %t/Core.ir
// RUN: %FileCheck %s --check-prefixes=CHECK-TBD-COMMON,CHECK-TBD-OPT < %t/libCore.tbd
// RUN: %FileCheck %s --check-prefixes=CHECK-COMMON,CHECK-RES < %t/CoreRes.ir
// RUN: %FileCheck %s --check-prefixes=CHECK-TBD-COMMON,CHECK-TBD-RES < %t/libCoreRes.tbd

//--- Core.swift

// CHECK-RES-NOT: s4Core8UFIKlassC6varUfiSSSgvpfi
// CHECK-RES-NOT: s4Core3FooC02myB0AA3PubCSgvpfi

final public class Pub {
  // type metadata accessor for Core.Pub
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc %swift.metadata_response @"$s4Core3PubCMa"({{i32|i64}} %0)

  // method lookup function for Core.Pub
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc ptr @"$s4Core3PubCMu"(ptr %0, ptr %1)

  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc ptr @"$s4Core3PubCfd"(ptr readnone returned swiftself %0)
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc void @"$s4Core3PubCfD"(ptr swiftself %0)
}

package class Foo {
  // key path getter for Core.Foo.myFoo
  // CHECK-COMMON-DAG: define linkonce_odr hidden swiftcc void @"$s4Core3FooC02myB0AA3PubCSgvpACTK"

  // key path setter for Core.Foo.myFoo
  // CHECK-COMMON-DAG: define linkonce_odr hidden swiftcc void @"$s4Core3FooC02myB0AA3PubCSgvpACTk"

  // variable initialization expression of Core.Foo.myFoo
  // CHECK-OPT-DAG: define {{(dllexport |protected )?}}swiftcc {{i32|i64}} @"$s4Core3FooC02myB0AA3PubCSgvpfi"() #0 {

  // Core.Foo.myFoo.getter
  // CHECK-RES-DAG: define hidden {{.*}}swiftcc {{i32|i64}} @"$s4Core3FooC02myB0AA3PubCSgvg"(ptr swiftself %0)
  // CHECK-OPT-DAG: define {{(dllexport |protected )?}}swiftcc {{i32|i64}} @"$s4Core3FooC02myB0AA3PubCSgvg"(ptr swiftself %0)

  // merged Core.Foo.myFoo.getter
  // CHECK-COMMON-DAG: define internal swiftcc {{i32|i64}} @"$s4Core3FooC02myB0AA3PubCSgvgTm"(ptr swiftself %0)

  // Core.Foo.myFoo.setter
  // CHECK-RES-DAG: define hidden {{.*}}swiftcc void @"$s4Core3FooC02myB0AA3PubCSgvs"({{i32|i64}} %0, ptr swiftself %1) #1 {
  // CHECK-OPT-DAG: define {{(dllexport |protected )?}}swiftcc void @"$s4Core3FooC02myB0AA3PubCSgvs"({{i32|i64}} %0, ptr swiftself %1) #1 {

  // merged Core.Foo.myFoo.setter
  // CHECK-COMMON-DAG: define internal swiftcc void @"$s4Core3FooC02myB0AA3PubCSgvsTm"({{i32|i64}} %0, ptr swiftself %1)

  // Core.Foo.myFoo.modify
  // CHECK-RES-DAG: define hidden {{.*}}swiftcc { ptr, ptr } @"$s4Core3FooC02myB0AA3PubCSgvM"
  // CHECK-OPT-DAG: define {{(dllexport |protected )?}}swiftcc { ptr, ptr } @"$s4Core3FooC02myB0AA3PubCSgvM"

  // Core.Foo.myFoo.modify
  // CHECK-COMMON-DAG: define internal swiftcc void @"$s4Core3FooC02myB0AA3PubCSgvM.resume.0"

  // type metadata accessor for Core.Foo
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc %swift.metadata_response @"$s4Core3FooCMa"

  // method lookup function for Core.Foo
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc ptr @"$s4Core3FooCMu"(ptr %0, ptr %1)

  // dispatch thunk of Core.Foo.myFoo.getter
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc {{i32|i64}} @"$s4Core3FooC02myB0AA3PubCSgvgTj"(ptr swiftself %0)

  // dispatch thunk of Core.Foo.myFoo.setter
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc void @"$s4Core3FooC02myB0AA3PubCSgvsTj"({{i32|i64}} %0, ptr swiftself %1)

  // dispatch thunk of Core.Foo.myFoo.modify
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc { ptr, ptr } @"$s4Core3FooC02myB0AA3PubCSgvMTj"

  // Core.Foo.deinit
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc ptr @"$s4Core3FooCfd"(ptr readonly returned swiftself %0)

  // Core.Foo.__deallocating_deinit
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc void @"$s4Core3FooCfD"(ptr swiftself %0)

  package var myFoo: Pub?
}

final package class Bar {
  
  // CHECK-OPT-DAG: define {{(dllexport |protected )?}}swiftcc {{i32|i64}} @"$s4Core3BarC02myB0AA3PubCSgvpfi"()
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc {{i32|i64}} @"$s4Core3BarC02myB0AA3PubCSgvg"(ptr swiftself %0)
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc void @"$s4Core3BarC02myB0AA3PubCSgvs"({{i32|i64}} %0, ptr swiftself %1)
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc { ptr, ptr } @"$s4Core3BarC02myB0AA3PubCSgvM"
  // CHECK-COMMON-DAG: define internal swiftcc void @"$s4Core3BarC02myB0AA3PubCSgvM.resume.0"

  // type metadata accessor for Core.Bar
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc %swift.metadata_response @"$s4Core3BarCMa"({{i32|i64}} %0)

  // method lookup function for Core.Bar
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc ptr @"$s4Core3BarCMu"(ptr %0, ptr %1)

  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc ptr @"$s4Core3BarCfd"(ptr readonly returned swiftself %0)
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc void @"$s4Core3BarCfD"(ptr swiftself %0)

  package var myBar: Pub?
}

@usableFromInline
class UFIKlass {
  var varNonUfi: String?

  // variable initialization expression of Core.UFIKlass.varUfi
  // CHECK-OPT-DAG: define {{(dllexport |protected )?}}swiftcc {{.*}} @"$s4Core8UFIKlassC6varUfiSSSgvpfi"()

  // key path getter for Core.UFIKlass.varUfi
  // CHECK-COMMON-DAG: define linkonce_odr hidden swiftcc void @"$s4Core8UFIKlassC6varUfiSSSgvpACTK"

  // key path setter for Core.UFIKlass.varUfi
  // CHECK-COMMON-DAG: define linkonce_odr hidden swiftcc void @"$s4Core8UFIKlassC6varUfiSSSgvpACTk"
  
  // dispatch thunk of Core.UFIKlass.varUfi.getter
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc {{.*}} @"$s4Core8UFIKlassC6varUfiSSSgvgTj"

  // dispatch thunk of Core.UFIKlass.varUfi.setter
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc void @"$s4Core8UFIKlassC6varUfiSSSgvsTj"

  // dispatch thunk of Core.UFIKlass.varUfi.modify
  // CHECK-COMMON-DAG: define {{(dllexport |protected )?}}swiftcc { ptr, ptr } @"$s4Core8UFIKlassC6varUfiSSSgvMTj"

  // Core.UFIKlass.varUfi.getter
  // CHECK-RES-DAG: define hidden {{.*}}swiftcc {{.*}} @"$s4Core8UFIKlassC6varUfiSSSgvg"
  // CHECK-OPT-DAG: define {{(dllexport |protected )?}}swiftcc {{.*}} @"$s4Core8UFIKlassC6varUfiSSSgvg"

  // Core.UFIKlass.varUfi.setter
  // CHECK-RES-DAG: define hidden {{.*}}swiftcc void @"$s4Core8UFIKlassC6varUfiSSSgvs"
  // CHECK-OPT-DAG: define {{(dllexport |protected )?}}swiftcc void @"$s4Core8UFIKlassC6varUfiSSSgvs"

  // Core.UFIKlass.varUfi.modify
  // CHECK-RES-DAG: define hidden {{.*}}swiftcc { ptr, ptr } @"$s4Core8UFIKlassC6varUfiSSSgvM"
  // CHECK-OPT-DAG: define {{(dllexport |protected )?}}swiftcc { ptr, ptr } @"$s4Core8UFIKlassC6varUfiSSSgvM"

  @usableFromInline
  var varUfi: String?
}

class InternalKlass {
  var varInternal: String?
}

/// TBD
///
/// Core.Foo
// property descriptor for Core.Foo.myFoo
// CHECK-TBD-COMMON-DAG: s4Core3FooC02myB0AA3PubCSgvpMV
// method descriptor for Core.Foo.myFoo.getter
// CHECK-TBD-COMMON-DAG: s4Core3FooC02myB0AA3PubCSgvgTq
// method descriptor for Core.Foo.myFoo.setter
// CHECK-TBD-COMMON-DAG: s4Core3FooC02myB0AA3PubCSgvsTq
// method descriptor for Core.Foo.myFoo.modify
// CHECK-TBD-COMMON-DAG: s4Core3FooC02myB0AA3PubCSgvMTq
// type metadata accessor for Core.Foo
// CHECK-TBD-COMMON-DAG: s4Core3FooCMa
// nominal type descriptor for Core.Foo
// CHECK-TBD-COMMON-DAG: s4Core3FooCMn

// dispatch thunk of Core.Foo.myFoo.getter
// CHECK-TBD-COMMON-DAG: s4Core3FooC02myB0AA3PubCSgvgTj
// dispatch thunk of Core.Foo.myFoo.setter
// CHECK-TBD-COMMON-DAG: s4Core3FooC02myB0AA3PubCSgvsTj
// dispatch thunk of Core.Foo.myFoo.modify
// CHECK-TBD-COMMON-DAG: s4Core3FooC02myB0AA3PubCSgvMTj
// method lookup function for Core.Foo
// CHECK-TBD-COMMON-DAG: s4Core3FooCMu
// class metadata base offset for Core.Foo
// CHECK-TBD-COMMON-DAG: s4Core3FooCMo

// CHECK-TBD-OPT-DAG: s4Core3FooC02myB0AA3PubCSgvpfi
// CHECK-TBD-OPT-DAG: s4Core3FooC02myB0AA3PubCSgvg
// CHECK-TBD-OPT-DAG: s4Core3FooC02myB0AA3PubCSgvs
// CHECK-TBD-COMMON-DAG: s4Core3FooCfd
// CHECK-TBD-COMMON-DAG: s4Core3FooCfD

/// Core.UFIKlass
// property descriptor for Core.UFIKlass.varUfi
// CHECK-TBD-COMMON-DAG: s4Core8UFIKlassC6varUfiSSSgvpMV
// method descriptor for Core.UFIKlass.varUfi.getter
// CHECK-TBD-COMMON-DAG: s4Core8UFIKlassC6varUfiSSSgvgTq
// method descriptor for Core.UFIKlass.varUfi.setter
// CHECK-TBD-COMMON-DAG: s4Core8UFIKlassC6varUfiSSSgvsTq
// method descriptor for Core.UFIKlass.varUfi.modify
// CHECK-TBD-COMMON-DAG: s4Core8UFIKlassC6varUfiSSSgvMTq
// type metadata accessor for Core.UFIKlass
// CHECK-TBD-COMMON-DAG: s4Core8UFIKlassCMa
// nominal type descriptor for Core.UFIKlass
// CHECK-TBD-COMMON-DAG: s4Core8UFIKlassCMn

// dispatch thunk of Core.UFIKlass.varUfi.getter
// CHECK-TBD-COMMON-DAG: s4Core8UFIKlassC6varUfiSSSgvgTj
// dispatch thunk of Core.UFIKlass.varUfi.setter
// CHECK-TBD-COMMON-DAG: s4Core8UFIKlassC6varUfiSSSgvsTj
// dispatch thunk of Core.UFIKlass.varUfi.modify
// CHECK-TBD-COMMON-DAG: s4Core8UFIKlassC6varUfiSSSgvMTj
// method lookup function for Core.UFIKlass
// CHECK-TBD-COMMON-DAG: s4Core8UFIKlassCMu
// class metadata base offset for Core.UFIKlass
// CHECK-TBD-COMMON-DAG: s4Core8UFIKlassCMo

// CHECK-TBD-OPT-DAG: s4Core8UFIKlassC6varUfiSSSgvpfi
// CHECK-TBD-OPT-DAG: s4Core8UFIKlassC9varNonUfiSSSgvpfi
// CHECK-TBD-OPT-DAG: s4Core8UFIKlassC6varUfiSSSgvg
// CHECK-TBD-OPT-DAG: s4Core8UFIKlassC6varUfiSSSgvs
// CHECK-TBD-COMMON-DAG: s4Core8UFIKlassCfd
// CHECK-TBD-COMMON-DAG: s4Core8UFIKlassCfD

/// Core.Bar is a final class so no dispatch thunks for its methods
// CHECK-TBD-OPT-DAG: s4Core3BarC02myB0AA3PubCSgvpfi
// CHECK-TBD-OPT-DAG: s4Core3BarC02myB0AA3PubCSgvg
// CHECK-TBD-OPT-DAG: s4Core3BarC02myB0AA3PubCSgvs
// CHECK-TBD-OPT-DAG: s4Core3BarC02myB0AA3PubCSgvM
// CHECK-TBD-COMMON-DAG: s4Core3BarC02myB0AA3PubCSgvpMV
// CHECK-TBD-COMMON-DAG: s4Core3BarCMa
// CHECK-TBD-COMMON-DAG: s4Core3BarCMn
// CHECK-TBD-COMMON-DAG: s4Core3BarCfd
// CHECK-TBD-COMMON-DAG: s4Core3BarCfD
// CHECK-TBD-COMMON-DAG: s4Core3BarCMu
// CHECK-TBD-COMMON-DAG: s4Core3BarCMo

/// Core.Pub is a final empty class
// CHECK-TBD-COMMON-DAG: s4Core3PubCMa
// CHECK-TBD-COMMON-DAG: s4Core3PubCMn
// CHECK-TBD-COMMON-DAG: s4Core3PubCfd
// CHECK-TBD-COMMON-DAG: s4Core3PubCfD
// CHECK-TBD-COMMON-DAG: s4Core3PubCMu
// CHECK-TBD-COMMON-DAG: s4Core3PubCMo
