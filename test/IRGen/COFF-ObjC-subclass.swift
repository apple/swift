// RUN: %swift -target thumbv7-unknown-windows-msvc -parse-stdlib -enable-objc-interop -disable-objc-attr-requires-foundation-module -I %S/Inputs/usr/include -emit-ir %s -o - | %FileCheck %s

// REQUIRES: OS=windows

import SRoA

class C : I {
}

