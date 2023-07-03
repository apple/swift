// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t -module-name "Mod"

protocol BaseP1 {}
protocol BaseP2 {}

func testBasicArchetype(arg: some BaseP1) {
  arg.#^TestBaseP1^#
// TestBaseP1: LookedupTypeNames: ['Mod.BaseP1']
}


protocol DerivedP1: BaseP1 {}
protocol DerivedP2: BaseP2 {}
protocol DerivedPComp: BaseP1, BaseP2 {}

func testInheritedArchetype(arg: some DerivedP1) {
  arg.#^TestDerivedP1^#
// TestDerivedP1: LookedupTypeNames: ['Mod.DerivedP1', 'Mod.BaseP1']
}

func testMultiInheritedArchetype(arg: some DerivedPComp) {
  arg.#^TestDerivedPComp^#
// TestDerivedPComp: LookedupTypeNames: ['Mod.DerivedPComp', 'Mod.BaseP1', 'Mod.BaseP2']
}

func testCompositionArchetype(arg: some BaseP1 & BaseP2) {
  arg.#^TestBaseP1AndBaseP2^#
// TestBaseP1AndBaseP2: LookedupTypeNames: ['Mod.BaseP1', 'Mod.BaseP2']
}

protocol DiamondRoot {}
protocol DiamondEdge1: DiamondRoot {}
protocol DiamondEdge2: DiamondRoot {}
protocol DiamondTop: DiamondEdge1, DiamondEdge2 {}

func testDiamondProtocol(arg: some DiamondTop) {
  arg.#^TestDiamondTop^#
// TestDiamondTop: LookedupTypeNames: ['Mod.DiamondTop', 'Mod.DiamondEdge1', 'Mod.DiamondRoot', 'Mod.DiamondEdge2']
}

func testExistential(arg: any DiamondTop) {
  arg.#^TestAnyDiamondTop^#
// TestAnyDiamondTop: LookedupTypeNames: ['Mod.DiamondTop', 'Mod.DiamondEdge1', 'Mod.DiamondRoot', 'Mod.DiamondEdge2']
}

class BaseClass {}
class DerivedClass: BaseClass {}

func testBasicClass(arg: BaseClass) {
  arg.#^TestBaseClass^#
// TestBaseClass: LookedupTypeNames: ['Mod.BaseClass']
}

func testSubClass(arg: DerivedClass) {
  arg.#^TestDerivedClass^#
// TestDerivedClass: LookedupTypeNames: ['Mod.DerivedClass', 'Mod.BaseClass']
}

protocol BaseClassConstrainedP: BaseClass {}
protocol DerivedClassConstrainedP: DerivedClass {}

func testClassConstrainedProto(arg: some BaseClassConstrainedP) {
  arg.#^TestBaseClassConstrainedP^#
// TestBaseClassConstrainedP: LookedupTypeNames: ['Mod.BaseClassConstrainedP', 'Mod.BaseClass']
}
func testClassConstriainedProto2(arg: some DerivedClassConstrainedP) {
  arg.#^TestDerivedClassConstrainedP^#
// TestDerivedClassConstrainedP: LookedupTypeNames: ['Mod.DerivedClassConstrainedP', 'Mod.DerivedClass', 'Mod.BaseClass']
}

class BaseClassWithProto: BaseP1 {}
class DerivedClassWithProto: BaseClassWithProto, BaseP2 {}

func testBaseClassWithProto(arg: BaseClassWithProto) {
  arg.#^TestBaseClassWithProto^#
// TestBaseClassWithProto: LookedupTypeNames: ['Mod.BaseClassWithProto', 'Mod.BaseP1']
}

func testDerivedClassWithProto(arg: DerivedClassWithProto) {
  arg.#^TestDerivedClassWithProto^#
// TestDerivedClassWithProto: LookedupTypeNames: ['Mod.DerivedClassWithProto', 'Mod.BaseP2', 'Mod.BaseP1', 'Mod.BaseClassWithProto']
}

struct GenericS<T> {}
extension GenericS: BaseP1 where T == Int {}

func testConditionalConformanceNo(arg: GenericS<String>) {
  arg.#^TestConditionalConformanceNo^#
// TestConditionalConformanceNo: LookedupTypeNames: ['Mod.GenericS', 'Swift.Sendable']
}

func testConditionalConformanceYes(arg: GenericS<Int>) {
  arg.#^TestConditionalConformanceYes^#
// TestConditionalConformanceYes: LookedupTypeNames: ['Mod.GenericS', 'Mod.BaseP1', 'Swift.Sendable']

}
