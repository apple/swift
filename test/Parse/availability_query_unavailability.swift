// RUN: %target-typecheck-verify-swift
// REQUIRES: OS=macosx
// This file is mostly an inverted version of availability_query.swift
if #unavailable(OSX 10.51) {
}

// Disallow explicit wildcards.
if #unavailable(OSX 10.51, *) {} // expected-error {{platform wildcard '*' is always implicit in #unavailable}} {{28-29=}}
// Disallow use as an expression.
if (#unavailable(OSX 10.51)) {}  // expected-error {{#unavailable may only be used as condition of an 'if', 'guard'}}
let x = #unavailable(OSX 10.51)  // expected-error {{#unavailable may only be used as condition of}}
(#unavailable(OSX 10.51) ? 1 : 0) // expected-error {{#unavailable may only be used as condition of an}}
if !#unavailable(OSX 10.52) { // expected-error {{#unavailable may only be used as condition of an}}
}
if let _ = Optional(5), !#unavailable(OSX 10.52) { // expected-error {{#unavailable may only be used as condition}}
}

if #unavailable(OSX 10.51) && #unavailable(OSX 10.52) { // expected-error {{expected ',' joining parts of a multi-clause condition}} {{27-30=,}}
}


if #unavailable { // expected-error {{expected availability condition}} expected-error {{closure expression is unused}} expected-error {{top-level statement cannot begin with a closure expression}} expected-note {{did you mean to use a 'do' statement?}} {{17-17=do }}
}

if #unavailable( { // expected-error {{expected platform name}} expected-error {{expected ')'}} expected-note {{to match this opening '('}}
}

if #unavailable() { // expected-error {{expected platform name}}
}

if #unavailable(OSX { // expected-error {{expected version number}} expected-error {{expected ')'}} expected-note {{to match this opening '('}}
}

if #unavailable(OSX) { // expected-error {{expected version number}}
}

if #unavailable(OSX 10.51 { // expected-error {{expected ')'}} expected-note {{to match this opening '('}}
}

if #unavailable(iDishwasherOS 10.51) { // expected-warning {{unrecognized platform name 'iDishwasherOS'}}
}

if #unavailable(iDishwasherOS 10.51) { // expected-warning {{unrecognized platform name 'iDishwasherOS'}}
}

if #unavailable(OSX 10.51, OSX 10.52) {  // expected-error {{version for 'macOS' already specified}}
}

if #unavailable(OSX 10.51, iOS 8.0, *) { }  // expected-error {{platform wildcard '*' is always implicit in #unavailable}} {{37-38=}}
if #unavailable(iOS 8.0) {
}

if #unavailable(iOSApplicationExtension, unavailable) { // expected-error 2{{expected version number}}
}

// Should this be a valid spelling since `#unvailable(*)` cannot be written?
if #unavailable() { // expected-error {{expected platform name}}
}

if #unavailable(OSX 10 { // expected-error {{expected ')' in availability query}} expected-note {{to match this opening '('}}
}

// Multiple platforms
if #unavailable(OSX 10.51, iOS 8.0) {
}


if #unavailable(OSX 10.51, { // expected-error {{expected platform name}} // expected-error {{expected ')'}} expected-note {{to match this opening '('}}
}

if #unavailable(OSX 10.51,) { // expected-error {{expected platform name}}
}

if #unavailable(OSX 10.51, iOS { // expected-error {{expected version number}} // expected-error {{expected ')'}} expected-note {{to match this opening '('}}
}

if #unavailable(OSX 10.51, iOS 8.0, iDishwasherOS 10.51) { // expected-warning {{unrecognized platform name 'iDishwasherOS'}}
}

if #unavailable(iDishwasherOS 10.51, OSX 10.51) { // expected-warning {{unrecognized platform name 'iDishwasherOS'}}
}

if #unavailable(OSX 10.51 || iOS 8.0) {// expected-error {{'||' cannot be used in an availability condition}}
}

// Emit Fix-It removing un-needed >=, for the moment.
if #unavailable(OSX >= 10.51) { // expected-error {{version comparison not needed}} {{21-24=}}
}

// Bool then #unavailable.
if 1 != 2, #unavailable(iOS 8.0) {}

// Pattern then #unavailable(iOS 8.0) {
if case 42 = 42, #unavailable(iOS 8.0) {}
if let _ = Optional(42), #unavailable(iOS 8.0) {}

// Allow "macOS" as well.
if #unavailable(macOS 10.51) {
}

// Prevent availability and unavailability being present in the same statement.
if #unavailable(macOS 10.51), #available(macOS 10.52, *) { // expected-error {{#available and #unavailable cannot be in the same statement}}
}
if #available(macOS 10.51, *), #unavailable(macOS 10.52) { // expected-error {{#available and #unavailable cannot be in the same statement}}
}
if #available(macOS 10.51, *), #available(macOS 10.55, *), #unavailable(macOS 10.53) { // expected-error {{#available and #unavailable cannot be in the same statement}}
}
if #unavailable(macOS 10.51), #unavailable(macOS 10.55), #available(macOS 10.53, *) { // expected-error {{#available and #unavailable cannot be in the same statement}}
}
if case 42 = 42, #available(macOS 10.51, *), #unavailable(macOS 10.52) { // expected-error {{#available and #unavailable cannot be in the same statement}}
}
if #available(macOS 10.51, *), case 42 = 42, #unavailable(macOS 10.52) { // expected-error {{#available and #unavailable cannot be in the same statement}}
}

// Allow availabiility and unavailability to mix if they are not in the same statement.
if #unavailable(macOS 11) {
  if #available(macOS 10, *) { }
}
if #available(macOS 10, *) {
  if #unavailable(macOS 11) { }
}

// Diagnose wrong spellings of unavailability
if #available(*) == false { // expected-error {{#available cannot be used as an expression, did you mean to use '#unavailable'?}} {{4-14=#unavailable}} {{18-27=}}
}
if !#available(*) { // expected-error {{#available cannot be used as an expression, did you mean to use '#unavailable'?}} {{4-15=#unavailable}}
} 