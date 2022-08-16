// RUN: %target-typecheck-verify-swift

class Dictionary<K, V> : ExpressibleByDictionaryLiteral { // expected-error {{type 'Dictionary<K, V>' does not conform to protocol 'ExpressibleByDictionaryLiteral'}} 
  typealias Key = K
  typealias Value = V
  init(dictionaryLiteral xs: K...){} // expected-note {{candidate has non-matching type '(dictionaryLiteral: K...)'}}
}

func useDict<K, V>(_ d: Dictionary<K,V>) {}

useDict(["Hello" : 1])
useDict(["Hello" : 1, "World" : 2])

