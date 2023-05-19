// RUN: %target-typecheck-verify-swift

func returnTuple1<each T>() -> (repeat each T) { fatalError() }
// expected-note@-1 {{in call to function 'returnTuple1()'}}

func returnTuple2<each T>() -> (Int, repeat each T) { fatalError() }
// expected-note@-1 2 {{in call to function 'returnTuple2()'}}

func returnTupleLabel1<each T>() -> (x: repeat each T) { fatalError() }
// expected-error@-1 {{cannot use label with pack expansion tuple element}}

func returnTupleLabel2<each T>() -> (Int, x: repeat each T) { fatalError() }
// expected-error@-1 {{cannot use label with pack expansion tuple element}}

func returnTupleLabel3<each T>() -> (Int, repeat each T, y: Float) { fatalError() }
// expected-note@-1 3 {{in call to function 'returnTupleLabel3()'}}

func returnTupleLabel4<each T>() -> (Int, x: repeat each T, y: Float) { fatalError() }
// expected-error@-1 {{cannot use label with pack expansion tuple element}}

func returnTupleLabel5<each T, each U>() -> (Int, repeat each T, y: repeat each U) { fatalError() }
// expected-error@-1 {{cannot use label with pack expansion tuple element}}

func returnTupleLabel6<each T, each U>() -> (Int, x: repeat each T, y: repeat each U) { fatalError() }
// expected-error@-1 2 {{cannot use label with pack expansion tuple element}}

func concreteReturnTupleValid() {
  let _: () = returnTuple1()
  let _: Int = returnTuple1()
  let _: (Int, String) = returnTuple1()

  let _: Int = returnTuple2()
  let _: (Int, String) = returnTuple2()
  let _: (Int, String, Float) = returnTuple2()

  let _: () = returnTupleLabel1()
  let _: Int = returnTupleLabel1()
  let _: (x: Int, String) = returnTupleLabel1()
  let _: (x: Int, String, Float) = returnTupleLabel1()

  let _: Int = returnTupleLabel2()
  let _: (Int, x: Int) = returnTupleLabel2()
  let _: (Int, x: Int, String) = returnTupleLabel2()
  let _: (Int, x: Int, String, Float) = returnTupleLabel2()

  let _: (Int, y: Float) = returnTupleLabel3()
  let _: (Int, Int, y: Float) = returnTupleLabel3()
  let _: (Int, Int, String, y: Float) = returnTupleLabel3()

  let _: (Int, y: Float) = returnTupleLabel4()
  let _: (Int, x: Int, y: Float) = returnTupleLabel4()
  let _: (Int, x: Int, String, y: Float) = returnTupleLabel4()

  let _: Int = returnTupleLabel5()
  let _: (Int, Int) = returnTupleLabel5()
  let _: (Int, Int, String) = returnTupleLabel5()
  let _: (Int, y: Double, Float) = returnTupleLabel5()
  let _: (Int, Int, String, Float, y: Double) = returnTupleLabel5()

  let _: Int = returnTupleLabel6()
  let _: (Int, x: Int) = returnTupleLabel6()
  let _: (Int, x: Int, String) = returnTupleLabel6()
  let _: (Int, y: Double, Float) = returnTupleLabel6()
  let _: (Int, x: Int, String, Float, y: Double) = returnTupleLabel6()
}

func concreteReturnTypeInvalid() {
  let _: Int = returnTuple1()

  let _: () = returnTuple2()
  // expected-error@-1 {{'(Int, repeat each T)' is not convertible to '()', tuples have a different number of elements}}
  // expected-error@-2 {{generic parameter 'each T' could not be inferred}}

  let _: Int = returnTupleLabel3()
  // expected-error@-1 {{cannot convert value of type '(Int, repeat each T, y: Float)' to specified type 'Int'}}
  // expected-error@-2 {{generic parameter 'each T' could not be inferred}}

  let _: (Int, Int, y: Float) = returnTupleLabel4() // error at declaration
  let _: () = returnTupleLabel5()  // error at declaration
}

func genericReturnTupleValid<each T>(_: repeat each T) {
  let _: (repeat each T) = returnTuple1()
  let _: (Int, repeat each T) = returnTuple1()

  let _: (Int, repeat each T) = returnTuple2()
  let _: (Int, String, repeat each T) = returnTuple2()

  let _: (x: repeat each T) = returnTupleLabel1()
  // expected-error@-1 {{cannot use label with pack expansion tuple element}}

  let _: (x: Int, repeat each T) = returnTupleLabel1()

  let _: (Int, x: repeat each T) = returnTupleLabel2()
  // expected-error@-1 {{cannot use label with pack expansion tuple element}}

  let _: (Int, x: String, repeat each T) = returnTupleLabel2()

  let _: (Int, repeat each T, y: Float) = returnTupleLabel3()
  let _: (Int, String, repeat each T, y: Float) = returnTupleLabel3()

  let _: (Int, x: repeat each T, y: Float) = returnTupleLabel4()
  // expected-error@-1 {{cannot use label with pack expansion tuple element}}

  let _: (Int, x: String, repeat each T, y: Float) = returnTupleLabel4()

  let _: (Int, repeat each T, y: repeat each T) = returnTupleLabel5()
  // expected-error@-1 {{cannot use label with pack expansion tuple element}}

  let _: (Int, String, repeat each T, y: Float, repeat each T) = returnTupleLabel5()

  let _: (Int, x: repeat each T, y: repeat each T) = returnTupleLabel6()
  // expected-error@-1 2 {{cannot use label with pack expansion tuple element}}

  let _: (Int, x: String, repeat each T, y: Float, repeat each T) = returnTupleLabel6()
}

func genericReturnTupleInvalid<each T>(_: repeat each T) {
  let _: (x: repeat each T) = returnTuple1()
  // expected-error@-1 {{cannot use label with pack expansion tuple element}}

  let _: (x: Int, repeat each T) = returnTuple1()
  // expected-error@-1 {{'(repeat each T)' is not convertible to '(x: Int, repeat each T)', tuples have a different number of elements}}
  // expected-error@-2 {{generic parameter 'each T' could not be inferred}}

  let _: (Int, x: repeat each T) = returnTuple2()
  // expected-error@-1 {{cannot use label with pack expansion tuple element}}

  let _: (Int, x: String, repeat each T) = returnTuple2()
  // expected-error@-1 {{'(Int, repeat each T)' is not convertible to '(Int, x: String, repeat each T)', tuples have a different number of elements}}
  // expected-error@-2 {{generic parameter 'each T' could not be inferred}}

  let _: (y: repeat each T) = returnTupleLabel1()
  // expected-error@-1 {{cannot use label with pack expansion tuple element}}

  let _: (y: Int, repeat each T) = returnTupleLabel1() // error at declaration

  let _: (x: repeat each T) = returnTupleLabel2()
  // expected-error@-1 {{cannot use label with pack expansion tuple element}}

  let _: (Int, y: String, repeat each T) = returnTupleLabel2() // error at declaration

  let _: (repeat each T, y: Float) = returnTupleLabel3()
  // expected-error@-1 {{'(Int, repeat each T, y: Float)' is not convertible to '(repeat each T, y: Float)', tuples have a different number of elements}}
  // expected-error@-2 {{generic parameter 'each T' could not be inferred}}

  let _: (Int, String, repeat each T, x: Float) = returnTupleLabel3()
  // expected-error@-1 {{'(Int, repeat each T, y: Float)' is not convertible to '(Int, String, repeat each T, x: Float)', tuples have a different number of elements}}
  // expected-error@-2 {{generic parameter 'each T' could not be inferred}}

  let _: (repeat each T, y: Float) = returnTupleLabel4() // error at declaration

  let _: (Int, x: String, y: repeat each T) = returnTupleLabel4()
  // expected-error@-1 {{cannot use label with pack expansion tuple element}}

  let _: (Int, repeat each T, x: repeat each T) = returnTupleLabel5()
  // expected-error@-1 {{cannot use label with pack expansion tuple element}}

  let _: (repeat each T, y: Float, repeat each T) = returnTupleLabel5() // error at declaration

  let _: (repeat each T, y: Int) = returnTupleLabel6() // error at declaration
}

func returnFunction1<each T>() -> (repeat each T) -> () {}

func returnFunction2<each T>() -> (Int, repeat each T) -> () {} // expected-note {{in call to function 'returnFunction2()'}}

func returnFunction3<each T>() -> (repeat each T, Float) -> () {} // expected-note {{in call to function 'returnFunction3()'}}

func returnFunction4<each T>() -> (Int, repeat each T, Float) -> () {} // expected-note 2{{in call to function 'returnFunction4()'}}

func concreteReturnFunctionValid() {
  let _: () -> () = returnFunction1()
  let _: (Int) -> () = returnFunction1()
  let _: (String, Double) -> () = returnFunction1()

  let _: (Int) -> () = returnFunction2()
  let _: (Int, Int) -> () = returnFunction2()
  let _: (Int, String, Double) -> () = returnFunction2()

  let _: (Float) -> () = returnFunction3()
  let _: (Int, Float) -> () = returnFunction3()
  let _: (String, Double, Float) -> () = returnFunction3()

  let _: (Int, Float) -> () = returnFunction4()
  let _: (Int, Int, Float) -> () = returnFunction4()
  let _: (Int, String, Double, Float) -> () = returnFunction4()
}

func concreteReturnFunctionInvalid() {
  let _: () -> () = returnFunction2() // expected-error {{cannot convert value of type '(Int, repeat each T) -> ()' to specified type '() -> ()'}}
  // expected-error@-1 {{generic parameter 'each T' could not be inferred}}

  let _: (String) -> () = returnFunction2() // expected-error {{cannot convert value of type '(Int) -> ()' to specified type '(String) -> ()'}}
  let _: (String, Int) -> () = returnFunction2() // expected-error {{cannot convert value of type '(Int, Int) -> ()' to specified type '(String, Int) -> ()'}}

  let _: () -> () = returnFunction3() // expected-error {{cannot convert value of type '(repeat each T, Float) -> ()' to specified type '() -> ()'}}
  // expected-error@-1 {{generic parameter 'each T' could not be inferred}}

  let _: (Float, Int) -> () = returnFunction3() // expected-error {{cannot convert value of type '(Float, Float) -> ()' to specified type '(Float, Int) -> ()'}}
  let _: (Float, Double, String) -> () = returnFunction3() // expected-error {{cannot convert value of type '(Float, Double, Float) -> ()' to specified type '(Float, Double, String) -> ()'}}

  let _: () -> () = returnFunction4() // expected-error {{cannot convert value of type '(Int, repeat each T, Float) -> ()' to specified type '() -> ()'}}
  // expected-error@-1 {{generic parameter 'each T' could not be inferred}}

  let _: (Int) -> () = returnFunction4() // expected-error {{cannot convert value of type '(Int, repeat each T, Float) -> ()' to specified type '(Int) -> ()'}}
  // expected-error@-1 {{generic parameter 'each T' could not be inferred}}

  let _: (Float, Int) -> () = returnFunction4() // expected-error {{cannot convert value of type '(Int, Float) -> ()' to specified type '(Float, Int) -> ()'}}
}

func patternInstantiationTupleTest1<each T>() -> (repeat Array<each T>) {}
// expected-note@-1 {{in call to function 'patternInstantiationTupleTest1()'}}
func patternInstantiationTupleTest2<each T, each U>() -> (repeat Dictionary<each T, each U>) {}

func patternInstantiationFunctionTest1<each T>() -> (repeat Array<each T>) -> () {}
func patternInstantiationFunctionTest2<each T, each U>() -> (repeat Dictionary<each T, each U>) -> () {}

func patternInstantiationConcreteValid() {
  let _: () = patternInstantiationTupleTest1()
  let _: Array<Int> = patternInstantiationTupleTest1()
  let _: (Array<Int>, Array<String>) = patternInstantiationTupleTest1()
  let _: (Array<Int>, Array<String>, Array<Float>) = patternInstantiationTupleTest1()

  let _: () = patternInstantiationTupleTest2()
  let _: Dictionary<Int, String> = patternInstantiationTupleTest2()
  let _: (Dictionary<Int, String>, Dictionary<Float, Bool>) = patternInstantiationTupleTest2()
  let _: (Dictionary<Int, String>, Dictionary<Float, Bool>, Dictionary<Double, Character>) = patternInstantiationTupleTest2()

  let _: () -> () = patternInstantiationFunctionTest1()
  let _: (_: Array<Int>) -> () = patternInstantiationFunctionTest1()
  let _: (Array<Int>, Array<String>) -> () = patternInstantiationFunctionTest1()
  let _: (Array<Int>, Array<String>, Array<Float>) -> () = patternInstantiationFunctionTest1()

  let _: () -> () = patternInstantiationFunctionTest2()
  let _: (_: Dictionary<Int, String>) -> () = patternInstantiationFunctionTest2()
  let _: (Dictionary<Int, String>, Dictionary<Float, Bool>) -> () = patternInstantiationFunctionTest2()
  let _: (Dictionary<Int, String>, Dictionary<Float, Bool>, Dictionary<Double, Character>) -> () = patternInstantiationFunctionTest2()
}

func patternInstantiationConcreteInvalid() {
  let _: Set<Int> = patternInstantiationTupleTest1()
  // expected-error@-1 {{cannot convert value of type '(repeat Array<Pack{_}>)' to specified type 'Set<Int>'}}

  let _: (Array<Int>, Set<String>) = patternInstantiationTupleTest1() // expected-error {{'(repeat Array<Pack{Int, _}>)' is not convertible to '(Array<Int>, Set<String>)', tuples have a different number of elements}}
}

func patternInstantiationGenericValid<each T, each U>(t: repeat each T, u: repeat each U)
    where (repeat (each T, each U)): Any, repeat each T: Hashable {
  let _: (repeat Array<each T>) = patternInstantiationTupleTest1()
  let _: (repeat Array<each T>, Array<String>) = patternInstantiationTupleTest1()
  let _: (Array<String>, repeat Array<each T>) = patternInstantiationTupleTest1()
  let _: (Array<Int>, repeat Array<each T>, Array<Float>) = patternInstantiationTupleTest1()

  let _: (repeat Dictionary<each T, each U>) = patternInstantiationTupleTest2()
  let _: (repeat Dictionary<each T, each U>, Dictionary<Float, Bool>) = patternInstantiationTupleTest2()
  let _: (Dictionary<Int, String>, repeat Dictionary<each T, each U>) = patternInstantiationTupleTest2()
  let _: (Dictionary<Int, String>, repeat Dictionary<each T, each U>, Dictionary<Double, Character>) = patternInstantiationTupleTest2()

  let _: (repeat Array<each T>) -> () = patternInstantiationFunctionTest1()
  let _: (repeat Array<each T>, Array<String>) -> () = patternInstantiationFunctionTest1()
  let _: (Array<String>, repeat Array<each T>) -> () = patternInstantiationFunctionTest1()
  let _: (Array<Int>, repeat Array<each T>, Array<Float>) -> () = patternInstantiationFunctionTest1()

  let _: (repeat Dictionary<each T, each U>) -> () = patternInstantiationFunctionTest2()
  let _: (repeat Dictionary<each T, each U>, Dictionary<Float, Bool>) -> () = patternInstantiationFunctionTest2()
  let _: (Dictionary<Int, String>, repeat Dictionary<each T, each U>) -> () = patternInstantiationFunctionTest2()
  let _: (Dictionary<Int, String>, repeat Dictionary<each T, each U>, Dictionary<Double, Character>) -> () = patternInstantiationFunctionTest2()
}

func patternInstantiationGenericInvalid<each T: Hashable>(t: repeat each T) {
  let _: (repeat Set<each T>) = patternInstantiationTupleTest1() // expected-error {{cannot convert value of type '(repeat Array<each T>)' to specified type '(repeat Set<each T>)}}
  // expected-error@-1 {{generic parameter 'each T' could not be inferred}}

  let _: (repeat Array<each T>, Set<String>) = patternInstantiationTupleTest1() // expected-error {{'(repeat Array<Pack{repeat each T, _}>)' is not convertible to '(repeat Array<each T>, Set<String>)', tuples have a different number of elements}}
}

// rdar://107996926 - Vanishing metatype of tuple not supported
func test_one_element_tuple_vs_non_tuple_matching() {
  struct S {
    func test<each T>(_: (repeat each T).Type) -> (repeat each T) { fatalError() }
    func testVanishing<each T>(_: (Int, repeat each T)) {}
  }

  let _ = S().test(Int.self) // Ok
  let _: Int = S().test(Int.self) // Ok
  let _ = S().test((Int, String).self) // Ok
  let _ = S().testVanishing(42) // Ok

  do {
    struct V<T> {}

    func test<each T>(_: V<(repeat each T)>?) {}
    func test<each T>(_: V<(repeat each T)>.Type) {}

    test(V<Int>()) // Ok
    test(V<Int>.self) // Ok
  }
}
