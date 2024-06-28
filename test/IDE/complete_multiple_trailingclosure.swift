// RUN: %batch-code-completion

func globalFunc1(fn1: () -> Int, fn2: () -> String) {}
func testGlobalFunc() {
  globalFunc1()
    { 1 } #^GLOBALFUNC_SAMELINE^#
    #^GLOBALFUNC_NEWLINE^#
// GLOBALFUNC_SAMELINE: Begin completions, 2 items
// GLOBALFUNC_SAMELINE-DAG: Keyword[self]/CurrNominal:          .self[#Void#]; name=self
// GLOBALFUNC_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]: {#fn2: () -> String {|}#}[#() -> String#];

// GLOBALFUNC_NEWLINE: Begin completions
// FIXME-GLOBALFUNC_NEWLINE-DAG: Pattern/Local/Flair[ArgLabels]: {#fn2: () -> String {|}#}[#() -> String#];

  globalFunc1()
    { 1 } fn2: #^GLOBALFUNC_AFTERLABEL^#
// FIXME: Closure literal completion.
// GLOBALFUNC_AFTERLABEL-NOT: Begin completions
}

struct S {
  func member() {}
}
func globalFunc2(x: Int = 0, _ fn: () -> Void) -> S {}
func globalFunc3(x: Int, _ fn: () -> Void) -> S {}

func testNonFuncArg() {
  do {
    // Don't complete for a non-function default argument.
    globalFunc2 {} #^GLOBALFUNC2^#
    // GLOBALFUNC2:     Begin completions, 2 items
    // GLOBALFUNC2-DAG: Decl[InstanceMethod]/CurrNominal:   .member()[#Void#]; name=member()
    // GLOBALFUNC2-DAG: Keyword[self]/CurrNominal: .self[#S#]; name=self

    globalFunc2 {}
      .#^GLOBALFUNC2_DOT^#
    // GLOBALFUNC2_DOT:     Begin completions, 2 items
    // GLOBALFUNC2_DOT-DAG: Decl[InstanceMethod]/CurrNominal:   member()[#Void#]; name=member()
    // GLOBALFUNC2_DOT-DAG: Keyword[self]/CurrNominal: self[#S#]; name=self
  }
  do {
    globalFunc3 {} #^GLOBALFUNC3^#
    // GLOBALFUNC3:     Begin completions, 2 items
    // GLOBALFUNC3-DAG: Decl[InstanceMethod]/CurrNominal:   .member()[#Void#]; name=member()
    // GLOBALFUNC3-DAG: Keyword[self]/CurrNominal: .self[#S#]; name=self
    
    globalFunc3 {}
      .#^GLOBALFUNC3_DOT^#
    // GLOBALFUNC3_DOT:     Begin completions, 2 items
    // GLOBALFUNC3_DOT-DAG: Decl[InstanceMethod]/CurrNominal:   member()[#Void#]; name=member()
    // GLOBALFUNC3_DOT-DAG: Keyword[self]/CurrNominal: self[#S#]; name=self
  }
}

struct SimpleEnum {
  case foo, bar

  func enumFunc() {}
  static func + (lhs: SimpleEnum, rhs: SimpleEnum) -> SimpleEnum {}
}

struct MyStruct {
  func method1(fn1: () -> Int, fn2: (() -> String)? = nil) -> SimpleEnum {}
  func method1(fn1: () -> Int, fn2: Int = nil) -> SimpleEnum {}
}
func testMethod(value: MyStruct) {
  value.method1 {
  } #^METHOD_SAMELINE^#
  #^METHOD_NEWLINE^#
// METHOD_SAMELINE: Begin completions, 4 items
// METHOD_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]:     {#fn2: (() -> String)? {|}#}[#(() -> String)?#];
// METHOD_SAMELINE-DAG: Decl[InstanceMethod]/CurrNominal:   .enumFunc()[#Void#];
// METHOD_SAMELINE-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: [' ']+ {#SimpleEnum#}[#SimpleEnum#];
// METHOD_SAMELINE-DAG: Keyword[self]/CurrNominal:          .self[#SimpleEnum#];

// FIXME-METHOD_NEWLINE-DAG: Pattern/Local/Flair[ArgLabels]: {#fn2: (() -> String)? {|}#}[#(() -> String)?#];
// METHOD_NEWLINE-DAG: Keyword[class]/None/Flair[RareKeyword]: class;
// METHOD_NEWLINE-DAG: Keyword[if]/None:                   if;
// METHOD_NEWLINE-DAG: Keyword[try]/None:                  try;
// METHOD_NEWLINE-DAG: Decl[LocalVar]/Local:               value[#MyStruct#]; name=value
}

struct TestStruct {
  init(fn1: () -> Int, fn2: () -> String, fn3: () -> String) {}
  init(fn1: () -> Int) {}
  init(fn1: () -> Int, fn2: () -> String) {}
  init(fn1: () -> Int, fn3: () -> String) {}

  func testStructMethod() {}
}

func testOverloadedInit() {
  TestStruct {
    1
  } #^INIT_OVERLOADED_SAMELINE^#
  #^INIT_OVERLOADED_NEWLINE^#

// INIT_OVERLOADED_SAMELINE: Begin completions, 4 items
// INIT_OVERLOADED_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]:     {#fn2: () -> String {|}#}[#() -> String#];
// INIT_OVERLOADED_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]:     {#fn3: () -> String {|}#}[#() -> String#];
// INIT_OVERLOADED_SAMELINE-DAG: Decl[InstanceMethod]/CurrNominal:   .testStructMethod()[#Void#];
// INIT_OVERLOADED_SAMELINE-DAG: Keyword[self]/CurrNominal:          .self[#TestStruct#];

// FIXME-INIT_OVERLOADED_NEWLINE-DAG: Pattern/Local/Flair[ArgLabels]: {#fn2: () -> String {|}#}[#() -> String#];
// FIXME-INIT_OVERLOADED_NEWLINE-DAG: Pattern/Local/Flair[ArgLabels]: {#fn3: () -> String {|}#}[#() -> String#];
// INIT_OVERLOADED_NEWLINE-DAG: Keyword[class]/None/Flair[RareKeyword]: class;
// INIT_OVERLOADED_NEWLINE-DAG: Keyword[if]/None:                   if;
// INIT_OVERLOADED_NEWLINE-DAG: Keyword[try]/None:                  try;
// INIT_OVERLOADED_NEWLINE-DAG: Decl[Struct]/CurrModule:            MyStruct[#MyStruct#]; name=MyStruct
}

struct TestStruct2 {
  init(fn1: () -> Int, fn2: () -> String = {}, fn3: () -> String = {}) {}
  func testStructMethod() {}
}
func testOptionalInit() {
  TestStruct2 {
    2
  } #^INIT_OPTIONAL_SAMELINE^#
  #^INIT_OPTIONAL_NEWLINE^#

// INIT_OPTIONAL_SAMELINE: Begin completions, 4 items
// INIT_OPTIONAL_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]:     {#fn2: () -> String {|}#}[#() -> String#];
// INIT_OPTIONAL_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]:     {#fn3: () -> String {|}#}[#() -> String#];
// INIT_OPTIONAL_SAMELINE-DAG: Decl[InstanceMethod]/CurrNominal:   .testStructMethod()[#Void#];
// INIT_OPTIONAL_SAMELINE-DAG: Keyword[self]/CurrNominal:          .self[#TestStruct2#];

// FIXME-INIT_OPTIONAL_NEWLINE-DAG: Pattern/Local/Flair[ArgLabels]: {#fn2: () -> String {|}#}[#() -> String#];
// FIXME-INIT_OPTIONAL_NEWLINE-DAG: Pattern/Local/Flair[ArgLabels]: {#fn3: () -> String {|}#}[#() -> String#];
// INIT_OPTIONAL_NEWLINE-DAG: Keyword[class]/None/Flair[RareKeyword]: class;
// INIT_OPTIONAL_NEWLINE-DAG: Keyword[if]/None:                   if;
// INIT_OPTIONAL_NEWLINE-DAG: Keyword[try]/None:                  try;
// INIT_OPTIONAL_NEWLINE-DAG: Decl[Struct]/CurrModule:            MyStruct[#MyStruct#]; name=MyStruct
}

func testOptionalInitDot() {
  // When there's a dot, we don't complete for the closure argument.
  TestStruct2 {
    2
  }
  .#^INIT_OPTIONAL_DOT^#
  // INIT_OPTIONAL_DOT:     Begin completions, 2 items
  // INIT_OPTIONAL_DOT-DAG: Keyword[self]/CurrNominal:          self[#TestStruct2#]; name=self
  // INIT_OPTIONAL_DOT-DAG: Decl[InstanceMethod]/CurrNominal:   testStructMethod()[#Void#]; name=testStructMethod()
}

struct TestStruct3 {
  init(fn1: () -> Int, fn2: () -> String, fn3: () -> String) {}
  func testStructMethod() {}
}
func testOptionalInit() {
  // missing 'fn2' and 'fn3'.
  TestStruct3 {
    2
  } #^INIT_REQUIRED_SAMELINE_1^#
  #^INIT_REQUIRED_NEWLINE_1^#

// INIT_REQUIRED_SAMELINE_1: Begin completions, 3 items
// INIT_REQUIRED_SAMELINE_1-DAG: Pattern/Local/Flair[ArgLabels]:     {#fn2: () -> String {|}#}[#() -> String#];
// INIT_REQUIRED_SAMELINE_1-DAG: Keyword[self]/CurrNominal:          .self[#TestStruct3#]; name=self
// INIT_REQUIRED_SAMELINE_1-DAG: Decl[InstanceMethod]/CurrNominal:   .testStructMethod()[#Void#]; name=testStructMethod()


// INIT_REQUIRED_NEWLINE_1: Begin completions
// FIXME-INIT_REQUIRED_NEWLINE_1-DAG: Pattern/Local/Flair[ArgLabels]:               {#fn2: () -> String {|}#}[#() -> String#];
// INIT_REQUIRED_NEWLINE_1: End completions

  // missing 'fn3'.
  TestStruct3 {
    2
  } fn2: {
    "test"
  } #^INIT_REQUIRED_SAMELINE_2^#
  #^INIT_REQUIRED_NEWLINE_2^#

// INIT_REQUIRED_SAMELINE_2: Begin completions, 3 items
// INIT_REQUIRED_SAMELINE_2-DAG: Keyword[self]/CurrNominal:          .self[#TestStruct3#]; name=self
// INIT_REQUIRED_SAMELINE_2-DAG: Decl[InstanceMethod]/CurrNominal:   .testStructMethod()[#Void#]; name=testStructMethod()
// INIT_REQUIRED_SAMELINE_2-DAG: Pattern/Local/Flair[ArgLabels]:     {#fn3: () -> String {|}#}[#() -> String#];

// INIT_REQUIRED_NEWLINE_2: Begin completions
// FIXME-INIT_REQUIRED_NEWLINE_2-DAG: Pattern/Local/Flair[ArgLabels]:               {#fn3: () -> String {|}#}[#() -> String#];
// INIT_REQUIRED_NEWLINE_2: End completions

  // Call is completed.
  TestStruct3 {
    2
  } fn2: {
    "test"
  } fn3: {
    "test"
  } #^INIT_REQUIRED_SAMELINE_3^#
  #^INIT_REQUIRED_NEWLINE_3^#

// INIT_REQUIRED_SAMELINE_3: Begin completions
// INIT_REQUIRED_SAMELINE_3-DAG: Decl[InstanceMethod]/CurrNominal:   .testStructMethod()[#Void#];
// INIT_REQUIRED_SAMELINE_3-DAG: Keyword[self]/CurrNominal:          .self[#TestStruct3#];

// INIT_REQUIRED_NEWLINE_3-NOT: name=fn2
// INIT_REQUIRED_NEWLINE_3-NOT: name=fn3
// INIT_REQUIRED_NEWLINE_3-DAG: Keyword[class]/None/Flair[RareKeyword]: class;
// INIT_REQUIRED_NEWLINE_3-DAG: Keyword[if]/None:                   if;
// INIT_REQUIRED_NEWLINE_3-DAG: Keyword[try]/None:                  try;
// INIT_REQUIRED_NEWLINE_3-DAG: Decl[Struct]/CurrModule:            MyStruct[#MyStruct#]; name=MyStruct
// INIT_REQUIRED_NEWLINE_3-NOT: name=fn2
// INIT_REQUIRED_NEWLINE_3-NOT: name=fn3
}

struct MyStruct4<T> {
  init(arg1: Int = 0, arg2: () -> T) {}
  init(name: String, arg2: () -> String, arg3: () -> T) {}

  func testStructMethod() {}
}
func testFallbackPostfix() {
  let _ = MyStruct4 {
    1
  } #^INIT_FALLBACK_1^#
// INIT_FALLBACK_1: Begin completions, 2 items
// INIT_FALLBACK_1-DAG: Keyword[self]/CurrNominal:          .self[#MyStruct4<Int>#]; name=self
// INIT_FALLBACK_1-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: .testStructMethod()[#Void#]; name=testStructMethod()
  let _ = MyStruct4(name: "test") {
    ""
  } arg3: {
    1
  } #^INIT_FALLBACK_2^#
// INIT_FALLBACK_2: Begin completions
// INIT_FALLBACK_2-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]:   .testStructMethod()[#Void#];
// INIT_FALLBACK_2-DAG: Keyword[self]/CurrNominal:          .self[#MyStruct4<Int>#];
}

protocol P {
  func foo()
}
struct TestNominalMember: P {
  var value = MyStruct().method1 { 1 } #^MEMBERDECL_SAMELINE^#
  #^MEMBERDECL_NEWLINE^#

// MEMBERDECL_SAMELINE: Begin completions, 4 items
// MEMBERDECL_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]:     {#fn2: (() -> String)? {|}#}[#(() -> String)?#]; name=fn2:
// MEMBERDECL_SAMELINE-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]:   .enumFunc()[#Void#]; name=enumFunc()
// MEMBERDECL_SAMELINE-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: [' ']+ {#SimpleEnum#}[#SimpleEnum#]; name=+ 
// MEMBERDECL_SAMELINE-DAG: Keyword[self]/CurrNominal:          .self[#SimpleEnum#]; name=self

// FIXME-MEMBERDECL_NEWLINE-DAG: Pattern/Local/Flair[ArgLabels]: {#fn2: (() -> String)? {|}#}[#(() -> String)?#]; name=fn2:
// MEMBERDECL_NEWLINE-DAG: Keyword[enum]/None:                 enum; name=enum
// MEMBERDECL_NEWLINE-DAG: Keyword[func]/None:                 func; name=func
// MEMBERDECL_NEWLINE-DAG: Keyword[private]/None:              private; name=private
// MEMBERDECL_NEWLINE-DAG: Keyword/None:                       lazy; name=lazy
// MEMBERDECL_NEWLINE-DAG: Keyword[var]/None:                  var; name=var
// MEMBERDECL_NEWLINE-DAG: Decl[InstanceMethod]/Super:         func foo() {|}; name=foo()
}

func testInitializedVarDecl() {
    let localVal = TestStruct {
        1
    } #^INITIALIZED_VARDECL_SAMELINE^#
    #^INITIALIZED_VARDECL_NEWLINE^#
// INITIALIZED_VARDECL_SAMELINE: Begin completions, 4 items
// INITIALIZED_VARDECL_SAMELINE-NOT: localVal
// INITIALIZED_VARDECL_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]:     {#fn2: () -> String {|}#}[#() -> String#];
// INITIALIZED_VARDECL_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]:     {#fn3: () -> String {|}#}[#() -> String#];
// INITIALIZED_VARDECL_SAMELINE-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]:   .testStructMethod()[#Void#];
// INITIALIZED_VARDECL_SAMELINE-DAG: Keyword[self]/CurrNominal:          .self[#TestStruct#];
// INITIALIZED_VARDECL_SAMELINE-NOT: localVal

// INITIALIZED_VARDECL_NEWLINE-DAG: Decl[LocalVar]/Local:               localVal[#TestStruct#];
}
