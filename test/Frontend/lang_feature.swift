// Check that hasFeature and the legacy $<Feature> both succeed for promoted
// language features.

// RUN: %target-swift-frontend -typecheck %s

#if !hasFeature(AsyncAwait)
#error("boom") // expected-error{{'boom'}}
#endif

#if !$AsyncAwait
#error("boom") // expected-error{{'boom'}}
#endif
