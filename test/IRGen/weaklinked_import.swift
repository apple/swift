// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -emit-module -emit-module-path %t/weaklinked_import_helper.swiftmodule -parse-as-library %S/Inputs/weaklinked_import_helper.swift -enable-library-evolution
//
// RUN: %target-swift-frontend -primary-file %s -I %t -emit-ir | %FileCheck %s

// UNSUPPORTED: OS=windows-msvc

@_weakLinked import weaklinked_import_helper

// CHECK-DAG: @"$s24weaklinked_import_helper12ConcreteTypeVAA13OtherProtocolAAWP" = extern_weak global ptr
// CHECK-DAG: @"$s24weaklinked_import_helper12ConcreteTypeVMn" = extern_weak global %swift.type_descriptor
// CHECK-DAG: @"$s24weaklinked_import_helper17ProtocolWithAssocMp" = extern_weak global %swift.protocol
// CHECK-DAG: @"$s24weaklinked_import_helper17ProtocolWithAssocP1TAC_AA05OtherD0Tn" = extern_weak global %swift.protocol_requirement
// CHECK-DAG: @"$s1T24weaklinked_import_helper17ProtocolWithAssocPTl" = extern_weak global %swift.protocol_requirement
// CHECK-DAG: @"$s24weaklinked_import_helper17ProtocolWithAssocP1fyyFTq" = extern_weak global %swift.method_descriptor
// CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper17ProtocolWithAssocPAAE1fyyF"
struct ConformsToProtocolWithAssoc: ProtocolWithAssoc {}

func testTopLevel() {
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper2fnyyF"()
  fn()

  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper12globalStoredSivg"
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper12globalStoredSivs"
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper12globalStoredSivM"
  let x = globalStored
  globalStored = x
  globalStored += 1

  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper14globalComputedSivg"
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper14globalComputedSivs"
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper14globalComputedSivM"
  let y = globalComputed
  globalComputed = y
  globalComputed += 1
}

func testStruct() {
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1SVMa"
  _ = MemoryLayout<S>.size

  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1SVACycfC"
  var s = S()

  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1SV2fnyyF"
  s.fn()

  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1SV10storedPropSivg"
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1SV10storedPropSivs"
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1SV10storedPropSivM"
  let x = s.storedProp
  s.storedProp = x
  s.storedProp += 1

  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1SV12computedPropSivg"
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1SV12computedPropSivs"
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1SV12computedPropSivM"
  let y = s.computedProp
  s.computedProp = y
  s.computedProp += 1

  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1SVyS2icig"
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1SVyS2icis"
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1SVyS2iciM"
  let z = s[0]
  s[0] = z
  s[0] += 1
}

func testEnum() {
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1EOMa"
  _ = MemoryLayout<E>.size

  // CHECK-DAG: @"$s24weaklinked_import_helper1EO5basicyA2CmFWC" = extern_weak constant i32
  _ = E.basic

  // CHECK-DAG: @"$s24weaklinked_import_helper1EO5assocyACSicACmFWC" = extern_weak constant i32
  _ = E.assoc(0)
}

func testClass() {
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1CCMa"
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1CCACycfC"
  let c = C()

  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1CC2fnyyFTj"
  c.fn()

  // CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1CC10storedPropSivgTj"
  // CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1CC10storedPropSivsTj"
  // CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1CC10storedPropSivMTj"
  let x = c.storedProp
  c.storedProp = x
  c.storedProp += 1

  // CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1CC12computedPropSivgTj"
  // CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1CC12computedPropSivsTj"
  // CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1CC12computedPropSivMTj"
  let y = c.computedProp
  c.computedProp = y
  c.computedProp += 1

  // CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1CCyS2icigTj"
  // CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1CCyS2icisTj"
  // CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1CCyS2iciMTj"
  let z = c[0]
  c[0] = z
  c[0] += 1
}

// CHECK-DAG: @"$s24weaklinked_import_helper1CCMn" = extern_weak global %swift.type_descriptor
// CHECK-DAG: @"$s24weaklinked_import_helper1CCACycfCTq" = extern_weak global %swift.method_descriptor
// CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1CCACycfc"
// CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1CCfd"
class Sub: C {}

func testProtocolExistential(_ p: P) {
  var mutP = p

  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1PP2fnyyFTj"
  p.fn()

  // CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1PP4propSivgTj"
  // CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1PP4propSivsTj"
  // CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1PP4propSivMTj"
  let x = p.prop
  mutP.prop = x
  mutP.prop += 1

  // CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1PPyS2icigTj"
  // CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1PPyS2icisTj"
  // CHECK-DAG: declare extern_weak swiftcc {{.+}} @"$s24weaklinked_import_helper1PPyS2iciMTj"
  let z = p[0]
  mutP[0] = z
  mutP[0] += 1
}

func testProtocolGeneric<Impl: P>(_ type: Impl.Type) {
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper1PPxycfCTj"
  var mutP = type.init()

  mutP.fn()

  let x = mutP.prop
  mutP.prop = x
  mutP.prop += 1

  let z = mutP[0]
  mutP[0] = z
  mutP[0] += 1
}

// CHECK-DAG: @"$s24weaklinked_import_helper5BasePMp" = extern_weak global %swift.protocol
protocol RefinesP: BaseP {}

// CHECK-DAG: @"$s24weaklinked_import_helper1SVAA5BasePAAWP" = extern_weak global ptr
// CHECK-DAG: @"$s24weaklinked_import_helper1SVMn" = extern_weak global %swift.type_descriptor
extension S: RefinesP {}

func testInlining() {
  // CHECK-DAG: define linkonce_odr hidden {{.+}} @"$s24weaklinked_import_helper22alwaysEmitIntoClientFnyyF"()
  // CHECK-DAG: declare extern_weak {{.+}} @"$s24weaklinked_import_helper18usableFromInlineFnyyF"
  alwaysEmitIntoClientFn()
}
