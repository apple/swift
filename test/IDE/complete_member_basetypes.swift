// RUN: %batch-code-completion -module-name "Mod"

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
// TestDerivedP1: LookedupTypeNames: ['Mod.BaseP1', 'Mod.DerivedP1']
}

func testMultiInheritedArchetype(arg: some DerivedPComp) {
  arg.#^TestDerivedPComp^#
// TestDerivedPComp: LookedupTypeNames: ['Mod.BaseP1', 'Mod.BaseP2', 'Mod.DerivedPComp']
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
// TestDiamondTop: LookedupTypeNames: ['Mod.DiamondEdge1', 'Mod.DiamondEdge2', 'Mod.DiamondRoot', 'Mod.DiamondTop']
}

func testExistential(arg: any DiamondTop) {
  arg.#^TestAnyDiamondTop^#
// TestAnyDiamondTop: LookedupTypeNames: ['Mod.DiamondEdge1', 'Mod.DiamondEdge2', 'Mod.DiamondRoot', 'Mod.DiamondTop']
}

class BaseClass {}
class DerivedClass: BaseClass {}

func testBasicClass(arg: BaseClass) {
  arg.#^TestBaseClass^#
// TestBaseClass: LookedupTypeNames: ['Mod.BaseClass']
}

func testSubClass(arg: DerivedClass) {
  arg.#^TestDerivedClass^#
// TestDerivedClass: LookedupTypeNames: ['Mod.BaseClass', 'Mod.DerivedClass']
}

protocol BaseClassConstrainedP: BaseClass {}
protocol DerivedClassConstrainedP: DerivedClass {}

func testClassConstrainedProto(arg: some BaseClassConstrainedP) {
  arg.#^TestBaseClassConstrainedP^#
// TestBaseClassConstrainedP: LookedupTypeNames: ['Mod.BaseClass', 'Mod.BaseClassConstrainedP']
}
func testClassConstriainedProto2(arg: some DerivedClassConstrainedP) {
  arg.#^TestDerivedClassConstrainedP^#
// TestDerivedClassConstrainedP: LookedupTypeNames: ['Mod.BaseClass', 'Mod.DerivedClass', 'Mod.DerivedClassConstrainedP']
}

class BaseClassWithProto: BaseP1 {}
class DerivedClassWithProto: BaseClassWithProto, BaseP2 {}

func testBaseClassWithProto(arg: BaseClassWithProto) {
  arg.#^TestBaseClassWithProto^#
// TestBaseClassWithProto: LookedupTypeNames: ['Mod.BaseClassWithProto', 'Mod.BaseP1']
}

func testDerivedClassWithProto(arg: DerivedClassWithProto) {
  arg.#^TestDerivedClassWithProto^#
// TestDerivedClassWithProto: LookedupTypeNames: ['Mod.BaseClassWithProto', 'Mod.BaseP1', 'Mod.BaseP2', 'Mod.DerivedClassWithProto']
}

struct GenericS<T> {}
extension GenericS: BaseP1 where T == Int {}

func testConditionalConformanceNo(arg: GenericS<String>) {
  arg.#^TestConditionalConformanceNo^#
// TestConditionalConformanceNo: LookedupTypeNames: ['Mod.GenericS', 'Swift.BitwiseCopyable', 'Swift.Sendable']
}

func testConditionalConformanceYes(arg: GenericS<Int>) {
  arg.#^TestConditionalConformanceYes^#
// TestConditionalConformanceYes: LookedupTypeNames: ['Mod.BaseP1', 'Mod.GenericS', 'Swift.BitwiseCopyable', 'Swift.Sendable']

}
