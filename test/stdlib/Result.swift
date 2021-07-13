// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import StdlibUnittest
import Swift

let ResultTests = TestSuite("Result")

fileprivate enum Err: Error, Equatable {
  case err
  case derr
}

fileprivate let string = "string"

fileprivate extension Result {
  var success: Success? {
    switch self {
    case let .success(success):
      return success
    case .failure:
      return nil
    }
  }
  
  var failure: Failure? {
    switch self {
    case .success:
      return nil
    case let .failure(failure):
      return failure
    }
  }
}

ResultTests.test("Construction") {
  let result1: Result<String, Err> = .success(string)
  let result2: Result<String, Err> = .failure(.err)
  let string1: String? = {
    switch result1 {
      case let .success(string): 
        return string
      case .failure: 
        expectUnreachable()
        return nil
    }
  }()
  let error: Err? = {
    switch result2 {
      case let .failure(failure): 
        return failure
      case .success: 
        expectUnreachable()
        return nil
    }
  }()

  expectEqual(string, string1)
  expectEqual(.err, error)
}

ResultTests.test("Throwing Initialization and Unwrapping") {
  func notThrowing() throws -> String {
    return string
  }

  func throwing() throws -> String {
    throw Err.err
  }
    
  let result1 = Result { try throwing() }
  let result2 = Result { try notThrowing() }
  
  expectEqual(result1.failure as? Err, Err.err)
  expectEqual(result2.success, string)
    
  do {
    _ = try result1.get()
  } catch let error as Err {
    expectEqual(error, Err.err)
  } catch {
    expectUnreachable()
  }

  do {
    let unwrapped = try result2.get()
    expectEqual(unwrapped, string)
  } catch {
    expectUnreachable()
  }

  // Test unwrapping strongly typed error.
  let result3 = Result<String, Err>.failure(Err.err)
  do {
    _ = try result3.get()
  } catch let error as Err {
    expectEqual(error, Err.err)
  } catch {
    expectUnreachable()
  }

  // Test throwing value property
  do {
    _ = try result1.value
  } catch let error as Err {
    expectEqual(error, Err.err)
  } catch {
    expectUnreachable()
  }

  do {
    let unwrapped = try result2.value
    expectEqual(unwrapped, string)
  } catch {
    expectUnreachable()
  }
}

ResultTests.test("Functional Transforms") {
  func transformDouble(_ int: Int) -> Int {
    return 2 * int
  }
  
  func transformTriple(_ int: Int) -> Int {
    return 3 * int
  }
  
  func transformError(_ err: Err) -> Err {
    if err == .err {
      return .derr
    } else {
      return .err
    }
  }

  func resultValueTransform(_ int: Int) -> Result<Int, Err> {
    return .success(transformDouble(int))
  }
  
  func resultErrorTransform(_ err: Err) -> Result<Int, Err> {
    return .failure(transformError(err))
  }
    
  let result1: Result<Int, Err> = .success(1)
  let newResult1 = result1.map(transformDouble)
    
  expectEqual(newResult1, .success(2))
    
  let result2: Result<Int, Err> = .failure(.err)
  let newResult2 = result2.mapError(transformError)
    
  expectEqual(newResult2, .failure(.derr))
    
  let result3: Result<Int, Err> = .success(1)
  let newResult3 = result3.flatMap(resultValueTransform)
    
  expectEqual(newResult3, .success(2))
    
  let result4: Result<Int, Err> = .failure(.derr)
  let newResult4 = result4.flatMapError(resultErrorTransform)
    
  expectEqual(newResult4, .failure(.err))
}

ResultTests.test("Equatable") {
  let result1: Result<Int, Err> = .success(1)
  let result2: Result<Int, Err> = .failure(.err)

  expectEqual(result1, .success(1))
  expectNotEqual(result1, .success(2))
  expectNotEqual(result1, .failure(.err))
  expectNotEqual(result1, .failure(.derr))

  expectNotEqual(result2, .success(1))
  expectNotEqual(result2, .success(2))
  expectEqual(result2, .failure(.err))
  expectNotEqual(result2, .failure(.derr))
  
  let confusables: [Result<Err, Err>] = [
    .success(.err),
    .success(.derr),
    .failure(.err),
    .failure(.derr)
  ]
  
  checkEquatable(confusables, oracle: { $0 == $1 })
}

ResultTests.test("Hashable") {
  let result1: Result<Int, Err> = .success(1)
  let result2: Result<Int, Err> = .success(2)
  let result3: Result<Int, Err> = .failure(.err)
  checkHashable([result1, result2, result3], equalityOracle: { $0 == $1 })

  let confusables: [Result<Err, Err>] = [
    .success(.err),
    .success(.derr),
    .failure(.err),
    .failure(.derr)
  ]
  checkHashable(confusables, equalityOracle: { $0 == $1 })
}

runAllTests()
