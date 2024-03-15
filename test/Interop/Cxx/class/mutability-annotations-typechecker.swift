// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -typecheck -I %S/Inputs %s -enable-experimental-cxx-interop -verify -verify-additional-file %S/Inputs/mutability-annotations.h

import MutabilityAnnotations

let obj = HasConstMethodAnnotatedAsMutating(a: 42) // expected-note {{change 'let' to 'var' to make it mutable}}
let i = obj.annotatedMutating() // expected-error {{cannot use mutating member on immutable value: 'obj' is a 'let' constant}}

let objWMutableProperty = HasMutableProperty(a: 42, b: 21) // expected-note {{change 'let' to 'var' to make it mutable}}
// TODO-note@-1 {{change 'let' to 'var' to make it mutable}}

let _ = objWMutableProperty.annotatedNonMutating()
let _ = objWMutableProperty.noAnnotation() // TODO-error {{cannot use mutating member on immutable value: 'objWMutableProperty' is a 'let' constant}}
let _ = objWMutableProperty.contradictingAnnotations() // expected-error {{cannot use mutating member on immutable value: 'objWMutableProperty' is a 'let' constant}}
let _ = objWMutableProperty.duplicateAnnotations()

let objWithoutMutableProperty = NoMutableProperty(a: 42) // expected-note {{change 'let' to 'var' to make it mutable}}
let _ = objWithoutMutableProperty.isConst()
let _ = objWithoutMutableProperty.nonConst() // expected-error {{cannot use mutating member on immutable value: 'objWithoutMutableProperty' is a 'let' constant}}