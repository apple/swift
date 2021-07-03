// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %S/Inputs/placement_module_A.swift -emit-module -parse-as-library -o %t
// RUN: %target-swift-frontend -I %t %S/Inputs/placement_module_B.swift -emit-module -parse-as-library -o %t

// RUN: %target-swift-frontend -typecheck -primary-file %s %S/Inputs/placement_2.swift -I %t -verify

// Tests for the placement of conformances as well as conflicts
// between conformances that come from different sources.

import placement_module_A
import placement_module_B

protocol P1 { }
protocol P2a : P1 { }
protocol P3a : P2a { }
protocol P2b : P1 { }
protocol P3b : P2b { }
protocol P4 : P3a, P3b { }

protocol AnyObjectRefinement : AnyObject { }

// ===========================================================================
// Tests within a single source file
// ===========================================================================

// ---------------------------------------------------------------------------
// Multiple explicit conformances to the same protocol
// ---------------------------------------------------------------------------
struct Explicit1 : P1 { } // expected-note{{'Explicit1' declares conformance to protocol 'P1' here}}
extension Explicit1 : P1 { } // expected-error{{redundant conformance of 'Explicit1' to protocol 'P1'}}

struct Explicit2 { }
extension Explicit2 : P1 { } // expected-note 2{{'Explicit2' declares conformance to protocol 'P1' here}}
extension Explicit2 : P1 { } // expected-error{{redundant conformance of 'Explicit2' to protocol 'P1'}}
extension Explicit2 : P1 { } // expected-error{{redundant conformance of 'Explicit2' to protocol 'P1'}}
 
// ---------------------------------------------------------------------------
// Multiple implicit conformances, with no ambiguities
// ---------------------------------------------------------------------------
struct MultipleImplicit1 : P2a { }
extension MultipleImplicit1 : P3a { }

struct MultipleImplicit2 : P4 { }

struct MultipleImplicit3 : P4 { }
extension MultipleImplicit3 : P1 { }

// ---------------------------------------------------------------------------
// Multiple implicit conformances, ambiguity resolved because they
// land in the same context.
// ---------------------------------------------------------------------------
struct MultipleImplicit4 : P2a, P2b { }

struct MultipleImplicit5 { }
extension MultipleImplicit5 : P2a, P2b { }

struct MultipleImplicit6 : P4 { }
extension MultipleImplicit6 : P3a { }

struct MultipleImplicit7 : P2a { }
extension MultipleImplicit7 : P2b { }

// ---------------------------------------------------------------------------
// Multiple implicit conformances, ambiguity resolved via explicit conformance
// ---------------------------------------------------------------------------
struct ExplicitMultipleImplicit1 : P2a { }
extension ExplicitMultipleImplicit1 : P2b { }
extension ExplicitMultipleImplicit1 : P1 { } // resolves ambiguity

// ---------------------------------------------------------------------------
// Implicit conformances superseded by inherited conformances
// ---------------------------------------------------------------------------
class ImplicitSuper1 : P3a { }

class ImplicitSub1 : ImplicitSuper1 { }

extension ImplicitSub1 : P4 { } // okay, introduces new conformance to P4; the rest are superseded

// ---------------------------------------------------------------------------
// Synthesized conformances superseded by implicit conformances
// ---------------------------------------------------------------------------

enum SuitA { case Spades, Hearts, Clubs, Diamonds }
func <(lhs: SuitA, rhs: SuitA) -> Bool { return false }
extension SuitA : Comparable {} // okay, implied conformance to Equatable here is preferred.

enum SuitB: Equatable { case Spades, Hearts, Clubs, Diamonds }
func <(lhs: SuitB, rhs: SuitB) -> Bool { return false }
extension SuitB : Comparable {} // okay, explicitly declared earlier.

enum SuitC { case Spades, Hearts, Clubs, Diamonds }
func <(lhs: SuitC, rhs: SuitC) -> Bool { return false }
extension SuitC : Equatable, Comparable {} // okay, explicitly declared here.

// ---------------------------------------------------------------------------
// Explicit conformances conflicting with inherited conformances
// ---------------------------------------------------------------------------

class ExplicitSuper1 : P3a { }

class ExplicitSub1 : ImplicitSuper1 { } // expected-note{{'ExplicitSub1' inherits conformance to protocol 'P1' from superclass here}}

extension ExplicitSub1 : P1 { } // expected-error{{redundant conformance of 'ExplicitSub1' to protocol 'P1'}}

// ---------------------------------------------------------------------------
// Suppression of synthesized conformances
// ---------------------------------------------------------------------------
class SynthesizedClass1 : AnyObject { } // expected-error{{class type cannot inherit from non-protocol, non-class type 'AnyObject'}}

class SynthesizedClass2 { }
extension SynthesizedClass2 : AnyObject { } // expected-error{{extension type cannot inherit from non-protocol type 'AnyObject'}}

class SynthesizedClass3 : AnyObjectRefinement { }

class SynthesizedClass4 { }
extension SynthesizedClass4 : AnyObjectRefinement { }

class SynthesizedSubClass1 : SynthesizedClass1, AnyObject { } // expected-error{{class type cannot inherit from non-protocol, non-class type 'AnyObject'}}

class SynthesizedSubClass2 : SynthesizedClass2 { }
extension SynthesizedSubClass2 : AnyObject { } // expected-error{{extension type cannot inherit from non-protocol type 'AnyObject'}}

class SynthesizedSubClass3 : SynthesizedClass1, AnyObjectRefinement { }

class SynthesizedSubClass4 : SynthesizedClass2 { }
extension SynthesizedSubClass4 : AnyObjectRefinement { }

enum SynthesizedEnum1 : Int, RawRepresentable { case none = 0 }

enum SynthesizedEnum2 : Int { case none = 0 }
extension SynthesizedEnum2 : RawRepresentable { }


// ===========================================================================
// Tests across different source files
// ===========================================================================

// ---------------------------------------------------------------------------
// Multiple explicit conformances to the same protocol
// ---------------------------------------------------------------------------
struct MFExplicit1 : P1 { }

extension MFExplicit2 : P1 { } // expected-error{{redundant conformance of 'MFExplicit2' to protocol 'P1'}}


// ---------------------------------------------------------------------------
// Multiple implicit conformances, with no ambiguities
// ---------------------------------------------------------------------------
extension MFMultipleImplicit1 : P3a { }

struct MFMultipleImplicit2 : P4 { }

extension MFMultipleImplicit3 : P1 { }

extension MFMultipleImplicit4 : P3a { }

extension MFMultipleImplicit5 : P2b { }

// ---------------------------------------------------------------------------
// Explicit conformances conflicting with inherited conformances
// ---------------------------------------------------------------------------

class MFExplicitSuper1 : P3a { }

extension MFExplicitSub1 : P1 { } // expected-error{{redundant conformance of 'MFExplicitSub1' to protocol 'P1'}}

// ---------------------------------------------------------------------------
// Suppression of synthesized conformances
// ---------------------------------------------------------------------------
class MFSynthesizedClass1 { }

extension MFSynthesizedClass2 : AnyObject { } // expected-error{{extension type cannot inherit from non-protocol type 'AnyObject'}}

class MFSynthesizedClass4 { }
extension MFSynthesizedClass4 : AnyObjectRefinement { }

extension MFSynthesizedSubClass2 : AnyObject { } // expected-error{{extension type cannot inherit from non-protocol type 'AnyObject'}}

extension MFSynthesizedSubClass3 : AnyObjectRefinement { }

class MFSynthesizedSubClass4 : MFSynthesizedClass2 { }

extension MFSynthesizedEnum1 : RawRepresentable { }

enum MFSynthesizedEnum2 : Int { case none = 0 }

// ===========================================================================
// Tests with conformances in imported modules
// ===========================================================================
extension MMExplicit1 : MMP1 { } // expected-warning{{conformance of 'MMExplicit1' to protocol 'MMP1' was already stated in the type's module 'placement_module_A'}}

extension MMExplicit1 : MMP2a { } // expected-warning{{MMExplicit1' to protocol 'MMP2a' was already stated in the type's module 'placement_module_A}}
extension MMExplicit1 : MMP3a { } // expected-warning{{conformance of 'MMExplicit1' to protocol 'MMP3a' was already stated in the type's module 'placement_module_A'}}

extension MMExplicit1 : MMP3b { } // okay

extension MMSuper1 : MMP1 { } // expected-warning{{conformance of 'MMSuper1' to protocol 'MMP1' was already stated in the type's module 'placement_module_A'}}
extension MMSuper1 : MMP2a { } // expected-warning{{conformance of 'MMSuper1' to protocol 'MMP2a' was already stated in the type's module 'placement_module_A'}}
extension MMSuper1 : MMP3b { } // okay

extension MMSub1 : AnyObject { } // expected-error{{extension type cannot inherit from non-protocol type 'AnyObject'}}

extension MMSub2 : MMP1 { } // expected-warning{{conformance of 'MMSub2' to protocol 'MMP1' was already stated in the type's module 'placement_module_A'}}
extension MMSub2 : MMP2a { } // expected-warning{{conformance of 'MMSub2' to protocol 'MMP2a' was already stated in the type's module 'placement_module_A'}}
extension MMSub2 : MMP3b { } // okay

extension MMSub2 : MMAnyObjectRefinement { } // okay

extension MMSub3 : MMP1 { } // expected-warning{{conformance of 'MMSub3' to protocol 'MMP1' was already stated in the type's module 'placement_module_A'}}
extension MMSub3 : MMP2a { } // expected-warning{{conformance of 'MMSub3' to protocol 'MMP2a' was already stated in the type's module 'placement_module_A'}}
extension MMSub3 : MMP3b { } // okay
extension MMSub3 : AnyObject { } // expected-error{{extension type cannot inherit from non-protocol type 'AnyObject'}}

extension MMSub4 : MMP1 { } // expected-warning{{conformance of 'MMSub4' to protocol 'MMP1' was already stated in the type's module 'placement_module_B'}}
extension MMSub4 : MMP2a { } // expected-warning{{conformance of 'MMSub4' to protocol 'MMP2a' was already stated in the type's module 'placement_module_B'}}
extension MMSub4 : MMP3b { } // okay
extension MMSub4 : AnyObjectRefinement { } // okay
