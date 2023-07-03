// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t %S/Inputs/HasObservable.swift
// RUN: %target-swift-frontend -emit-module -o %t %S/Inputs/ReexportsObservation.swift
// RUN: %target-swift-frontend -typecheck %s -I %t -verify
// RUN: %target-swift-frontend -typecheck %s -I %t -verify -DVIA_REEXPORT

// REQUIRES: observation

import HasObservable

#if VIA_REEXPORT
import ReexportsObservation
#else
import Observation
#endif

func foo() -> Observable<Int> {
  return .just(42)
}
