// Continuous integration for the OS X Platform also runs the tests in the
// iPhone, Apple TV and Apple Watch simulators. We only need to run the
// Python lint once per OSX Platform test run, rather than once for each
// supported Apple device.

// REQUIRES: testing-will-fixup-later

// REQUIRES: OS=macosx

// RUN: %{python} %utils/python_lint.py
