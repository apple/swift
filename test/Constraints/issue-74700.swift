// RUN: %target-typecheck-verify-swift

protocol Idable<ID> {
  associatedtype ID
  var id: ID { get }
}
func test74700() {
  @dynamicMemberLookup struct Binding<C> {
    subscript(dynamicMember member: String) -> String { "" }
  }
  struct ForEach<Data, ID, Content> where Data : RandomAccessCollection, ID: Hashable {
    init(_ data: Data, content: (Data.Element) -> Content) where ID == Data.Element.ID, Data.Element : Idable {}
    // expected-note@-1 {{where 'Data.Element' = 'User'}}
    init<C>(_ data: Binding<C>, content: (Binding<C.Element>) -> Content) where Data == Array<C>, ID == C.Element.ID, C : MutableCollection, C : RandomAccessCollection, C.Element : Idable, C.Index : Hashable {}
  }

  struct User {
    let name: String // expected-note {{'name' declared here}}
  }

  struct ContentView {
    let users: [User] = []
    func body() -> Void {
      ForEach(users) { user in // expected-error {{initializer 'init(_:content:)' requires that 'User' conform to 'Idable'}}
        return user.nam // expected-error {{value of type 'User' has no member 'nam'; did you mean 'name'?}}
      }
    }
  }
}
