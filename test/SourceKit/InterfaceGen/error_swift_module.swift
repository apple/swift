// RUN: %empty-directory(%t/Inputs)
// RUN: split-file %s %t/Inputs

//--- Transitive.swift

public func foo() {}

//--- Library.swift

import Transitive

//--- LibraryWrong.swift

import WrongName

//--- LibraryNonExistant.swift

import NonExistant

// RUN: %target-swift-frontend -emit-module %t/Inputs/Transitive.swift -module-name Transitive -o %t/WrongName.swiftmodule
// RUN: %target-swift-frontend -emit-module %t/Inputs/Transitive.swift -module-name Transitive -o %t/Transitive.swiftmodule

// First try printing the interface of the Transitive module directly.

// RUN: %sourcekitd-test -req=interface-gen -module Transitive -- -I %t -target %target-triple %s | %FileCheck --check-prefix DIRECT-SUCCESS %s
// DIRECT-SUCCESS: public func foo()

// RUN: not %sourcekitd-test -req=interface-gen -module WrongName -- -I %t -target %target-triple %s 2>&1 | %FileCheck --check-prefix DIRECT-FAIL %s
// DIRECT-FAIL: Could not load module: WrongName (cannot load module 'Transitive' as 'WrongName')

// Now try doing it transitively

// First undo the WrongName module
// RUN: %target-swift-frontend -emit-module %t/Inputs/Transitive.swift -module-name WrongName -o %t/WrongName.swiftmodule

// RUN: %target-swift-frontend -emit-module %t/Inputs/Library.swift -I %t -module-name Library -o %t
// RUN: %target-swift-frontend -emit-module %t/Inputs/LibraryWrong.swift -I %t -module-name LibraryWrong -o %t

// Then redo the WrongName module
// RUN: %target-swift-frontend -emit-module %t/Inputs/Transitive.swift -module-name Transitive -o %t/WrongName.swiftmodule

// RUN: %sourcekitd-test -req=interface-gen -module Library -- -I %t -target %target-triple %s | %FileCheck --check-prefix TRANSITIVE-SUCCESS %s
// TRANSITIVE-SUCCESS: import Transitive

// RUN: not %sourcekitd-test -req=interface-gen -module LibraryWrong -- -I %t -target %target-triple %s 2>&1 | %FileCheck --check-prefix TRANSITIVE-FAIL %s
// TRANSITIVE-FAIL: Could not load module: LibraryWrong (cannot load module 'Transitive' as 'WrongName')

// Try a non-existant module

// RUN: not %sourcekitd-test -req=interface-gen -module NonExistant -- -I %t -target %target-triple %s 2>&1 | %FileCheck --check-prefix DIRECT-NONEXISTANT %s
// DIRECT-NONEXISTANT: Could not load module: NonExistant

// RUN: %target-swift-frontend -emit-module %t/Inputs/Transitive.swift -module-name NonExistant -o %t
// RUN: %target-swift-frontend -emit-module %t/Inputs/LibraryNonExistant.swift -module-name LibraryNonExistant -I %t -o %t
// RUN: rm -rf %t/NonExistant.swiftmodule

// RUN: not %sourcekitd-test -req=interface-gen -module LibraryNonExistant -- -I %t -target %target-triple %s 2>&1 | %FileCheck --check-prefix TRANSITIVE-NONEXISTANT %s
// TRANSITIVE-NONEXISTANT: Could not load module: LibraryNonExistant (missing required module 'NonExistant')
