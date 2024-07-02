// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -typecheck -verify %s
// REQUIRES: objc_interop

import Foundation

@objc class A : NSObject {
  func uniqueID() -> Int {
    42
  }
}

struct Loop< // expected-note {{required by generic struct 'Loop' where 'ID' = '() -> Int'}}
  Data : RandomAccessCollection,
  ID : Hashable,
  Content
> {
  public init(
    _ data: Data,
    id: KeyPath<Data.Element, ID>,
    content: @escaping (Data.Element) -> Content) {}
}

func data() -> [A] {
  return []
}

_ = Loop(data(), id: \.uniqueID) { $0 } // expected-error {{key path cannot refer to instance method 'uniqueID()'}}
// expected-error@-1 {{type '() -> Int' cannot conform to 'Hashable'}} note@-1 {{only concrete types such as structs, enums and classes can conform to protocols}}
// expected-note@-2 {{only concrete types such as structs, enums and classes can conform to protocols}}