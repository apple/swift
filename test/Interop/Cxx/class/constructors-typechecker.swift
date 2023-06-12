// RUN: %target-typecheck-verify-swift -verify-ignore-unknown -I %S/Inputs -enable-experimental-cxx-interop

import Constructors

let explicit = ExplicitDefaultConstructor()

let implicit = ImplicitDefaultConstructor()

let deletedImplicitly = ConstructorWithParam() // expected-warning {{'init()' is deprecated}}

let deletedExplicitly = DefaultConstructorDeleted() // expected-error {{missing argument for parameter 'a' in call}}

let withArg = ConstructorWithParam(42)
