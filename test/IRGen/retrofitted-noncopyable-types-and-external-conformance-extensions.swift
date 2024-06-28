// RUN: %target-swift-frontend -emit-ir %s | %FileCheck --check-prefix=IR %s
// RUN: %target-run-simple-swift | %FileCheck --check-prefix=EXEC %s

// REQUIRES: executable_test

// The conformance descriptor for Optional: P ought to be encoded as if it
// were an unconditional conformance. We check this by checking that the
// global variable type is `%swift.protocol_conformance_descriptor`, indicating
// that there is no tail matter encoding conditional or inverted requirements.

// IR-LABEL: @"$sxSg4main1PABMc" ={{.*}} constant %swift.protocol_conformance_descriptor {

protocol P /*<implied> : Copyable*/ {
    func p()
}

extension Optional: P /*<implied> where T: Copyable */ {
    func p() {
        print("conforming optional \(self)")
    }
}

@inline(never)
func cast<T>(value: T) -> (any P)? {
    return value as? any P
}

func main() {
    // EXEC: going to test
    print("going to test")

    // EXEC-NEXT: conforming optional Optional("test")
    let x: String? = "test"
    if let p = cast(value: x) {
        p.p()
    } else {
        print("not a P")
    }

    // EXEC-NEXT: done testing
    print("done testing")
}
main()
