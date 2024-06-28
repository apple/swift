// RUN: %empty-directory(%t)
// RUN: split-file %s %t
// RUN: %target-build-swift -I %S/Inputs %t/main.swift %t/second.swift -cxx-interoperability-mode=default -Xfrontend -disable-availability-checking

// XFAIL: OS=windows-msvc

//--- main.swift
import ReferenceCounted

protocol P1 {}
extension GlobalCount : P1 {}

//--- second.swift
import ReferenceCounted

protocol P2 {}
extension GlobalCount : P2 {}
