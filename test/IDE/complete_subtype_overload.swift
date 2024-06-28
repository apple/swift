// RUN: %batch-code-completion

class C {
  static func cMethod() -> C {}
}
class D : C {
  static func dMethod() -> D {}
}

func test1(_ x: C) {}
func test1(_ x: D) {}

// We prefer the subtype here, so we show completions for D.
test1(.#^TEST1^#)
// TEST1-DAG: Decl[StaticMethod]/Super:           cMethod()[#C#]; name=cMethod()
// TEST1-DAG: Decl[StaticMethod]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: dMethod()[#D#]; name=dMethod()
// TEST1-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: init()[#D#]; name=init()

func test2(_ x: C?) {}
func test2(_ x: D?) {}

test2(.#^TEST2^#)
// TEST2-DAG: Decl[StaticMethod]/Super:           cMethod()[#C#]; name=cMethod()
// TEST2-DAG: Decl[StaticMethod]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: dMethod()[#D#]; name=dMethod()
// TEST2-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: init()[#D#]; name=init()
// TEST2-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Convertible]: none[#Optional<D>#]; name=none
// TEST2-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Convertible]: some({#D#})[#Optional<D>#]; name=some()

func test3(_ x: C?) {}
func test3(_ x: D) {}

// We can still provide both C and D completions here.
test3(.#^TEST3^#)
// TEST3-DAG: Decl[StaticMethod]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: cMethod()[#C#]; name=cMethod()
// TEST3-DAG: Decl[StaticMethod]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: dMethod()[#D#]; name=dMethod()
// TEST3-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: init()[#D#]; name=init()
// TEST3-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Convertible]: none[#Optional<C>#]; name=none
// TEST3-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Convertible]: some({#C#})[#Optional<C>#]; name=some()

func test4(_ x: Int) {}
func test4(_ x: AnyHashable) {}

// Make sure we show Int completions.
test4(.#^TEST4^#)
// TEST4: Decl[StaticVar]/Super/Flair[ExprSpecific]/IsSystem/TypeRelation[Convertible]: zero[#Int#]; name=zero

protocol P {}
extension P {
  func pMethod() {}
}
struct S : P {
  func sMethod() {}
}

func test5() -> any P {}
func test5() -> S {}

test5().#^TEST5^#
// TEST5-DAG: Decl[InstanceMethod]/CurrNominal:   pMethod()[#Void#]; name=pMethod()
// TEST5-DAG: Decl[InstanceMethod]/CurrNominal:   sMethod()[#Void#]; name=sMethod()
