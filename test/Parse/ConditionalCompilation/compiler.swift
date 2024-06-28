// RUN: %target-typecheck-verify-swift
// RUN: %target-typecheck-verify-swift -swift-version 4

#if !compiler(>=4.1)
 // There should be no error here.
 foo bar
#else
 let _: Int = 1
#endif

#if compiler(<4.1)
 // There should be no error here.
 foo bar
#else
 let _: Int = 1
#endif

#if (compiler(>=4.1))
 let _: Int = 1
#else
 // There should be no error here.
 foo bar
#endif

#if !compiler(<4.1)
 let _: Int = 1
#else
 // There should be no error here.
 foo bar
#endif

#if compiler(>=4.1)
 let _: Int = 1
#else
 #if false
  // There should be no error here.
  foo bar
 #endif
#endif

#if false
 #if compiler(>=4.1)
  let _: Int = 1
 #else
  // There should be no error here.
  foo bar
 #endif
#endif
