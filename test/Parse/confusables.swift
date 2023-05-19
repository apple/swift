// RUN: %target-typecheck-verify-swift

// expected-error @+4 {{type annotation missing in pattern}}
// expected-error @+3 {{cannot find operator '⁚' in scope}}
// expected-error @+2 {{operator with postfix spacing cannot start a subexpression}}
// expected-error @+1 {{consecutive statements on a line must be separated by ';'}}
let number⁚ Int // expected-note {{operator '⁚' (Two Dot Punctuation) looks similar to ':' (Colon); did you mean ':' (Colon)?}} {{11-14=:}}

// expected-warning @+3 2 {{integer literal is unused}}
// expected-error @+2 {{invalid character in source file}}
// expected-error @+1 {{consecutive statements on a line must be separated by ';'}}
5 ‒ 5 // expected-note {{unicode character '‒' (Figure Dash) looks similar to '-' (Hyphen Minus); did you mean to use '-' (Hyphen Minus)?}} {{3-6=-}}

// expected-error @+3 {{cannot convert value of type '(Bool, _)' to expected condition type 'Bool'}}
// expected-error @+2 {{cannot find 'ꝸꝸꝸ' in scope}}
// expected-error @+1 {{expected ',' separator}}
if (true ꝸꝸꝸ false) {} // expected-note {{identifier 'ꝸꝸꝸ' contains possibly confused characters; did you mean to use '&&&'?}} {{10-19=&&&}}

// expected-error @+3 {{invalid character in source file}}
// expected-error @+2 {{expected ',' separator}}
// expected-error @+1 {{binary operator '==' cannot be applied to operands of type '(Int, Int)' and 'Int'}}
if (5 ‒ 5) == 0 {} // expected-note {{unicode character '‒' (Figure Dash) looks similar to '-' (Hyphen Minus); did you mean to use '-' (Hyphen Minus)?}} {{7-10=-}}

// GREEK QUESTION MARK (which looks like a semicolon) 
print("A"); print("B")
// expected-error@-1 2{{consecutive statements on a line must be separated by ';'}}
// expected-error@-2 {{cannot find ';' in scope}}
// expected-note@-3 {{identifier ';' (Greek Question Mark) looks similar to ';' (Semicolon); did you mean ';' (Semicolon)?}} {{11-13=;}}
