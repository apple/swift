// RUN: %target-typecheck-verify-swift -swift-version 6

func doStuff(_ fn : @escaping () -> Int) {}
func doVoidStuff(_ fn : @escaping () -> ()) {}
func doVoidStuffNonEscaping(_ fn: () -> ()) {}

class ExplicitSelfRequiredTest {
  var x = 42
  func method() -> Int {
    doVoidStuff({ doStuff({ x+1 })}) // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{18-18= [self] in}} expected-note{{reference 'self.' explicitly}} {{29-29=self.}}
    doVoidStuff({ [self] in doStuff({ x+1 })}) // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{38-38= [self] in}} expected-note{{reference 'self.' explicitly}} {{39-39=self.}}
    return 42
  }
  
  func weakSelfError() {
    doVoidStuff({ [weak self] in x += 1 }) // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
    doVoidStuffNonEscaping({ [weak self] in x += 1 }) // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
    doStuff({ [weak self] in x+1 }) // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
    doVoidStuff({ [weak self] in _ = method() }) // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note{{reference 'self?.' explicitly}}
    doVoidStuffNonEscaping({ [weak self] in _ = method() }) // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note{{reference 'self?.' explicitly}}
    doStuff({ [weak self] in method() }) // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note{{reference 'self?.' explicitly}}
  }
}

// https://github.com/apple/swift/issues/56501
class C_56501 {
  func operation() {}

  func test1() {
    doVoidStuff { [self] in
      operation()
    }
  }

  func test2() {
    doVoidStuff { [self] in
      doVoidStuff {
        // expected-error@+3 {{call to method 'operation' in closure requires explicit use of 'self'}}
        // expected-note@-2 {{capture 'self' explicitly to enable implicit 'self' in this closure}}
        // expected-note@+1 {{reference 'self.' explicitly}}
        operation()
      }
    }
  }

  func test3() {
    doVoidStuff { [self] in
      doVoidStuff { [self] in
        operation()
      }
    }
  }

  func test4() {
    doVoidStuff { [self] in
      doVoidStuff {
        self.operation()
      }
    }
  }

  func test5() {
    doVoidStuff { [self] in
      doVoidStuffNonEscaping {
        operation()
      }
    }
  }

  func test6() {
    doVoidStuff { [self] in
      doVoidStuff { [self] in
        doVoidStuff {
          // expected-error@+3 {{call to method 'operation' in closure requires explicit use of 'self'}}
          // expected-note@-2 {{capture 'self' explicitly to enable implicit 'self' in this closure}}
          // expected-note@+1 {{reference 'self.' explicitly}}
          operation()
        }
      }
    }
  }
}

func takesEscapingWithAllowedImplicitSelf(@_implicitSelfCapture _ fn: @escaping () -> Void) {}

public final class TestImplicitSelfForWeakSelfCapture: Sendable {
  static let staticOptional: TestImplicitSelfForWeakSelfCapture? = .init()
  func method() { }
  
  private init() {
    doVoidStuff { [weak self] in
      method() // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
      guard let self = self else { return }
      method()
    }

    doVoidStuff { [weak self] in
      guard let self else { return }
      method()
    }
    
    doVoidStuff { [weak self] in
      if let self = self {
        method()
      }

      if let self {
        method()
      }
    }
    
    doVoidStuff { [weak self] in
      guard let self = self ?? TestImplicitSelfForWeakSelfCapture.staticOptional else { return }
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }

    let otherValue = TestImplicitSelfForWeakSelfCapture.staticOptional ?? self
    doVoidStuff { [weak self = otherValue] in
      guard let self else { return }
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }
    
    doVoidStuffNonEscaping { [weak self] in
      method() // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
      guard let self = self else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      if let self = self {
        method()
      }
    }

    takesEscapingWithAllowedImplicitSelf { [weak self] in
      method() // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
      guard let self = self else { return }
      method()
    }

    doVoidStuff { [weak self] in
      doVoidStuff { // expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}}
        guard let self = self else { return }
        method() // expected-error{{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{reference 'self.' explicitly}}
      }
    }
    
    doVoidStuff { [weak self] in
      let `self`: TestImplicitSelfForWeakSelfCapture? = self ?? TestImplicitSelfForWeakSelfCapture.staticOptional
      guard let self = self else { return }
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }
    
    doVoidStuffNonEscaping { [weak self] in
      let `self`: TestImplicitSelfForWeakSelfCapture? = self ?? TestImplicitSelfForWeakSelfCapture.staticOptional
      guard let self = self else { return }
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard let self = self else { return }
      doVoidStuff { // expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}}
        method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
      }
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard let self = self ?? TestImplicitSelfForWeakSelfCapture.staticOptional else { return }
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }
    
    doVoidStuff { [weak self] in
      func innerFunction1() {
          method() // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
          self?.method()
      }
      
      guard let self else { return }
      
      func innerFunction2() {
          method()
          self.method()
      }

      doVoidStuff { [self] in
        method()
        self.method()
      }
    }
    
    doVoidStuffNonEscaping { [weak self] in
      func innerFunction1() {
          method() // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
          self?.method()
      }
      
      guard let self else { return }
      
      func innerFunction2() {
          method()
          self.method()
      }
    }
  }
}

class NoImplicitSelfInInnerClass {
  func method() { }
  
  private init() { // expected-note {{'self' declared here}} expected-note {{'self' declared here}}
    doVoidStuff {
      class InnerType { // expected-note {{type declared here}}
          func functionInsideInnerType() {
            method() // expected-error {{class declaration cannot close over value 'self' defined in outer scope}}
            self.method() // expected-error {{value of type 'InnerType' has no member 'method'}}
          }
      }
    }
    
    doVoidStuff { [weak self] in
      guard let self else { return }
      method()
      
      class InnerType { // expected-note {{type declared here}}
          func functionInsideInnerType() {
            method() // expected-error {{class declaration cannot close over value 'self' defined in outer scope}}
            self.method() // expected-error {{value of type 'InnerType' has no member 'method'}}
          }
      }
    }
  }
  
  func foo(condition: Bool) {
    doVoidStuff { [weak self] in
      guard condition, let self else { return }
      method()
    }
    
    doVoidStuff { [weak self] in
      guard let self, condition else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard condition, let self else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard let self, condition else { return }
      method()
    }
  }

  func foo(optionalCondition: Bool?) {
    doVoidStuff { [weak self] in
      guard let optionalCondition, optionalCondition, let self else { return }
      method()
    }
    
    doVoidStuff { [weak self] in
      guard let self, let optionalCondition, optionalCondition else { return }
      method()
    }
    
    doVoidStuff { [weak self] in
      guard let optionalCondition, let self, optionalCondition else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard let optionalCondition, optionalCondition, let self else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard let self, let optionalCondition, optionalCondition else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard let optionalCondition, let self, optionalCondition else { return }
      method()
    }
  }
  
  func foo() {
    doVoidStuff { [weak self] in
      guard #available(SwiftStdlib 5.8, *), let self else { return }
      method()
    }
    
    doVoidStuff { [weak self] in
      guard let self, #available(SwiftStdlib 5.8, *) else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard #available(SwiftStdlib 5.8, *), let self else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard let self, #available(SwiftStdlib 5.8, *) else { return }
      method()
    }
  }
}

public class TestRebindingSelfIsDisallowed {
  let count: Void = ()
  
  private init() {
    doVoidStuff {
      let `self` = "self shouldn't become a string"
      let _: Int = count // expected-error{{cannot convert value of type 'Void' to specified type 'Int'}}
    }
    
    doVoidStuffNonEscaping {
      let `self` = "self shouldn't become a string"
      let _: Int = count // expected-error{{cannot convert value of type 'Void' to specified type 'Int'}}
    }
    
    doVoidStuff { [weak self] in
      let `self` = "self shouldn't become a string"
      let _: Int = count // expected-error{{cannot convert value of type 'Void' to specified type 'Int'}}
    }
    
    doVoidStuffNonEscaping { [weak self] in
      let `self` = "self shouldn't become a string"
      let _: Int = count // expected-error{{cannot convert value of type 'Void' to specified type 'Int'}}
    }
  }
  
  func method() {
    let `self` = "self shouldn't become a string"
    let _: Int = count // expected-error{{cannot convert value of type 'Void' to specified type 'Int'}}
  }

  func testTypeNamedSelf() {
    struct `self` {
      static func staticMember() {}
    }

    doVoidStuff {
      staticMember() // expected-error{{cannot find 'staticMember' in scope}}
      self.staticMember()
    }

    doVoidStuffNonEscaping {
      staticMember() // expected-error{{cannot find 'staticMember' in scope}}
      self.staticMember()      
    }

    doVoidStuff { [weak self] in
      staticMember() // expected-error{{cannot find 'staticMember' in scope}}
      self.staticMember()
    }

    doVoidStuffNonEscaping { [weak self] in
      staticMember() // expected-error{{cannot find 'staticMember' in scope}}
      self.staticMember()
    }
  }
}

class TestGithubIssue70089 {
    var x: Int = 0
    nonisolated(unsafe) static let staticOptional: TestGithubIssue70089? = .init()
    func method() { }

    func f() {
      doVoidStuff { [weak self] in
        guard let self else { return }

        doVoidStuff { [self] in
          x += 1
        }

        doVoidStuff { // expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}}
          x += 1 // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
        }

        doVoidStuff { [self = TestGithubIssue70089()] in // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}} expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}} expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}}
          x += 1 // expected-error{{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
          self.x += 1

          doVoidStuff {
            x += 1  // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
            self.x += 1
          }

          doVoidStuff { [self] in
            x += 1  // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
            self.x += 1
          }
        }

        doVoidStuff { // expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}}
          doVoidStuff {
            x += 1  // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
            self.x += 1
          }
        }

        doVoidStuff { [self] in
          doVoidStuff { // expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}}
            x += 1 // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
            self.x += 1
          }
        }

        doVoidStuff {
          doVoidStuff { [self] in
            x += 1
            self.x += 1
          }
        }
      }

      doVoidStuff { [weak self] in
        doVoidStuff {
          x += 1 // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note{{reference 'self?.' explicitly}}
          self?.x += 1
        }
      }

      doVoidStuff { [weak self] in
        doVoidStuffNonEscaping {
          x += 1 // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note{{reference 'self?.' explicitly}}
          self?.x += 1
        }
      }

      doVoidStuff { [weak self] in
        // Since this unwrapping is invalid, implicit self is disallowed in all nested closures:
        guard let self = self ?? TestGithubIssue70089.staticOptional else { return }
        
        doVoidStuff { [self] in
          x += 1 // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
          self.x += 1
        }

        doVoidStuffNonEscaping {
          x += 1 // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
          self.x += 1
        }
      }
      
      doVoidStuff { [self = TestGithubIssue70089()] in // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}}
        doVoidStuff { [self] in
          x += 1 // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
          self.x += 1
        }
      }
    }
}

class TestGithubIssue69911 {
    var x: Int = 0
    nonisolated(unsafe) static let staticOptional: TestGithubIssue69911? = .init()

    func f() {
      doVoidStuff { [weak self] in
        guard let self else { return }

        doVoidStuffNonEscaping {
          x += 1
          self.x += 1
        }

        doVoidStuffNonEscaping {
          doVoidStuffNonEscaping {
            x += 1
            self.x += 1
          }
        }

        doVoidStuff { [self] in
          doVoidStuffNonEscaping {
            x += 1
            self.x += 1
          }
        }

        doVoidStuffNonEscaping { [self] in
          doVoidStuffNonEscaping { [self] in
            x += 1
            self.x += 1
          }
        }

        doVoidStuffNonEscaping { [self = TestGithubIssue69911()] in // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}} expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}} expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}}
           x += 1 // expected-error{{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
           self.x += 1

           doVoidStuff { [self] in
             x += 1 // expected-error{{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
             self.x += 1
           }

           doVoidStuffNonEscaping {
             x += 1 // expected-error{{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
             self.x += 1
           }
        }
      }

      doVoidStuff { [weak self] in
        doVoidStuffNonEscaping { [weak self] in
          x += 1 // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
          self?.x += 1
        }
      }

      doVoidStuff { [weak self] in
        // Since this unwrapping is invalid, implicit self is disallowed in all nested closures:
        guard let self = self ?? TestGithubIssue69911.staticOptional else { return }

        doVoidStuffNonEscaping {
          x += 1 // expected-error{{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
          self.x += 1
        }

        doVoidStuffNonEscaping {
          doVoidStuffNonEscaping {
            x += 1 // expected-error{{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
            self.x += 1
          }
        }

        doVoidStuff { [self] in
          doVoidStuffNonEscaping {
            x += 1 // expected-error{{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
            self.x += 1
          }
        }

        doVoidStuffNonEscaping { [self] in
          doVoidStuffNonEscaping { [self] in
            x += 1 // expected-error{{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
            self.x += 1
          }
        }

        doVoidStuffNonEscaping { [self = TestGithubIssue69911()] in // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}}
           x += 1 // expected-error{{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
           self.x += 1
        }

        doVoidStuff { [self] in
          doVoidStuffNonEscaping {
            doVoidStuffNonEscaping {
              doVoidStuff { [weak self] in
                self?.x += 1
                guard let self else { return }
                x += 1 // expected-error{{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
                self.x += 1
              }
            }
          }
        }
      }
    }
}

func withNonEscapingAutoclosure<T>(_ x: @autoclosure () -> T) {}
func withEscapingAutoclosure<T>(_ x: @escaping @autoclosure () -> T) {}

final class AutoclosureTests {
  func bar() -> Bool { true }
  func method() { }

  func foo(_ x: AutoclosureTests) {
    withNonEscapingAutoclosure(bar())
    withEscapingAutoclosure(bar()) // expected-error {{call to method 'bar' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
    
    doVoidStuff { [self] in
      withNonEscapingAutoclosure(bar())
      withEscapingAutoclosure(bar()) // expected-error {{call to method 'bar' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
      
      doVoidStuff { // expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}}
        withNonEscapingAutoclosure(bar()) // expected-error {{call to method 'bar' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
      }
      
      doVoidStuffNonEscaping {
        withNonEscapingAutoclosure(bar())
      }
      
      doVoidStuffNonEscaping { [self] in
        withNonEscapingAutoclosure(bar())
      }
      
      doVoidStuff {
        withEscapingAutoclosure(bar()) // expected-error {{call to method 'bar' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
      }
      
      doVoidStuffNonEscaping {
        withEscapingAutoclosure(bar()) // expected-error {{call to method 'bar' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
      }
      
      doVoidStuffNonEscaping { [self] in
        withEscapingAutoclosure(bar()) // expected-error {{call to method 'bar' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
      }
    }
    
    doVoidStuff { [weak self] in
      withNonEscapingAutoclosure(bar()) // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
    }
    
    doVoidStuff { [weak self] in
      withEscapingAutoclosure(bar()) // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
    }
    
    doVoidStuff { [weak self] in
      doVoidStuff {
        withNonEscapingAutoclosure(bar()) // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
      }
    }
      
    doVoidStuff { [weak self] in
      doVoidStuff {
        withEscapingAutoclosure(bar()) // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
      }
    }

    doVoidStuff { [weak self] in
      doVoidStuff { [self] in
        withNonEscapingAutoclosure(bar()) // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
      }
    }
      
    doVoidStuff { [weak self] in
      doVoidStuff { [self] in
        withEscapingAutoclosure(bar()) // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
      }
    }

    doVoidStuff { [weak self] in
      guard let self else { return }
      
      withNonEscapingAutoclosure(bar())
      withEscapingAutoclosure(bar()) // expected-error {{call to method 'bar' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
      
      doVoidStuff { // expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}}
        withNonEscapingAutoclosure(bar()) // expected-error {{call to method 'bar' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
      }
      
      doVoidStuffNonEscaping {
        withNonEscapingAutoclosure(bar())
      }
      
      doVoidStuffNonEscaping { [self] in
        withNonEscapingAutoclosure(bar())
      }
      
      doVoidStuff {
        withEscapingAutoclosure(bar()) // expected-error {{call to method 'bar' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
      }
      
      doVoidStuffNonEscaping {
        withEscapingAutoclosure(bar()) // expected-error {{call to method 'bar' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
      }
      
      doVoidStuffNonEscaping { [self] in
        withEscapingAutoclosure(bar()) // expected-error {{call to method 'bar' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
      }
    }

    doVoidStuff { [weak self] in
      doVoidStuff { [self] in
        guard let self else { return }
        method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
      }
    }
  
    doVoidStuff { [weak self] in
      let someOptional: Self? = Self()
      let `self` = self ?? someOptional
      guard let self = self else { return }
      method() // expected-error{{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }
  
    doVoidStuff {
      let someOptional: Self? = Self()
      guard case let self = someOptional else { return }
      method() // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
    }

    doVoidStuff { [weak self] in
      let someOptional: Self? = Self()
      var `self` = self ?? someOptional
      guard let self = self else { return }
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}

      doVoidStuff {
        method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
      }

      doVoidStuff { [self] in
        method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
      }
    }

    doVoidStuff { [weak self] in
      doVoidStuff { [self] in
        guard let self else { return }
        method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
      }
    }

    doVoidStuff { [weak self] in
      guard let self = self else { return }
      doVoidStuff { // expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}}
        method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
      }
    }
  }
}

class TestInvalidRebindingOutsideOfClosure {
  func method() { }

  func testInvalidRebindingCondition() {
    guard case let self = TestInvalidRebindingOutsideOfClosure() else { return } // expected-warning {{'guard' condition is always true, body is unreachable}}

    doVoidStuff { [self] in // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}}
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }

    doVoidStuffNonEscaping { [self] in // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}}
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }
    
    doVoidStuff() { [weak self] in
      guard let self else { return }
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }

    doVoidStuffNonEscaping() { [weak self] in
      guard let self else { return }
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }
  }

  func testInvalidSelfWithBackticks() {
    let `self` = TestInvalidRebindingOutsideOfClosure()

    doVoidStuff { [self] in // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}}
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }

    doVoidStuffNonEscaping { [self] in // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}}
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }
    
    doVoidStuff() { [weak self] in
      guard let self else { return }
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }

    doVoidStuffNonEscaping() { [weak self] in
      guard let self else { return }
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }
  }

  func testInvalidSelfWithBackticks2() {
    let `self` = self

    doVoidStuff { [self] in // expected-note{{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}}
      method() // expected-error{{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }

    // Allowed in Swift 5 mode for source compatibility:
    doVoidStuffNonEscaping { [self] in // expected-note{{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}}
      method() // expected-error{{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }
    
    doVoidStuff() { [weak self] in
      guard let self else { return }
      method() // expected-error{{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }

    doVoidStuffNonEscaping() { [weak self] in
      guard let self else { return }
      method() // expected-error{{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }
  }
}

struct TestInvalidSelfCaptureInStruct {
  func method() { }

  func bar() {
    // To maintain source compatibility, we continue allowing this in Swift 5 mode:
    doVoidStuff { [self = TestInvalidSelfCaptureInStruct()] in // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}}
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
      self.method()
    }

    doVoidStuffNonEscaping { [self = TestInvalidSelfCaptureInStruct()] in // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}}
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
      self.method()
    }

    doVoidStuffNonEscaping { [self = TestInvalidSelfCaptureInStruct()] in // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}}
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
      self.method()
    }
  }
}

// rdar://129475277
class rdar129475277 {
  func bar() -> Int { 0 }
  func method() {}

  func test1() {
    takesEscapingWithAllowedImplicitSelf { [weak self] in
      takesEscapingWithAllowedImplicitSelf {
        method() // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
      }
    }

    takesEscapingWithAllowedImplicitSelf { [weak self] in
      takesEscapingWithAllowedImplicitSelf {
        doVoidStuffNonEscaping {
          withNonEscapingAutoclosure(bar()) // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
        }
      }
    }

    takesEscapingWithAllowedImplicitSelf { [weak self] in
      withNonEscapingAutoclosure(bar()) // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
    }
  }

  func test2() {
    guard case let self: rdar129475277? = nil else { return }
    // expected-warning@-1 {{'guard' condition is always true, body is unreachable}}
    doVoidStuffNonEscaping {
      method() // expected-error {{explicit use of 'self' is required when 'self' is optional, to make control flow explicit}} expected-note {{reference 'self?.' explicitly}}
    }
  }
}
