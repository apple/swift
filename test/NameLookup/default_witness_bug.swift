// RUN: %target-run-simple-swift(-D BAD) | %FileCheck %s
// RUN: %target-run-simple-swift | %FileCheck %s

// REQUIRES: executable_test

public struct Logger {
  var name: String = "bob" // to prevent rematerialization
 }

public protocol NetworkProtocol {
  var defaultLogger: Logger? { get }
  func send(logger: Logger?)
}

extension NetworkProtocol {
    public var defaultLogger: Logger? { nil } // default witness
}

struct Implementation<Client: NetworkProtocol>: NetworkProtocol {
  let defaultLogger: Logger // does NOT witness NetworkProtocol.defaultLogger
  let client: Client

  func send(logger: Logger?) {
    #if BAD
      // !! will access: NetworkProtocol.defaultLogger
      self.client.send(logger: logger ?? self.defaultLogger)
    #else
      // !! will access: Implementation.defaultLogger
      let arg = logger ?? self.defaultLogger
      self.client.send(logger: arg)
    #endif
  }
}

struct Outputter: NetworkProtocol {
  func send(logger maybeLogger: Logger?) {
    guard let logger = maybeLogger else {
      print("nil")
      return
    }
    print(logger.name)
  }
}

let impl = Implementation(defaultLogger: Logger(), client: Outputter())
impl.send(logger: nil)

// CHECK: bob
