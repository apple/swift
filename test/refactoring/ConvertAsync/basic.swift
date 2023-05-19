// REQUIRES: concurrency

// RUN: %empty-directory(%t)

enum CustomError: Error {
  case invalid
  case insecure
}

typealias SomeCallback = (String) -> Void
typealias SomeResultCallback = (Result<String, CustomError>) -> Void
typealias NestedAliasCallback = SomeCallback

// 1. Check various functions for having/not having async alternatives

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+4):1 | %FileCheck -check-prefix=ASYNC-SIMPLE %s
// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+3):6 | %FileCheck -check-prefix=ASYNC-SIMPLE %s
// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+2):12 | %FileCheck -check-prefix=ASYNC-SIMPLE %s
// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):20 | %FileCheck -check-prefix=ASYNC-SIMPLE %s
func simple(/*cs*/ completion: @escaping (String) -> Void /*ce*/) { }
// ASYNC-SIMPLE: basic.swift [[# @LINE-1]]:1 -> [[# @LINE-1]]:1
// ASYNC-SIMPLE-NEXT: @available(*, renamed: "simple()")
// ASYNC-SIMPLE-EMPTY:
// ASYNC-SIMPLE-NEXT: basic.swift [[# @LINE-4]]:67 -> [[# @LINE-4]]:70
// ASYNC-SIMPLE-NEXT: {
// ASYNC-SIMPLE-NEXT: Task {
// ASYNC-SIMPLE-NEXT: let result = await simple()
// ASYNC-SIMPLE-NEXT: completion(result)
// ASYNC-SIMPLE-NEXT: }
// ASYNC-SIMPLE-NEXT: }
// ASYNC-SIMPLE-EMPTY:
// ASYNC-SIMPLE-NEXT: basic.swift [[# @LINE-12]]:70 -> [[# @LINE-12]]:70
// ASYNC-SIMPLE-EMPTY:
// ASYNC-SIMPLE-EMPTY:
// ASYNC-SIMPLE-EMPTY:
// ASYNC-SIMPLE-NEXT: basic.swift [[# @LINE-16]]:70 -> [[# @LINE-16]]:70
// ASYNC-SIMPLE-NEXT: func simple() async -> String { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ASYNC-SIMPLENOLABEL %s
func simpleWithoutLabel(_ completion: @escaping (String) -> Void) { }
// ASYNC-SIMPLENOLABEL: {
// ASYNC-SIMPLENOLABEL-NEXT: Task {
// ASYNC-SIMPLENOLABEL-NEXT: let result = await simpleWithoutLabel()
// ASYNC-SIMPLENOLABEL-NEXT: completion(result)
// ASYNC-SIMPLENOLABEL-NEXT: }
// ASYNC-SIMPLENOLABEL-NEXT: }
// ASYNC-SIMPLENOLABEL: func simpleWithoutLabel() async -> String { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ASYNC-SIMPLEWITHARG %s
func simpleWithArg(/*c1s*/ a: Int /*c1e*/, /*c2s*/ completion: @escaping (String) -> Void /*c2e*/) { }
// ASYNC-SIMPLEWITHARG: {
// ASYNC-SIMPLEWITHARG-NEXT: Task {
// ASYNC-SIMPLEWITHARG-NEXT: let result = await simpleWithArg(a: a)
// ASYNC-SIMPLEWITHARG-NEXT: completion(result)
// ASYNC-SIMPLEWITHARG-NEXT: }
// ASYNC-SIMPLEWITHARG-NEXT: }
// ASYNC-SIMPLEWITHARG: func simpleWithArg(/*c1s*/ a: Int /*c1e*/) async -> String { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ASYNC-MULTIPLERESULTS %s
func multipleResults(completion: @escaping (String, Int) -> Void) { }
// ASYNC-MULTIPLERESULTS: {
// ASYNC-MULTIPLERESULTS-NEXT: Task {
// ASYNC-MULTIPLERESULTS-NEXT: let result = await multipleResults()
// ASYNC-MULTIPLERESULTS-NEXT: completion(result.0, result.1)
// ASYNC-MULTIPLERESULTS-NEXT: }
// ASYNC-MULTIPLERESULTS-NEXT: }
// ASYNC-MULTIPLERESULTS: func multipleResults() async -> (String, Int) { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ASYNC-NONOPTIONALERROR %s
func nonOptionalError(completion: @escaping (String, Error) -> Void) { }
// ASYNC-NONOPTIONALERROR: {
// ASYNC-NONOPTIONALERROR-NEXT: Task {
// ASYNC-NONOPTIONALERROR-NEXT: let result = await nonOptionalError()
// ASYNC-NONOPTIONALERROR-NEXT: completion(result.0, result.1)
// ASYNC-NONOPTIONALERROR-NEXT: }
// ASYNC-NONOPTIONALERROR-NEXT: }
// ASYNC-NONOPTIONALERROR: func nonOptionalError() async -> (String, any Error) { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ASYNC-NOPARAMS %s
func noParams(completion: @escaping () -> Void) { }
// ASYNC-NOPARAMS: {
// ASYNC-NOPARAMS-NEXT: Task {
// ASYNC-NOPARAMS-NEXT: await noParams()
// ASYNC-NOPARAMS-NEXT: completion()
// ASYNC-NOPARAMS-NEXT: }
// ASYNC-NOPARAMS-NEXT: }
// ASYNC-NOPARAMS: func noParams() async { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ASYNC-ERROR %s
func error(completion: @escaping (String?, Error?) -> Void) { }
// ASYNC-ERROR: {
// ASYNC-ERROR-NEXT: Task {
// ASYNC-ERROR-NEXT: do {
// ASYNC-ERROR-NEXT: let result = try await error()
// ASYNC-ERROR-NEXT: completion(result, nil)
// ASYNC-ERROR-NEXT: } catch {
// ASYNC-ERROR-NEXT: completion(nil, error)
// ASYNC-ERROR-NEXT: }
// ASYNC-ERROR-NEXT: }
// ASYNC-ERROR: func error() async throws -> String { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ASYNC-ERRORONLY %s
func errorOnly(completion: @escaping (Error?) -> Void) { }
// ASYNC-ERRORONLY: {
// ASYNC-ERRORONLY-NEXT: Task {
// ASYNC-ERRORONLY-NEXT: do {
// ASYNC-ERRORONLY-NEXT: try await errorOnly()
// ASYNC-ERRORONLY-NEXT: completion(nil)
// ASYNC-ERRORONLY-NEXT: } catch {
// ASYNC-ERRORONLY-NEXT: completion(error)
// ASYNC-ERRORONLY-NEXT: }
// ASYNC-ERRORONLY-NEXT: }
// ASYNC-ERRORONLY-NEXT: }
// ASYNC-ERRORONLY: func errorOnly() async throws { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ASYNC-ERRORNONOPTIONALRESULT %s
func errorNonOptionalResult(completion: @escaping (String, Error?) -> Void) { }
// ASYNC-ERRORNONOPTIONALRESULT: {
// ASYNC-ERRORNONOPTIONALRESULT-NEXT: Task {
// ASYNC-ERRORNONOPTIONALRESULT-NEXT: do {
// ASYNC-ERRORNONOPTIONALRESULT-NEXT: let result = try await errorNonOptionalResult()
// ASYNC-ERRORNONOPTIONALRESULT-NEXT: completion(result, nil)
// ASYNC-ERRORNONOPTIONALRESULT-NEXT: } catch {
// ASYNC-ERRORNONOPTIONALRESULT-NEXT: completion(<#String#>, error)
// ASYNC-ERRORNONOPTIONALRESULT-NEXT: }
// ASYNC-ERRORNONOPTIONALRESULT-NEXT: }
// ASYNC-ERRORNONOPTIONALRESULT-NEXT: }
// ASYNC-ERRORNONOPTIONALRESULT: func errorNonOptionalResult() async throws -> String { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ASYNC-CUSTOMERROR %s
func customError(completion: @escaping (String?, CustomError?) -> Void) { }
// ASYNC-CUSTOMERROR: {
// ASYNC-CUSTOMERROR-NEXT: Task {
// ASYNC-CUSTOMERROR-NEXT: do {
// ASYNC-CUSTOMERROR-NEXT: let result = try await customError()
// ASYNC-CUSTOMERROR-NEXT: completion(result, nil)
// ASYNC-CUSTOMERROR-NEXT: } catch {
// ASYNC-CUSTOMERROR-NEXT: completion(nil, (error as! CustomError))
// ASYNC-CUSTOMERROR-NEXT: }
// ASYNC-CUSTOMERROR-NEXT: }
// ASYNC-CUSTOMERROR-NEXT: }
// ASYNC-CUSTOMERROR: func customError() async throws -> String { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ASYNC-ALIAS %s
func alias(completion: @escaping SomeCallback) { }
// ASYNC-ALIAS: {
// ASYNC-ALIAS-NEXT: Task {
// ASYNC-ALIAS-NEXT: let result = await alias()
// ASYNC-ALIAS-NEXT: completion(result)
// ASYNC-ALIAS-NEXT: }
// ASYNC-ALIAS-NEXT: }
// ASYNC-ALIAS: func alias() async -> String { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ASYNC-NESTEDALIAS %s
func nestedAlias(completion: @escaping NestedAliasCallback) { }
// ASYNC-NESTEDALIAS: {
// ASYNC-NESTEDALIAS-NEXT: Task {
// ASYNC-NESTEDALIAS-NEXT: let result = await nestedAlias()
// ASYNC-NESTEDALIAS-NEXT: completion(result)
// ASYNC-NESTEDALIAS-NEXT: }
// ASYNC-NESTEDALIAS-NEXT: }
// ASYNC-NESTEDALIAS: func nestedAlias() async -> String { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ASYNC-SIMPLERESULT %s
func simpleResult(completion: @escaping (Result<String, Never>) -> Void) { }
// ASYNC-SIMPLERESULT: {
// ASYNC-SIMPLERESULT-NEXT: Task {
// ASYNC-SIMPLERESULT-NEXT: let result = await simpleResult()
// ASYNC-SIMPLERESULT-NEXT: completion(.success(result))
// ASYNC-SIMPLERESULT-NEXT: }
// ASYNC-SIMPLERESULT-NEXT: }
// ASYNC-SIMPLERESULT: func simpleResult() async -> String { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ASYNC-ERRORRESULT %s
func errorResult(completion: @escaping (Result<String, Error>) -> Void) { }
// ASYNC-ERRORRESULT: {
// ASYNC-ERRORRESULT-NEXT: Task {
// ASYNC-ERRORRESULT-NEXT: do {
// ASYNC-ERRORRESULT-NEXT: let result = try await errorResult()
// ASYNC-ERRORRESULT-NEXT: completion(.success(result))
// ASYNC-ERRORRESULT-NEXT: } catch {
// ASYNC-ERRORRESULT-NEXT: completion(.failure(error))
// ASYNC-ERRORRESULT-NEXT: }
// ASYNC-ERRORRESULT-NEXT: }
// ASYNC-ERRORRESULT-NEXT: }
// ASYNC-ERRORRESULT: func errorResult() async throws -> String { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ASYNC-CUSTOMERRORRESULT %s
func customErrorResult(completion: @escaping (Result<String, CustomError>) -> Void) { }
// ASYNC-CUSTOMERRORRESULT: {
// ASYNC-CUSTOMERRORRESULT-NEXT: Task {
// ASYNC-CUSTOMERRORRESULT-NEXT: do {
// ASYNC-CUSTOMERRORRESULT-NEXT: let result = try await customErrorResult()
// ASYNC-CUSTOMERRORRESULT-NEXT: completion(.success(result))
// ASYNC-CUSTOMERRORRESULT-NEXT: } catch {
// ASYNC-CUSTOMERRORRESULT-NEXT: completion(.failure(error as! CustomError))
// ASYNC-CUSTOMERRORRESULT-NEXT: }
// ASYNC-CUSTOMERRORRESULT-NEXT: }
// ASYNC-CUSTOMERRORRESULT-NEXT: }
// ASYNC-CUSTOMERRORRESULT: func customErrorResult() async throws -> String { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ASYNC-ALIASRESULT %s
func aliasedResult(completion: @escaping SomeResultCallback) { }
// ASYNC-ALIASRESULT: {
// ASYNC-ALIASRESULT-NEXT: Task {
// ASYNC-ALIASRESULT-NEXT: do {
// ASYNC-ALIASRESULT-NEXT: let result = try await aliasedResult()
// ASYNC-ALIASRESULT-NEXT: completion(.success(result))
// ASYNC-ALIASRESULT-NEXT: } catch {
// ASYNC-ALIASRESULT-NEXT: completion(.failure(error as! CustomError))
// ASYNC-ALIASRESULT-NEXT: }
// ASYNC-ALIASRESULT-NEXT: }
// ASYNC-ALIASRESULT-NEXT: }
// ASYNC-ALIASRESULT: func aliasedResult() async throws -> String { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=MANY %s
func many(_ completion: @escaping (String, Int) -> Void) { }
// MANY: {
// MANY-NEXT: Task {
// MANY-NEXT: let result = await many()
// MANY-NEXT: completion(result.0, result.1)
// MANY-NEXT: }
// MANY-NEXT: }
// MANY: func many() async -> (String, Int) { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=OPTIONAL-SINGLE %s
func optionalSingle(completion: @escaping (String?) -> Void) { }
// OPTIONAL-SINGLE: {
// OPTIONAL-SINGLE-NEXT: Task {
// OPTIONAL-SINGLE-NEXT: let result = await optionalSingle()
// OPTIONAL-SINGLE-NEXT: completion(result)
// OPTIONAL-SINGLE-NEXT: }
// OPTIONAL-SINGLE-NEXT: }
// OPTIONAL-SINGLE: func optionalSingle() async -> String? { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=MANY-OPTIONAL %s
func manyOptional(_ completion: @escaping (String?, Int?) -> Void) { }
// MANY-OPTIONAL: {
// MANY-OPTIONAL-NEXT: Task {
// MANY-OPTIONAL-NEXT: let result = await manyOptional()
// MANY-OPTIONAL-NEXT: completion(result.0, result.1)
// MANY-OPTIONAL-NEXT: }
// MANY-OPTIONAL-NEXT: }
// MANY-OPTIONAL: func manyOptional() async -> (String?, Int?) { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=MIXED %s
func mixed(_ completion: @escaping (String?, Int) -> Void) { }
// MIXED: {
// MIXED-NEXT: Task {
// MIXED-NEXT: let result = await mixed()
// MIXED-NEXT: completion(result.0, result.1)
// MIXED-NEXT: }
// MIXED-NEXT: }
// MIXED: func mixed() async -> (String?, Int) { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=MIXED-OPTIONAL-ERROR %s
func mixedOptionalError(_ completion: @escaping (String?, Int, Error?) -> Void) { }
// MIXED-OPTIONAL-ERROR: {
// MIXED-OPTIONAL-ERROR-NEXT: Task {
// MIXED-OPTIONAL-ERROR-NEXT: do {
// MIXED-OPTIONAL-ERROR-NEXT: let result = try await mixedOptionalError()
// MIXED-OPTIONAL-ERROR-NEXT: completion(result.0, result.1, nil)
// MIXED-OPTIONAL-ERROR-NEXT: } catch {
// MIXED-OPTIONAL-ERROR-NEXT: completion(nil, <#Int#>, error)
// MIXED-OPTIONAL-ERROR-NEXT: }
// MIXED-OPTIONAL-ERROR-NEXT: }
// MIXED-OPTIONAL-ERROR-NEXT: }
// MIXED-OPTIONAL-ERROR: func mixedOptionalError() async throws -> (String, Int) { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=MIXED-ERROR %s
func mixedError(_ completion: @escaping (String?, Int, Error) -> Void) { }
// MIXED-ERROR: {
// MIXED-ERROR-NEXT: Task {
// MIXED-ERROR-NEXT: let result = await mixedError()
// MIXED-ERROR-NEXT: completion(result.0, result.1, result.2)
// MIXED-ERROR-NEXT: }
// MIXED-ERROR-NEXT: }
// MIXED-ERROR: func mixedError() async -> (String?, Int, any Error) { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=GENERIC %s
func generic<T, R>(completion: @escaping (T, R) -> Void) { }
// GENERIC: {
// GENERIC-NEXT: Task {
// GENERIC-NEXT: let result: (T, R) = await generic()
// GENERIC-NEXT: completion(result.0, result.1)
// GENERIC-NEXT: }
// GENERIC-NEXT: }
// GENERIC: func generic<T, R>() async -> (T, R) { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=GENERIC-RESULT %s
func genericResult<T>(completion: @escaping (T?, Error?) -> Void) where T: Numeric { }
// GENERIC-RESULT: {
// GENERIC-RESULT-NEXT: Task {
// GENERIC-RESULT-NEXT: do {
// GENERIC-RESULT-NEXT: let result: T = try await genericResult()
// GENERIC-RESULT-NEXT: completion(result, nil)
// GENERIC-RESULT-NEXT: } catch {
// GENERIC-RESULT-NEXT: completion(nil, error)
// GENERIC-RESULT-NEXT: }
// GENERIC-RESULT-NEXT: }
// GENERIC-RESULT-NEXT: }
// GENERIC-RESULT: func genericResult<T>() async throws -> T where T: Numeric { }

// FIXME: This doesn't compile after refactoring because we aren't using the
// generic argument 'E' in the async method
// (https://github.com/apple/swift/issues/56912).
// RUN: %refactor -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=GENERIC-ERROR %s
func genericError<E>(completion: @escaping (String?, E?) -> Void) where E: Error { }
// GENERIC-ERROR: {
// GENERIC-ERROR-NEXT: Task {
// GENERIC-ERROR-NEXT: do {
// GENERIC-ERROR-NEXT: let result: String = try await genericError()
// GENERIC-ERROR-NEXT: completion(result, nil)
// GENERIC-ERROR-NEXT: } catch {
// GENERIC-ERROR-NEXT: completion(nil, (error as! E))
// GENERIC-ERROR-NEXT: }
// GENERIC-ERROR-NEXT: }
// GENERIC-ERROR-NEXT: }
// GENERIC-ERROR: func genericError<E>() async throws -> String where E: Error { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=OTHER-NAME %s
func otherName(execute: @escaping (String) -> Void) { }
// OTHER-NAME: {
// OTHER-NAME-NEXT: Task {
// OTHER-NAME-NEXT: let result = await otherName()
// OTHER-NAME-NEXT: execute(result)
// OTHER-NAME-NEXT: }
// OTHER-NAME-NEXT: }
// OTHER-NAME: func otherName() async -> String { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=DEFAULT_ARGS %s
func defaultArgs(a: Int, b: Int = 10, completion: @escaping (String) -> Void) { }
// DEFAULT_ARGS: {
// DEFAULT_ARGS-NEXT: Task {
// DEFAULT_ARGS-NEXT: let result = await defaultArgs(a: a, b: b)
// DEFAULT_ARGS-NEXT: completion(result)
// DEFAULT_ARGS-NEXT: }
// DEFAULT_ARGS-NEXT: }
// DEFAULT_ARGS: func defaultArgs(a: Int, b: Int = 10) async -> String { }

struct MyStruct {
  var someVar: (Int) -> Void {
    get {
      return {_ in }
    }
    // RUN: not %refactor -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):5
    set (completion) {
    }
  }

  init() { }

  // RUN: not %refactor -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):3
  init(completion: @escaping (String) -> Void) { }

  func retSelf() -> MyStruct { return self }

  // RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):10 | %FileCheck -check-prefix=MODIFIERS %s
  public func publicMember(completion: @escaping (String) -> Void) { }
  // MODIFIERS: public func publicMember() async -> String { }

  // RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=STATIC %s
  static func staticMember(completion: @escaping (String) -> Void) { }
  // STATIC: static func staticMember() async -> String { }

  // RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+2):11 | %FileCheck -check-prefix=DEPRECATED %s
  @available(*, deprecated, message: "Deprecated")
  private func deprecated(completion: @escaping (String) -> Void) { }
  // DEPRECATED: @available(*, deprecated, message: "Deprecated")
  // DEPRECATED-NEXT: private func deprecated() async -> String { }
}
func retStruct() -> MyStruct { return MyStruct() }

protocol MyProtocol {
  // RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=PROTO-MEMBER %s
  // RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=PROTO-MEMBER %s
  func protoMember(completion: @escaping (String) -> Void)
  // PROTO-MEMBER: func protoMember() async -> String{{$}}
}

// RUN: not %refactor -add-async-alternative -dump-text -source-filename %s -pos=%(line+2):1
// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=NON-COMPLETION %s
func nonCompletion(a: Int) { }
// NON-COMPLETION: func nonCompletion(a: Int) async { }

// RUN: not %refactor -add-async-alternative -dump-text -source-filename %s -pos=%(line+2):1
// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=NON-ESCAPING-COMPLETION %s
func nonEscapingCompletion(completion: (Int) -> Void) { }
// NON-ESCAPING-COMPLETION: func nonEscapingCompletion(completion: (Int) -> Void) async { }

// RUN: not %refactor -add-async-alternative -dump-text -source-filename %s -pos=%(line+2):1
// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=MULTIPLE-RESULTS %s
func multipleResults(completion: @escaping (Result<String, Error>, Result<String, Error>) -> Void) { }
// MULTIPLE-RESULTS: func multipleResults(completion: @escaping (Result<String, Error>, Result<String, Error>) -> Void) async { }

// RUN: not %refactor -add-async-alternative -dump-text -source-filename %s -pos=%(line+2):1
// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=NON-VOID %s
func nonVoid(completion: @escaping (String) -> Void) -> Int { return 0 }
// NON-VOID: func nonVoid(completion: @escaping (String) -> Void) async -> Int { return 0 }

// RUN: not %refactor -add-async-alternative -dump-text -source-filename %s -pos=%(line+2):1
// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=COMPLETION-NON-VOID %s
func completionNonVoid(completion: @escaping (String) -> Int) -> Void { }
// COMPLETION-NON-VOID: func completionNonVoid(completion: @escaping (String) -> Int) async -> Void { }

// RUN: not %refactor -add-async-alternative -dump-text -source-filename %s -pos=%(line+2):1
// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=ALREADY-THROWS %s
func alreadyThrows(completion: @escaping (String) -> Void) throws { }
// ALREADY-THROWS: func alreadyThrows(completion: @escaping (String) -> Void) async throws { }

// RUN: not %refactor -add-async-alternative -dump-text -source-filename %s -pos=%(line+2):1
// RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=AUTO-CLOSURE %s
func noParamAutoclosure(completion: @escaping @autoclosure () -> Void) { }
// AUTO-CLOSURE: func noParamAutoclosure(completion: @escaping @autoclosure () -> Void) async { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix BLOCK-CONVENTION %s
func blockConvention(completion: @escaping @convention(block) () -> Void) { }
// BLOCK-CONVENTION: func blockConvention() async { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix C-CONVENTION %s
func cConvention(completion: @escaping @convention(c) () -> Void) { }
// C-CONVENTION: func cConvention() async { }

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix OPT-VOID-AND-ERROR-HANDLER %s
func optVoidAndErrorCompletion(completion: @escaping (Void?, Error?) -> Void) {}
// OPT-VOID-AND-ERROR-HANDLER: {
// OPT-VOID-AND-ERROR-HANDLER-NEXT: Task {
// OPT-VOID-AND-ERROR-HANDLER-NEXT: do {
// OPT-VOID-AND-ERROR-HANDLER-NEXT: try await optVoidAndErrorCompletion()
// OPT-VOID-AND-ERROR-HANDLER-NEXT: completion((), nil)
// OPT-VOID-AND-ERROR-HANDLER-NEXT: } catch {
// OPT-VOID-AND-ERROR-HANDLER-NEXT: completion(nil, error)
// OPT-VOID-AND-ERROR-HANDLER-NEXT: }
// OPT-VOID-AND-ERROR-HANDLER-NEXT: }
// OPT-VOID-AND-ERROR-HANDLER-NEXT: }
// OPT-VOID-AND-ERROR-HANDLER: func optVoidAndErrorCompletion() async throws {}

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix TOO-MUCH-VOID-AND-ERROR-HANDLER %s
func tooMuchVoidAndErrorCompletion(completion: @escaping (Void?, Void?, Error?) -> Void) {}
// TOO-MUCH-VOID-AND-ERROR-HANDLER: {
// TOO-MUCH-VOID-AND-ERROR-HANDLER-NEXT: Task {
// TOO-MUCH-VOID-AND-ERROR-HANDLER-NEXT: do {
// TOO-MUCH-VOID-AND-ERROR-HANDLER-NEXT: try await tooMuchVoidAndErrorCompletion()
// TOO-MUCH-VOID-AND-ERROR-HANDLER-NEXT: completion((), (), nil)
// TOO-MUCH-VOID-AND-ERROR-HANDLER-NEXT: } catch {
// TOO-MUCH-VOID-AND-ERROR-HANDLER-NEXT: completion(nil, nil, error)
// TOO-MUCH-VOID-AND-ERROR-HANDLER-NEXT: }
// TOO-MUCH-VOID-AND-ERROR-HANDLER-NEXT: }
// TOO-MUCH-VOID-AND-ERROR-HANDLER-NEXT: }
// TOO-MUCH-VOID-AND-ERROR-HANDLER: func tooMuchVoidAndErrorCompletion() async throws {}

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix VOID-PROPER-AND-ERROR-HANDLER %s
func tooVoidProperAndErrorCompletion(completion: @escaping (Void?, String?, Error?) -> Void) {}
// VOID-PROPER-AND-ERROR-HANDLER: {
// VOID-PROPER-AND-ERROR-HANDLER-NEXT: Task {
// VOID-PROPER-AND-ERROR-HANDLER-NEXT: do {
// VOID-PROPER-AND-ERROR-HANDLER-NEXT: let result = try await tooVoidProperAndErrorCompletion()
// VOID-PROPER-AND-ERROR-HANDLER-NEXT: completion((), result.1, nil)
// VOID-PROPER-AND-ERROR-HANDLER-NEXT: } catch {
// VOID-PROPER-AND-ERROR-HANDLER-NEXT: completion(nil, nil, error)
// VOID-PROPER-AND-ERROR-HANDLER-NEXT: }
// VOID-PROPER-AND-ERROR-HANDLER-NEXT: }
// VOID-PROPER-AND-ERROR-HANDLER-NEXT: }
// VOID-PROPER-AND-ERROR-HANDLER: func tooVoidProperAndErrorCompletion() async throws -> (Void, String) {}

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix VOID-AND-ERROR-HANDLER %s
func voidAndErrorCompletion(completion: @escaping (Void, Error?) -> Void) {}
// VOID-AND-ERROR-HANDLER: {
// VOID-AND-ERROR-HANDLER-NEXT: Task {
// VOID-AND-ERROR-HANDLER-NEXT: do {
// VOID-AND-ERROR-HANDLER-NEXT: try await voidAndErrorCompletion()
// VOID-AND-ERROR-HANDLER-NEXT: completion((), nil)
// VOID-AND-ERROR-HANDLER-NEXT: } catch {
// VOID-AND-ERROR-HANDLER-NEXT: completion((), error)
// VOID-AND-ERROR-HANDLER-NEXT: }
// VOID-AND-ERROR-HANDLER-NEXT: }
// VOID-AND-ERROR-HANDLER-NEXT: }
// VOID-AND-ERROR-HANDLER: func voidAndErrorCompletion() async throws {}

// RUN: %refactor-check-compiles -add-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix VOID-RESULT-HANDLER %s
func voidResultCompletion(completion: @escaping (Result<Void, Error>) -> Void) {}
// VOID-RESULT-HANDLER:      {
// VOID-RESULT-HANDLER-NEXT:   Task {
// VOID-RESULT-HANDLER-NEXT:     do {
// VOID-RESULT-HANDLER-NEXT:       try await voidResultCompletion()
// VOID-RESULT-HANDLER-NEXT:       completion(.success(()))
// VOID-RESULT-HANDLER-NEXT:     } catch {
// VOID-RESULT-HANDLER-NEXT:       completion(.failure(error))
// VOID-RESULT-HANDLER-NEXT:     }
// VOID-RESULT-HANDLER-NEXT:   }
// VOID-RESULT-HANDLER-NEXT: }
// VOID-RESULT-HANDLER:      func voidResultCompletion() async throws {


// 2. Check that the various ways to call a function (and the positions the
//    refactoring is called from) are handled correctly

class MyClass {}

func simpleClassParam(completion: @escaping (MyClass) -> Void) { }

// TODO: We cannot check that the refactored code compiles because 'simple' and
// friends aren't refactored when only invoking the refactoring on this function.
// TODO: When changing this line to %refactor-check-compiles, 'swift-refactor'
// is crashing in '-dump-rewritten'. This is because
// 'swift-refactor -dump-rewritten' is removing 'RUN' lines. After removing
// those lines, we are trying to remove the function body, using its length
// before the 'RUN' lines were removed, thus pointing past the end of the
// rewritten buffer.

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=CALL %s
func testSimple() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+4):3 | %FileCheck -check-prefix=CALL %s
  // RUN: not %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+3):10
  // RUN: not %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):24
  // RUN: not %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):28
  simple(completion: { str in
    // RUN: not %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):5
    print("with label")
  })
}
// CALL: let str = await simple(){{$}}
// CALL-NEXT: //
// CALL-NEXT: {{^}} print("with label")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=CALL-NOLABEL %s
func testSimpleWithoutLabel() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=CALL-NOLABEL %s
  simpleWithoutLabel({ str in
    print("without label")
  })
}
// CALL-NOLABEL: let str = await simpleWithoutLabel(){{$}}
// CALL-NOLABEL-NEXT: {{^}}print("without label")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=CALL-WRAPPED %s
func testWrapped() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=CALL-WRAPPED %s
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):5 | %FileCheck -check-prefix=CALL-WRAPPED %s
  ((simple))(completion: { str in
    print("wrapped call")
  })
}
// CALL-WRAPPED: let str = await ((simple))(){{$}}
// CALL-WRAPPED-NEXT: {{^}}print("wrapped call")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=TRAILING %s
func testTrailing() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=TRAILING %s
  // RUN: not %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):12
  simple { str in
    print("trailing")
  }
}
// TRAILING: let str = await simple(){{$}}
// TRAILING-NEXT: {{^}}print("trailing")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=TRAILING-PARENS %s
func testTrailingParens() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=TRAILING-PARENS %s
  simple() { str in
    print("trailing with parens")
  }
}
// TRAILING-PARENS: let str = await simple(){{$}}
// TRAILING-PARENS-NEXT: {{^}}print("trailing with parens")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefixes=TRAILING-WRAPPED %s
func testTrailingWrapped() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):5 | %FileCheck -check-prefix=TRAILING-WRAPPED %s
  ((simple)) { str in
    print("trailing with wrapped call")
  }
}
// TRAILING-WRAPPED: let str = await ((simple))(){{$}}
// TRAILING-WRAPPED-NEXT: {{^}}print("trailing with wrapped call")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=CALL-ARG %s
func testCallArg() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+3):3 | %FileCheck -check-prefix=CALL-ARG %s
  // RUN: not %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):17
  // RUN: not %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):20
  simpleWithArg(a: 10) { str in
    print("with arg")
  }
}
// CALL-ARG: let str = await simpleWithArg(a: 10){{$}}
// CALL-ARG-NEXT: {{^}}print("with arg")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefixes=MANY-CALL %s
func testMany() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MANY-CALL %s
  many { str, num in
    print("many")
  }
}
// MANY-CALL: let (str, num) = await many(){{$}}
// MANY-CALL-NEXT: {{^}}print("many")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=MEMBER-CALL %s
func testMember() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):15 | %FileCheck -check-prefix=MEMBER-CALL %s
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MEMBER-CALL %s
  retStruct().publicMember { str in
    print("call on member")
  }
}
// MEMBER-CALL: let str = await retStruct().publicMember(){{$}}
// MEMBER-CALL-NEXT: {{^}}print("call on member")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=MEMBER-CALL2 %s
func testMember2() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):25 | %FileCheck -check-prefix=MEMBER-CALL2 %s
  retStruct().retSelf().publicMember { str in
    print("call on member 2")
  }
}
// MEMBER-CALL2: let str = await retStruct().retSelf().publicMember(){{$}}
// MEMBER-CALL2-NEXT: {{^}}print("call on member 2")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=MEMBER-PARENS %s
func testMemberParens() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+3):3 | %FileCheck -check-prefix=MEMBER-PARENS %s
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):5 | %FileCheck -check-prefix=MEMBER-PARENS %s
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):15 | %FileCheck -check-prefix=MEMBER-PARENS %s
  (((retStruct().retSelf()).publicMember)) { str in
    print("call on member parens")
  }
}
// MEMBER-PARENS: let str = await (((retStruct().retSelf()).publicMember))(){{$}}
// MEMBER-PARENS-NEXT: {{^}}print("call on member parens")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefixes=SKIP-ASSIGN-FUNC %s
func testSkipAssign() {
  // RUN: not %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):13
  let _: Void = simple { str in
    print("assigned")
  }
}
// SKIP-ASSIGN-FUNC: {{^}}func testSkipAssign() async {
// SKIP-ASSIGN-FUNC: let _: Void = simple { str in{{$}}
// SKIP-ASSIGN-FUNC-NEXT: print("assigned"){{$}}
// SKIP-ASSIGN-FUNC-NEXT: }{{$}}

// Same as noParamAutoclosure defined above, but used just for the test below.
// This avoids a compiler error when converting noParamAutoclosure to async.
func noParamAutoclosure2(completion: @escaping @autoclosure () -> Void) {}

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefixes=SKIP-AUTOCLOSURE-FUNC %s
func testSkipAutoclosure() {
  // RUN: not %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3
  noParamAutoclosure2(completion: print("autoclosure"))
}
// SKIP-AUTOCLOSURE-FUNC: {{^}}func testSkipAutoclosure() async {
// SKIP-AUTOCLOSURE-FUNC: noParamAutoclosure2(completion: print("autoclosure")){{$}}

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=EMPTY-CAPTURE %s
func testEmptyCapture() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=EMPTY-CAPTURE %s
  simple { [] str in
    print("closure with empty capture list")
  }
}
// EMPTY-CAPTURE: let str = await simple(){{$}}
// EMPTY-CAPTURE-NEXT: {{^}}print("closure with empty capture list")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=CAPTURE %s
func testCapture() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+2):3 | %FileCheck -check-prefix=CAPTURE %s
  let myClass = MyClass()
  simpleClassParam { [unowned myClass] str in
    print("closure with capture list \(myClass)")
  }
}
// CAPTURE: let str = await simpleClassParam(){{$}}
// CAPTURE-NEXT: {{^}}print("closure with capture list \(myClass)")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefixes=NOT-HANDLER-FUNC %s
func testNotCompletionHandler() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=NOT-HANDLER %s
  otherName(execute: { str in
    print("otherName")
  })
}
// NOT-HANDLER-FUNC: otherName(execute: { str in{{$}}
// NOT-HANDLER-FUNC-NEXT: print("otherName"){{$}}
// NOT-HANDLER-FUNC-NEXT: }){{$}}
// NOT-HANDLER: let str = await otherName(){{$}}
// NOT-HANDLER-NEXT: {{^}}print("otherName")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=DEFAULT-ARGS-MISSING %s
func testDefaultArgsMissing() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=DEFAULT-ARGS-MISSING %s
  defaultArgs(a: 1) { str in
    print("defaultArgs missing")
  }
}
// DEFAULT-ARGS-MISSING: let str = await defaultArgs(a: 1){{$}}
// DEFAULT-ARGS-MISSING-NEXT: {{^}}print("defaultArgs missing")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=DEFAULT-ARGS-CALL %s
func testDefaultArgs() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=DEFAULT-ARGS-CALL %s
  defaultArgs(a: 1, b: 2) { str in
    print("defaultArgs")
  }
}
// DEFAULT-ARGS-CALL: let str = await defaultArgs(a: 1, b: 2){{$}}
// DEFAULT-ARGS-CALL-NEXT: {{^}}print("defaultArgs")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=BLOCK-CONVENTION-CALL %s
func testBlockConvention() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=BLOCK-CONVENTION-CALL %s
  blockConvention {
    print("blockConvention")
  }
}
// BLOCK-CONVENTION-CALL: await blockConvention(){{$}}
// BLOCK-CONVENTION-CALL-NEXT: {{^}}print("blockConvention")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=C-CONVENTION-CALL %s
func testCConvention() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=C-CONVENTION-CALL %s
  cConvention {
    print("cConvention")
  }
}
// C-CONVENTION-CALL: await cConvention(){{$}}
// C-CONVENTION-CALL-NEXT: {{^}}print("cConvention")

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=VOID-AND-ERROR-CALL %s
func testVoidAndError() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=VOID-AND-ERROR-CALL %s
  optVoidAndErrorCompletion { v, err in
    print("opt void and error completion \(String(describing: v))")
  }
}
// VOID-AND-ERROR-CALL: try await optVoidAndErrorCompletion(){{$}}
// VOID-AND-ERROR-CALL-NEXT: {{^}}print("opt void and error completion \(String(describing: <#v#>))"){{$}}

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=VOID-AND-ERROR-CALL2 %s
func testVoidAndError2() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=VOID-AND-ERROR-CALL2 %s
  optVoidAndErrorCompletion { _, err in
    print("opt void and error completion 2")
  }
}
// VOID-AND-ERROR-CALL2: try await optVoidAndErrorCompletion(){{$}}
// VOID-AND-ERROR-CALL2-NEXT: {{^}}print("opt void and error completion 2"){{$}}

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=VOID-AND-ERROR-CALL3 %s
func testVoidAndError3() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=VOID-AND-ERROR-CALL3 %s
  tooMuchVoidAndErrorCompletion { v, v1, err in
    print("void and error completion 3")
  }
}
// VOID-AND-ERROR-CALL3: try await tooMuchVoidAndErrorCompletion(){{$}}
// VOID-AND-ERROR-CALL3-NEXT: {{^}}print("void and error completion 3"){{$}}

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=VOID-AND-ERROR-CALL4 %s
func testVoidAndError4() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=VOID-AND-ERROR-CALL4 %s
  voidAndErrorCompletion { v, err in
    print("void and error completion \(v)")
  }
}
// VOID-AND-ERROR-CALL4: try await voidAndErrorCompletion(){{$}}
// VOID-AND-ERROR-CALL4-NEXT: {{^}}print("void and error completion \(<#v#>)"){{$}}

func testPreserveComments() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=PRESERVE-COMMENTS %s
  simpleWithArg(/*hello*/ a: /*a*/5) { str in
  // b1
  // b2
  print("1")
  // c
  print("2") /*
   d1
   d2
   */
  if .random() {
    // e
  }
  /* f1 */
  /* f2 */} // don't pick this up
}
// PRESERVE-COMMENTS:      let str = await simpleWithArg(/*hello*/ a: /*a*/5)
// PRESERVE-COMMENTS-NEXT: // b1
// PRESERVE-COMMENTS-NEXT: // b2
// PRESERVE-COMMENTS-NEXT: print("1")
// PRESERVE-COMMENTS-NEXT: // c
// PRESERVE-COMMENTS-NEXT: print("2")
// PRESERVE-COMMENTS-NEXT: /*
// PRESERVE-COMMENTS-NEXT:  d1
// PRESERVE-COMMENTS-NEXT:  d2
// PRESERVE-COMMENTS-NEXT: */
// PRESERVE-COMMENTS-NEXT: if .random() {
// PRESERVE-COMMENTS-NEXT:   // e
// PRESERVE-COMMENTS-NEXT: }
// PRESERVE-COMMENTS-NEXT: /* f1 */
// PRESERVE-COMMENTS-NEXT: /* f2 */{{$}}
// PRESERVE-COMMENTS-NOT: }{{$}}

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=PRESERVE-COMMENTS-ERROR %s
func testPreserveComments2() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=PRESERVE-COMMENTS-ERROR %s
  errorOnly { err in
    // a
    if err != nil {
      // b
      print("oh no") // c
      /* d */
      return /* e */
    }
    if err != nil {
      // f
      print("fun")
      // g
    }
    // h
    print("good times") // i
  }
}
// PRESERVE-COMMENTS-ERROR:      do {
// PRESERVE-COMMENTS-ERROR-NEXT:   try await errorOnly()
// PRESERVE-COMMENTS-ERROR-NEXT:   // a
// PRESERVE-COMMENTS-ERROR-NEXT:   // h
// PRESERVE-COMMENTS-ERROR-NEXT:   print("good times")
// PRESERVE-COMMENTS-ERROR-NEXT:   // i
// PRESERVE-COMMENTS-ERROR:      } catch let err {
// PRESERVE-COMMENTS-ERROR-NEXT:   // b
// PRESERVE-COMMENTS-ERROR-NEXT:   print("oh no")
// PRESERVE-COMMENTS-ERROR-NEXT:   // c
// PRESERVE-COMMENTS-ERROR-NEXT:   /* d */
// PRESERVE-COMMENTS-ERROR-NEXT:   /* e */
// PRESERVE-COMMENTS-ERROR-NEXT:   // f
// PRESERVE-COMMENTS-ERROR-NEXT:   print("fun")
// PRESERVE-COMMENTS-ERROR-NEXT:   // g
// PRESERVE-COMMENTS-ERROR-NEXT: }

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=PRESERVE-TRAILING-COMMENT-FN %s
func testPreserveComments3() {
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=PRESERVE-TRAILING-COMMENT-CALL %s
  simple { s in
    print(s)
  }
  // make sure we pickup this trailing comment if we're converting the function, but not the call
}
// PRESERVE-TRAILING-COMMENT-FN:      func testPreserveComments3() async {
// PRESERVE-TRAILING-COMMENT-FN-NEXT:   //
// PRESERVE-TRAILING-COMMENT-FN-NEXT:   let s = await simple()
// PRESERVE-TRAILING-COMMENT-FN-NEXT:   print(s)
// PRESERVE-TRAILING-COMMENT-FN-NEXT:   // make sure we pickup this trailing comment if we're converting the function, but not the call
// PRESERVE-TRAILING-COMMENT-FN-NEXT: }

// PRESERVE-TRAILING-COMMENT-CALL:      let s = await simple()
// PRESERVE-TRAILING-COMMENT-CALL-NEXT: print(s)
// PRESERVE-TRAILING-COMMENT-CALL-NOT:  // make sure we pickup this trailing comment if we're converting the function, but not the call

class TestConvertFunctionWithCallToFunctionsWithSpecialName {
  required init() {}
  subscript(index: Int) -> Int { return index }

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):3
  static func example() -> Self {
    let x = self.init()
    _ = x[1]
    return x
  }
}

// rdar://78781061
// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=FOR-IN-WHERE %s
func testForInWhereRefactoring() {
  let arr: [String] = []
  for str in arr where str.count != 0 {
    simple { res in
      print(res)
    }
  }
}
// FOR-IN-WHERE:      func testForInWhereRefactoring() async {
// FOR-IN-WHERE-NEXT:   let arr: [String] = []
// FOR-IN-WHERE-NEXT:   for str in arr where str.count != 0 {
// FOR-IN-WHERE-NEXT:     let res = await simple()
// FOR-IN-WHERE-NEXT:     print(res)
// FOR-IN-WHERE-NEXT:   }
// FOR-IN-WHERE-NEXT: }
