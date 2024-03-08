// RUN: %empty-directory(%t)
// RUN: cd %t
// RUN: %target-build-swift %S/Inputs/same-name-lib.swift -emit-module -emit-library -module-name SameNameLib
// RUN: %target-build-swift %s -I %S/Inputs -Xfrontend -enable-cxx-interop -o %t/run -I %t/ -L %t/ -lSameNameLib

import SameName
import SameNameLib

public func test(x: X) { }
