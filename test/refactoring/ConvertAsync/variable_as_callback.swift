// REQUIRES: concurrency

// RUN: %empty-directory(%t)

enum CustomError: Error {
  case invalid
  case insecure
}

typealias SomeCallback = (String) -> Void

func simple(completion: @escaping (String) -> Void) { }
func simple() async -> String { }

func simpleWithArg(a: Int, completion: @escaping (String) -> Void) { }
func simpleWithArg(a: Int) async -> String { }

func multipleResults(completion: @escaping (String, Int) -> Void) { }
func multipleResults() async -> (String, Int) { }

func nonOptionalError(completion: @escaping (String, Error) -> Void) { }
func nonOptionalError() async -> (String, Error) { }

func noParams(completion: @escaping () -> Void) { }
func noParams() async { }

func error(completion: @escaping (String?, Error?) -> Void) { }
func error() async throws -> String { }

func errorOnly(completion: @escaping (Error?) -> Void) { }
func errorOnly() async throws { }

func errorNonOptionalResult(completion: @escaping (String, Error?) -> Void) { }
func errorNonOptionalResult() async throws -> String { }

func alias(completion: @escaping SomeCallback) { }
func alias() async -> String { }

func simpleResult(completion: @escaping (Result<String, Never>) -> Void) { }
func simpleResult() async -> String { }

func errorResult(completion: @escaping (Result<String, Error>) -> Void) { }
func errorResult() async throws -> String { }

func customErrorResult(completion: @escaping (Result<String, CustomError>) -> Void) { }
func customErrorResult() async throws -> String { }

func optionalSingle(completion: @escaping (String?) -> Void) { }
func optionalSingle() async -> String? { }

func manyOptional(_ completion: @escaping (String?, Int?) -> Void) { }
func manyOptional() async -> (String?, Int?) { }

func generic<T, R>(completion: @escaping (T, R) -> Void) { }
func generic<T, R>() async -> (T, R) { }

func genericResult<T>(completion: @escaping (T?, Error?) -> Void) where T: Numeric { }
func genericResult<T>() async throws -> T where T: Numeric { }

func genericError<E>(completion: @escaping (String?, E?) -> Void) where E: Error { }
func genericError() async throws -> String { }

func defaultArgs(a: Int, b: Int = 10, completion: @escaping (String) -> Void) { }
func defaultArgs(a: Int, b: Int = 10) async -> String { }


// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=SIMPLE-WITH-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=SIMPLE-WITH %s
func testSimpleWithVariableCompletionHandler(completionHandler: @escaping (String) -> Void) {
  simple(completion: completionHandler)
}
// SIMPLE-WITH-FUNC: func testSimpleWithVariableCompletionHandler() async -> String {
// SIMPLE-WITH-FUNC-NEXT:  return await simple()
// SIMPLE-WITH-FUNC-NEXT: }

// SIMPLE-WITH: let result = await simple()
// SIMPLE-WITH-NEXT: completionHandler(result)

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=SIMPLE-WITH-ARG-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=SIMPLE-WITH-ARG %s
func testSimpleWithArgVariableCompletionHandler(b: Int, completionHandler: @escaping (String) -> Void) {
  simpleWithArg(a: b, completion: completionHandler)
}
// SIMPLE-WITH-ARG-FUNC: func testSimpleWithArgVariableCompletionHandler(b: Int) async -> String {
// SIMPLE-WITH-ARG-FUNC-NEXT:  return await simpleWithArg(a: b)
// SIMPLE-WITH-ARG-FUNC-NEXT: }

// SIMPLE-WITH-ARG: let result = await simpleWithArg(a: b)
// SIMPLE-WITH-ARG-NEXT: completionHandler(result)

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=SIMPLE-WITH-CONSTANT-ARG-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=SIMPLE-WITH-CONSTANT-ARG %s
func testSimpleWithConstantArgVariableCompletionHandler(completionHandler: @escaping (String) -> Void) {
  simpleWithArg(a: 1, completion: completionHandler)
}
// SIMPLE-WITH-CONSTANT-ARG-FUNC: func testSimpleWithConstantArgVariableCompletionHandler() async -> String {
// SIMPLE-WITH-CONSTANT-ARG-FUNC-NEXT:  return await simpleWithArg(a: 1)
// SIMPLE-WITH-CONSTANT-ARG-FUNC-NEXT: }

// SIMPLE-WITH-CONSTANT-ARG: let result = await simpleWithArg(a: 1)
// SIMPLE-WITH-CONSTANT-ARG-NEXT: completionHandler(result)

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=MULTIPLE-RESULTS-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=MULTIPLE-RESULTS %s
func testMultipleResultsVariableCompletionHandler(completionHandler: @escaping (String, Int) -> Void) {
  multipleResults(completion: completionHandler)
}
// MULTIPLE-RESULTS-FUNC: func testMultipleResultsVariableCompletionHandler() async -> (String, Int) {
// MULTIPLE-RESULTS-FUNC-NEXT:  return await multipleResults()
// MULTIPLE-RESULTS-FUNC-NEXT: }

// MULTIPLE-RESULTS: let result = await multipleResults()
// MULTIPLE-RESULTS-NEXT: completionHandler(result.0, result.1)

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=NON-OPTIONAL-ERROR-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=NON-OPTIONAL-ERROR %s
func testNonOptionalErrorVariableCompletionHandler(completionHandler: @escaping (String, Error) -> Void) {
  nonOptionalError(completion: completionHandler)
}
// NON-OPTIONAL-ERROR-FUNC: func testNonOptionalErrorVariableCompletionHandler() async -> (String, any Error) {
// NON-OPTIONAL-ERROR-FUNC-NEXT:  return await nonOptionalError()
// NON-OPTIONAL-ERROR-FUNC-NEXT: }

// NON-OPTIONAL-ERROR: let result = await nonOptionalError()
// NON-OPTIONAL-ERROR-NEXT: completionHandler(result.0, result.1)

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=NO-PARAMS-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=NO-PARAMS %s
func testNoParamsVariableCompletionHandler(completionHandler: @escaping () -> Void) {
  noParams(completion: completionHandler)
}
// NO-PARAMS-FUNC: func testNoParamsVariableCompletionHandler() async {
// NO-PARAMS-FUNC-NOT: return
// NO-PARAMS-FUNC-NEXT:  await noParams()
// NO-PARAMS-FUNC-NEXT: }

// NO-PARAMS: await noParams()
// NO-PARAMS-NEXT: completionHandler()

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=ERROR-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=ERROR %s
func testErrorWithVariableCompletionHandler(completionHandler: @escaping (String?, Error?) -> Void) {
  error(completion: completionHandler)
}
// ERROR-FUNC: func testErrorWithVariableCompletionHandler() async throws -> String {
// ERROR-FUNC-NEXT:  return try await error()
// ERROR-FUNC-NEXT: }

// ERROR: do {
// ERROR-NEXT:   let result = try await error()
// ERROR-NEXT:   completionHandler(result, nil)
// ERROR-NEXT: } catch {
// ERROR-NEXT:   completionHandler(nil, error)
// ERROR-NEXT: }

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=ERROR-ONLY-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=ERROR-ONLY %s
func testErrorOnlyWithVariableCompletionHandler(completionHandler: @escaping (Error?) -> Void) {
  errorOnly(completion: completionHandler)
}
// ERROR-ONLY-FUNC: func testErrorOnlyWithVariableCompletionHandler() async throws {
// ERROR-ONLY-FUNC-NOT:   return
// ERROR-ONLY-FUNC-NEXT:   try await errorOnly()
// ERROR-ONLY-FUNC-NEXT: }

// ERROR-ONLY: do {
// ERROR-ONLY-NEXT:   try await errorOnly()
// ERROR-ONLY-NEXT:   completionHandler(nil)
// ERROR-ONLY-NEXT: } catch {
// ERROR-ONLY-NEXT:   completionHandler(error)
// ERROR-ONLY-NEXT: }

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1  | %FileCheck -check-prefix=ERROR-NON-OPTIONAL-RESULT-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3  | %FileCheck -check-prefix=ERROR-NON-OPTIONAL-RESULT %s
func testErrorNonOptionalResultWithVariableCompletionHandler(completionHandler: @escaping (String, Error?) -> Void) {
  errorNonOptionalResult(completion: completionHandler)
}
// ERROR-NON-OPTIONAL-RESULT-FUNC: func testErrorNonOptionalResultWithVariableCompletionHandler() async throws -> String {
// ERROR-NON-OPTIONAL-RESULT-FUNC-NEXT:  return try await errorNonOptionalResult()
// ERROR-NON-OPTIONAL-RESULT-FUNC-NEXT: }

// ERROR-NON-OPTIONAL-RESULT: do {
// ERROR-NON-OPTIONAL-RESULT-NEXT: let result = try await errorNonOptionalResult()
// ERROR-NON-OPTIONAL-RESULT-NEXT: completionHandler(result, nil)
// ERROR-NON-OPTIONAL-RESULT-NEXT: } catch {
// ERROR-NON-OPTIONAL-RESULT-NEXT: completionHandler(<#String#>, error)
// ERROR-NON-OPTIONAL-RESULT-NEXT: }

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=ALIAS-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=ALIAS %s
func testAliasWithVariableCompletionHandler(completionHandler: @escaping SomeCallback) {
  alias(completion: completionHandler)
}
// ALIAS-FUNC: func testAliasWithVariableCompletionHandler() async -> String {
// ALIAS-FUNC-NEXT:  return await alias()
// ALIAS-FUNC-NEXT: }

// ALIAS: let result = await alias()
// ALIAS-NEXT: completionHandler(result)

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=SIMPLE-RESULT-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=SIMPLE-RESULT %s
func testSimpleResultVariableCompletionHandler(completionHandler: @escaping (Result<String, Never>) -> Void) {
  simpleResult(completion: completionHandler)
}
// SIMPLE-RESULT-FUNC: func testSimpleResultVariableCompletionHandler() async -> String {
// SIMPLE-RESULT-FUNC-NEXT:  return await simpleResult()
// SIMPLE-RESULT-FUNC-NEXT: }

// SIMPLE-RESULT: let result = await simpleResult()
// SIMPLE-RESULT-NEXT: completionHandler(.success(result))

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=ERROR-RESULT-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=ERROR-RESULT %s
func testErrorResultVariableCompletionHandler(completionHandler: @escaping (Result<String, Error>) -> Void) {
  errorResult(completion: completionHandler)
}
// ERROR-RESULT-FUNC: func testErrorResultVariableCompletionHandler() async throws -> String {
// ERROR-RESULT-FUNC-NEXT:  return try await errorResult()
// ERROR-RESULT-FUNC-NEXT: }

// ERROR-RESULT: do {
// ERROR-RESULT-NEXT:   let result = try await errorResult()
// ERROR-RESULT-NEXT:   completionHandler(.success(result))
// ERROR-RESULT-NEXT: } catch {
// ERROR-RESULT-NEXT:   completionHandler(.failure(error))
// ERROR-RESULT-NEXT: }

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=CUSTOM-ERROR-RESULT-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=CUSTOM-ERROR-RESULT %s
func testErrorResultVariableCompletionHandler(completionHandler: @escaping (Result<String, CustomError>) -> Void) {
  customErrorResult(completion: completionHandler)
}
// CUSTOM-ERROR-RESULT-FUNC: func testErrorResultVariableCompletionHandler() async throws -> String {
// CUSTOM-ERROR-RESULT-FUNC-NEXT:  return try await customErrorResult()
// CUSTOM-ERROR-RESULT-FUNC-NEXT: }

// CUSTOM-ERROR-RESULT: do {
// CUSTOM-ERROR-RESULT-NEXT:   let result = try await customErrorResult()
// CUSTOM-ERROR-RESULT-NEXT:   completionHandler(.success(result))
// CUSTOM-ERROR-RESULT-NEXT: } catch {
// CUSTOM-ERROR-RESULT-NEXT:   completionHandler(.failure(error as! CustomError))
// CUSTOM-ERROR-RESULT-NEXT: }

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=OPTIONAL-SINGLE-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=OPTIONAL-SINGLE %s
func testOptionalSingleVariableCompletionHandler(completionHandler: @escaping (String?) -> Void) {
  optionalSingle(completion: completionHandler)
}
// OPTIONAL-SINGLE-FUNC: func testOptionalSingleVariableCompletionHandler() async -> String? {
// OPTIONAL-SINGLE-FUNC-NEXT:  return await optionalSingle()
// OPTIONAL-SINGLE-FUNC-NEXT: }

// OPTIONAL-SINGLE: let result = await optionalSingle()
// OPTIONAL-SINGLE-NEXT: completionHandler(result)

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=MANY-OPTIONAL-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=MANY-OPTIONAL %s
func testManyOptionalVariableCompletionHandler(completionHandler: @escaping (String?, Int?) -> Void) {
  manyOptional(completionHandler)
}
// MANY-OPTIONAL-FUNC: func testManyOptionalVariableCompletionHandler() async -> (String?, Int?) {
// MANY-OPTIONAL-FUNC-NEXT:  return await manyOptional()
// MANY-OPTIONAL-FUNC-NEXT: }

// MANY-OPTIONAL: let result = await manyOptional()
// MANY-OPTIONAL-NEXT: completionHandler(result.0, result.1)

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=GENERIC-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=GENERIC %s
func testGenericVariableCompletionHandler<T, R>(completionHandler: @escaping (T, R) -> Void) {
  generic(completion: completionHandler)
}
// GENERIC-FUNC: func testGenericVariableCompletionHandler<T, R>() async -> (T, R) {
// GENERIC-FUNC-NEXT:  return await generic()
// GENERIC-FUNC-NEXT: }

// GENERIC: let result: (T, R) = await generic()
// GENERIC-NEXT: completionHandler(result.0, result.1)

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=SPECIALIZE-GENERIC-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=SPECIALIZE-GENERIC %s
func testSpecializeGenericsVariableCompletionHandler(completionHandler: @escaping (String, Int) -> Void) {
  generic(completion: completionHandler)
}
// SPECIALIZE-GENERIC-FUNC: func testSpecializeGenericsVariableCompletionHandler() async -> (String, Int) {
// SPECIALIZE-GENERIC-FUNC-NEXT:  return await generic()
// SPECIALIZE-GENERIC-FUNC-NEXT: }

// SPECIALIZE-GENERIC: let result: (String, Int) = await generic()
// SPECIALIZE-GENERIC-NEXT: completionHandler(result.0, result.1)

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=GENERIC-RESULT-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=GENERIC-RESULT %s
func testGenericResultVariableCompletionHandler<T>(completionHandler: @escaping (T?, Error?) -> Void) where T: Numeric {
  genericResult(completion: completionHandler)
}
// GENERIC-RESULT-FUNC: func testGenericResultVariableCompletionHandler<T>() async throws -> T where T: Numeric {
// GENERIC-RESULT-FUNC-NEXT:  return try await genericResult()
// GENERIC-RESULT-FUNC-NEXT: }

// GENERIC-RESULT: do {
// GENERIC-RESULT-NEXT:   let result: T = try await genericResult()
// GENERIC-RESULT-NEXT:   completionHandler(result, nil)
// GENERIC-RESULT-NEXT: } catch {
// GENERIC-RESULT-NEXT:   completionHandler(nil, error)
// GENERIC-RESULT-NEXT: }

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=GENERIC-ERROR-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=GENERIC-ERROR %s
func testGenericErrorVariableCompletionHandler<MyGenericError>(completionHandler: @escaping (String?, MyGenericError?) -> Void) where MyGenericError: Error {
  genericError(completion: completionHandler)
}
// GENERIC-ERROR-FUNC: func testGenericErrorVariableCompletionHandler<MyGenericError>() async throws -> String where MyGenericError: Error {
// GENERIC-ERROR-FUNC-NEXT:  return try await genericError()
// GENERIC-ERROR-FUNC-NEXT: }

// GENERIC-ERROR: do {
// GENERIC-ERROR-NEXT:   let result: String = try await genericError()
// GENERIC-ERROR-NEXT:   completionHandler(result, nil)
// GENERIC-ERROR-NEXT: } catch {
// GENERIC-ERROR-NEXT:   completionHandler(nil, (error as! MyGenericError))
// GENERIC-ERROR-NEXT: }

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=DEFAULT-ARGS-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=DEFAULT-ARGS %s
func testDefaultArgsVariableCompletionHandler(completionHandler: @escaping (String) -> Void) {
  defaultArgs(a: 5, completion: completionHandler)
}
// DEFAULT-ARGS-FUNC: func testDefaultArgsVariableCompletionHandler() async -> String {
// DEFAULT-ARGS-FUNC-NEXT:  return await defaultArgs(a: 5)
// DEFAULT-ARGS-FUNC-NEXT: }

// DEFAULT-ARGS: let result = await defaultArgs(a: 5)
// DEFAULT-ARGS-NEXT: completionHandler(result)

func myPrint(_ message: String) {
  print(message)
}

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=GLOBAL-FUNC-AS-COMPLETION-HANDLER-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=GLOBAL-FUNC-AS-COMPLETION-HANDLER %s
func testGlobalFuncAsCompletionHandler() {
  simple(completion: myPrint)
}
// GLOBAL-FUNC-AS-COMPLETION-HANDLER-FUNC: func testGlobalFuncAsCompletionHandler() async {
// GLOBAL-FUNC-AS-COMPLETION-HANDLER-FUNC-NEXT:  let result = await simple()
// GLOBAL-FUNC-AS-COMPLETION-HANDLER-FUNC-NEXT:  myPrint(result)
// GLOBAL-FUNC-AS-COMPLETION-HANDLER-FUNC-NEXT: }

// GLOBAL-FUNC-AS-COMPLETION-HANDLER: let result = await simple()
// GLOBAL-FUNC-AS-COMPLETION-HANDLER-NEXT: myPrint(result)

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):1 | %FileCheck -check-prefix=VARIABLE-AS-COMPLETION-HANDLER-FUNC %s
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+3):3 | %FileCheck -check-prefix=VARIABLE-AS-COMPLETION-HANDLER %s
func testVariableAsCompletionHandler() {
  let complete: (String) -> Void = { print($0) }
  simple(completion: complete)
}
// VARIABLE-AS-COMPLETION-HANDLER-FUNC: func testVariableAsCompletionHandler() async {
// VARIABLE-AS-COMPLETION-HANDLER-FUNC-NEXT: let complete: (String) -> Void = { print($0) }
// VARIABLE-AS-COMPLETION-HANDLER-FUNC-NEXT:  let result = await simple()
// VARIABLE-AS-COMPLETION-HANDLER-FUNC-NEXT:  complete(result)
// VARIABLE-AS-COMPLETION-HANDLER-FUNC-NEXT: }

// VARIABLE-AS-COMPLETION-HANDLER: let result = await simple()
// VARIABLE-AS-COMPLETION-HANDLER-NEXT: complete(result)

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=PRINTING-WRAPPER-FUNC %s
func testPrintingWrapper(completionHandler: @escaping (String) -> Void) {
  print("Starting")
  simple(completion: completionHandler)
  print("Operation scheduled")
}
// PRINTING-WRAPPER-FUNC: func testPrintingWrapper() async -> String {
// PRINTING-WRAPPER-FUNC-NEXT:   print("Starting")
// PRINTING-WRAPPER-FUNC-NEXT:   return await simple()
// PRINTING-WRAPPER-FUNC-NEXT:   print("Operation scheduled")
// PRINTING-WRAPPER-FUNC-NEXT: }

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=SHADOWING-BEFORE %s
func testShadowingBefore() {
  let complete: (String) -> Void = { print($0) }
  let result = 1
  simple(completion: complete)
}
// SHADOWING-BEFORE: func testShadowingBefore() async {
// SHADOWING-BEFORE-NEXT: let complete: (String) -> Void = { print($0) }
// SHADOWING-BEFORE-NEXT: let result = 1
// SHADOWING-BEFORE-NEXT: let result1 = await simple()
// SHADOWING-BEFORE-NEXT: complete(result1)

// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=SHADOWING-AFTER %s
func testShadowingAfter() {
  let complete: (String) -> Void = { print($0) }
  simple(completion: complete)
  let result = 1
}
// SHADOWING-AFTER: func testShadowingAfter() async {
// SHADOWING-AFTER-NEXT: let complete: (String) -> Void = { print($0) }
// SHADOWING-AFTER-NEXT: let result = await simple()
// SHADOWING-AFTER-NEXT: complete(result)
// SHADOWING-AFTER-NEXT: let result1 = 1

class Foo {
  var foo: Foo

  init(foo: Foo) {
    self.foo = foo
  }

  func myFooPrint(_ message: String) {
    print("FOO: \(message)")
  }

  // RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=MEMBER-FUNC-AS-COMPLETION-HANDLER-FUNC %s
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):5 | %FileCheck -check-prefix=MEMBER-FUNC-AS-COMPLETION-HANDLER %s
  func testMethodAsCompletionHandler() {
    simple(completion: myFooPrint)
  }
  // MEMBER-FUNC-AS-COMPLETION-HANDLER-FUNC: func testMethodAsCompletionHandler() async {
  // MEMBER-FUNC-AS-COMPLETION-HANDLER-FUNC-NEXT:   let result = await simple()
  // MEMBER-FUNC-AS-COMPLETION-HANDLER-FUNC-NEXT:   myFooPrint(result)
  // MEMBER-FUNC-AS-COMPLETION-HANDLER-FUNC-NEXT: }

  // MEMBER-FUNC-AS-COMPLETION-HANDLER: let result = await simple()
  // MEMBER-FUNC-AS-COMPLETION-HANDLER-NEXT: myFooPrint(result)

  // RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=MEMBER-FUNC-ON-OTHER-OBJECT-AS-COMPLETION-HANDLER-FUNC %s
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):5 | %FileCheck -check-prefix=MEMBER-FUNC-ON-OTHER-OBJECT-AS-COMPLETION-HANDLER %s
  func testMethodOnOtherObjectAsCompletionHandler(foo: Foo) {
    simple(completion: foo.myFooPrint)
  }
  // MEMBER-FUNC-ON-OTHER-OBJECT-AS-COMPLETION-HANDLER-FUNC: func testMethodOnOtherObjectAsCompletionHandler(foo: Foo) async {
  // MEMBER-FUNC-ON-OTHER-OBJECT-AS-COMPLETION-HANDLER-FUNC-NEXT:   let result = await simple()
  // MEMBER-FUNC-ON-OTHER-OBJECT-AS-COMPLETION-HANDLER-FUNC-NEXT:   foo.myFooPrint(result)
  // MEMBER-FUNC-ON-OTHER-OBJECT-AS-COMPLETION-HANDLER-FUNC-NEXT: }

  // MEMBER-FUNC-ON-OTHER-OBJECT-AS-COMPLETION-HANDLER: let result = await simple()
  // MEMBER-FUNC-ON-OTHER-OBJECT-AS-COMPLETION-HANDLER-NEXT: foo.myFooPrint(result)

  // RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=MEMBER-FUNC-ON-NESTED-OTHER-OBJECT-AS-COMPLETION-HANDLER-FUNC %s
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):5 | %FileCheck -check-prefix=MEMBER-FUNC-ON-NESTED-OTHER-OBJECT-AS-COMPLETION-HANDLER %s
  func testMethodOnNestedOtherObjectAsCompletionHandler(foo: Foo) {
    simple(completion: foo.foo.myFooPrint)
  }
  // MEMBER-FUNC-ON-NESTED-OTHER-OBJECT-AS-COMPLETION-HANDLER-FUNC: func testMethodOnNestedOtherObjectAsCompletionHandler(foo: Foo) async {
  // MEMBER-FUNC-ON-NESTED-OTHER-OBJECT-AS-COMPLETION-HANDLER-FUNC-NEXT:   let result = await simple()
  // MEMBER-FUNC-ON-NESTED-OTHER-OBJECT-AS-COMPLETION-HANDLER-FUNC-NEXT:   foo.foo.myFooPrint(result)
  // MEMBER-FUNC-ON-NESTED-OTHER-OBJECT-AS-COMPLETION-HANDLER-FUNC-NEXT: }

  // MEMBER-FUNC-ON-NESTED-OTHER-OBJECT-AS-COMPLETION-HANDLER: let result = await simple()
  // MEMBER-FUNC-ON-NESTED-OTHER-OBJECT-AS-COMPLETION-HANDLER-NEXT: foo.foo.myFooPrint(result)

}
