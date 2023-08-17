public struct OtherStruct {
  public internal(set) static var staticProp = 123
  // expected-note@-1 {{setter for static property 'staticProp' is not '@usableFromInline' or public}}
}
