// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend %S/Inputs/use-cxx-stdlib-impl-only.swift -I %S/Inputs/ -module-name UseCxxStdlibImplOnly -emit-module -emit-module-path %t/UseCxxStdlibImplOnly.swiftmodule -cxx-interoperability-mode=default -enable-library-evolution

// RUN: %target-swift-frontend %s -typecheck -module-name TestMod -I %t -I %S/Inputs

// REQUIRES: OS=macosx || OS=linux-gnu

import UseCxxStdlibImplOnly

public func testCallsAPI() {
    testUsesCxxStdlib()
}
