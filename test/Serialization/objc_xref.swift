// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-module -o %t %S/Inputs/def_objc_xref.swift
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %t -typecheck %s -verify

// REQUIRES: objc_interop

import def_objc_xref

// Trigger deserialization of the MyObjectFactorySub initializer.
let sub = MyObjectFactorySub()
