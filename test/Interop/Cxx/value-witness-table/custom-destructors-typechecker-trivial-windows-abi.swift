// RUN: %target-typecheck-verify-swift -I %S/Inputs -enable-experimental-cxx-interop -Xcc -DWIN_TRIVIAL

// REQUIRES: OS=windows-msvc

import CustomDestructor

_ = HasUserProvidedDestructor() // expected-error {{non-trivial C++ class with trivial ABI is not yet available in Swift}}
_ = HasEmptyDestructorAndMemberWithUserDefinedConstructor() // expected-error {{non-trivial C++ class with trivial ABI is not yet available in Swift}}
_ = HasNonTrivialImplicitDestructor() // expected-error {{non-trivial C++ class with trivial ABI is not yet available in Swift}}
_ = HasNonTrivialDefaultedDestructor() // expected-error {{non-trivial C++ class with trivial ABI is not yet available in Swift}}
