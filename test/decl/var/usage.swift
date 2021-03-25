// RUN: %target-typecheck-verify-swift

func mutableParameter(_ a : Int, h : Int, var i : Int, j: Int, g: Int) -> Int { // expected-warning {{'var' in this position is interpreted as an argument label}} {{43-46=`var`}}
  i += 1 // expected-error {{left side of mutating operator isn't mutable: 'i' is a 'let' constant}}
  var j = j
  swap(&i, &j) // expected-error {{cannot pass immutable value as inout argument: 'i' is a 'let' constant}}
  return i+g
}
