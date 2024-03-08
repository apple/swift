// REQUIRES: OS=macosx

// RUN: %empty-directory(%t)
// RUN: mv "$(dirname %utils)/docs" "$(dirname %utils)/docs.docc"
// RUN: plutil -create xml1 "$(dirname %utils)/docs.docc/Info.plist"
// RUN: plutil -insert CFBundleDisplayName -string "Swift Compiler Documentation" "$(dirname %utils)/docs.docc/Info.plist"
// RUN: plutil -insert CFBundleIdentifier -string "org.swift.compiler-documentation" "$(dirname %utils)/docs.docc/Info.plist"
// RUN: xcrun docc convert --output-path %t $(dirname %utils)/docs.docc
// RUN: mv $(dirname %utils)/docs.docc $(dirname %utils)/docs

// the generated bundle should be at least as big as all of the markdown files in the source directory
// RUN: test $(du -c %t | tail -1 | awk '{ print $1 }') -gt $(du -c $(dirname %utils)/docs/*.md | tail -1 | awk '{ print $1 }') && echo success
// CHECK: success
