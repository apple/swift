// RUN: %target-typecheck-verify-swift

protocol P: _Copyable {} // expected-error {{'_Copyable' is unavailable}}
struct S: P {}

typealias PleaseLetMeDoIt = _Copyable // expected-error {{'_Copyable' is unavailable}}
typealias WhatIfIQualify = Swift._Copyable // expected-error {{'_Copyable' is unavailable}}

class C: _Copyable {} // expected-error {{'_Copyable' is unavailable}}

@_moveOnly struct MOStruct: _Copyable {}
// expected-error@-1 {{noncopyable struct 'MOStruct' cannot conform to '_Copyable'}}
// expected-error@-2 {{'_Copyable' is unavailable}}


func whatever<T>(_ t: T) where T: _Copyable {} // expected-error {{'_Copyable' is unavailable}}
func vatever<T: _Copyable>(_ t: T) {} // expected-error {{'_Copyable' is unavailable}}
func buttever(_ t: any _Copyable) {} // expected-error {{'_Copyable' is unavailable}}
func zuttever(_ t: some _Copyable) {} // expected-error 2{{'_Copyable' is unavailable}}

enum RockNRoll<T: _Copyable> { // expected-error {{'_Copyable' is unavailable}}
 case isNoisePollution(_Copyable) // expected-error {{'_Copyable' is unavailable}}
 case isMusic(T)
}

enum namespace {
  typealias _Copyable = Int

  func _Copyable() -> _Copyable { return 0 }
}
