// Continuous integration for the OS X Platform also runs the tests in the
// iPhone, Apple TV and Apple Watch simulators. We only need to run the
// build_swift module unit-tests once per OSX Platform test run, rather than
// once for each supported Apple device.

// REQUIRES: disabled-will-fixup-later

// UNSUPPORTED: OS=ios
// UNSUPPORTED: OS=tvos
// UNSUPPORTED: OS=watchos

// RUN: %{python} %utils/build_swift/run_tests.py
