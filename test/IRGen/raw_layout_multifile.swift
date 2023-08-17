// RUN: %target-swift-frontend -enable-experimental-feature RawLayout -emit-ir -primary-file %s %S/Inputs/raw_layout_multifile_b.swift
// RUN: %target-swift-frontend -enable-experimental-feature RawLayout -emit-ir %s -primary-file %S/Inputs/raw_layout_multifile_b.swift

import Swift

@_rawLayout(like: Int32)
public struct Foo<T>: ~Copyable {}
