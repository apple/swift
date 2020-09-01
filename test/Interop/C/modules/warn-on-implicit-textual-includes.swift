// RUN: %target-typecheck-verify-swift -I %S/Inputs

import FakeGlibc
import WarnOnImplicitTextualIncludes

// Calling a function from a modularized header (whether qualified or
// unqualified) is fine.
_ = userFunction()
_ = RejectTextualIncludes.userFunction()

// Calling `fakeCos` without qualification is OK. The declaration comes from a
// textually included header, but `FakeGlibc` has an `export *` statement.
_ = fakeCos(0.0)
_ = FakeGlibc.fakeCos(0.0)

// Calling `fakeCos` with a `RejectTextualIncludes` qualification is not OK
// because the definition comes from a textually included header and
// `RejectTextualIncludes` does not contain an `export *` statement.
_ = RejectTextualIncludes.fakeCos(0.0)
