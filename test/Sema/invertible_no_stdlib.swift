// RUN: %target-typecheck-verify-swift -enable-builtin-module \
// RUN:   -parse-stdlib -module-name Ghost



// This test covers only rudimentary typechecking when only the Builtin module
// is available. It's not a fully-supported configuration!

import Builtin

func reqCopy1<T>(_ t: T) {} // expected-note {{'where T: Builtin.Copyable' is implicit here}}
func reqCopy2<T: Builtin.Copyable>(_ t: T) {} // expected-note {{'where T: Builtin.Copyable' is implicit here}}

protocol P {}

struct DataType: P, Builtin.Escapable {} // expected-error {{type 'Escapable' requires -enable-experimental-feature NonescapableTypes}}
struct DataTypeNC: ~Builtin.Copyable {}

func main() {
    reqCopy1(DataTypeNC()) // expected-error {{global function 'reqCopy1' requires that 'DataTypeNC' conform to 'Builtin.Copyable'}}
    reqCopy2(DataTypeNC()) // expected-error {{global function 'reqCopy2' requires that 'DataTypeNC' conform to 'Builtin.Copyable'}}
    reqCopy1(DataType())
    reqCopy2(DataType())
}
