// RUN: %batch-code-completion

// TYPEATTR-NOT: myIntValue
// TYPEATTR-NOT: Type Attribute
// TYPEATTR-DAG: Keyword/None:                       autoclosure[#Type Attribute#]; name=autoclosure
// TYPEATTR-DAG: Keyword/None:                       convention(swift)[#Type Attribute#]; name=convention(swift)
// TYPEATTR-DAG: Keyword/None:                       convention(block)[#Type Attribute#]; name=convention(block)
// TYPEATTR-DAG: Keyword/None:                       convention(c)[#Type Attribute#]; name=convention(c)
// TYPEATTR-DAG: Keyword/None:                       convention(thin)[#Type Attribute#]; name=convention(thin)
// TYPEATTR-DAG: Keyword/None:                       escaping[#Type Attribute#]; name=escaping
// TYPEATTR-DAG: Keyword/None:                       isolated(any)[#Type Attribute#]; name=isolated(any)
// TYPEATTR-DAG: Keyword/None:                       noDerivative[#Type Attribute#]; name=noDerivative
// TYPEATTR-DAG: Keyword/None:                       Sendable[#Type Attribute#]; name=Sendable
// TYPEATTR-NOT: Type Attribute
// TYPEATTR-DAG: Decl[Struct]/CurrModule:            MyStruct[#MyStruct#]; name=MyStruct
// TYPEATTR-NOT: Type Attribute
// TYPEATTR-NOT: myIntValue

// TYPEATTR_INHERIT-DAG: Keyword/None:               autoclosure[#Type Attribute#]; name=autoclosure
// TYPEATTR_INHERIT-DAG: Keyword/None:               convention(swift)[#Type Attribute#]; name=convention(swift)
// TYPEATTR_INHERIT-DAG: Keyword/None:               convention(block)[#Type Attribute#]; name=convention(block)
// TYPEATTR_INHERIT-DAG: Keyword/None:               convention(c)[#Type Attribute#]; name=convention(c)
// TYPEATTR_INHERIT-DAG: Keyword/None:               convention(thin)[#Type Attribute#]; name=convention(thin)
// TYPEATTR_INHERIT-DAG: Keyword/None:               escaping[#Type Attribute#]; name=escaping
// TYPEATTR_INHERIT-DAG: Keyword/None:               isolated(any)[#Type Attribute#]; name=isolated(any)
// TYPEATTR_INHERIT-DAG: Keyword/None:               noDerivative[#Type Attribute#]; name=noDerivative
// TYPEATTR_INHERIT-DAG: Keyword/None:               Sendable[#Type Attribute#]; name=Sendable
//
// TYPEATTR_INHERIT-DAG: Keyword/None:               retroactive[#Type Attribute#]; name=retroactive
// TYPEATTR_INHERIT-DAG: Keyword/None:               unchecked[#Type Attribute#]; name=unchecked
// TYPEATTR_INHERIT-DAG: Keyword/None:               preconcurrency[#Type Attribute#]; name=preconcurrency

struct MyStruct : @#^STRUCT_INHERIT?check=TYPEATTR_INHERIT^# {}

class C : @#^CLASS_INHERIT?check=TYPEATTR_INHERIT^#, 
          Array<@#^GENERIC_ARG_INHERIT?check=TYPEATTR^#>,
          @unchecked @#^CLASS_INHERIT2?check=TYPEATTR_INHERIT^# {}

extension C : @#^EXT_INHERIT1?check=TYPEATTR_INHERIT^# {}

protocol P : @#^PROTO_INHERIT?check=TYPEATTR_INHERIT^# {}
extension C : P, @#^EXT_INHERIT2?check=TYPEATTR_INHERIT^# {}

var myIntValue: Int = 1

func foo() -> @#^FUNC_RESULT?check=TYPEATTR^# {}

func foo(x: @#^FUNC_PARAM?check=TYPEATTR^#) {}

func foo(x: (Int, @#^CLOSURE_PARAM?check=TYPEATTR^#) -> Void) {}

func foo(x: (Int) -> @#^CLOSURE_RESULT?check=TYPEATTR^#) {}
func foo(x: Array<@#^GENERIC_ARGS?check=TYPEATTR^#>) {}

func foo<T>() where T.Something == @#^WHERE_CLAUSE?check=TYPEATTR^# {}

func test() {
    let value: @#^VARDECL?check=TYPEATTR^#
}
func test() {
    let value: @Sendable @#^MULTIPLE?check=TYPEATTR^#
}
func test() {
    typealias A = @#^TYPEALIAS?check=TYPEATTR^#
}
func test(thing: Any) {
    let _ = thing as? @#^CAST?check=TYPEATTR^#
}

