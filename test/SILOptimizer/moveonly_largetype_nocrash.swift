// RUN: %target-swift-emit-sil -sil-verify-all -verify %s

// Previously there was a latent albeit harmless bug in
// FieldSensitivePrunedLiveness around an array inside of it re-allocating. This
// test exercises that behavior by creating a type that is so large that it
// cannot be stored within anything but a very large small vector (> 128
// elements).

public struct Large : ~Copyable {
    var a0 = 0
    var a1 = 0
    var a2 = 0
    var a3 = 0
    var a4 = 0
    var a5 = 0
    var a6 = 0
    var a7 = 0
    var a8 = 0
    var a9 = 0
    var a10 = 0
    var a11 = 0
    var a12 = 0
    var a13 = 0
    var a14 = 0
    var a15 = 0
    var a16 = 0
    var a17 = 0
    var a18 = 0
    var a19 = 0
    var a20 = 0
    var a21 = 0
    var a22 = 0
    var a23 = 0
    var a24 = 0
    var a25 = 0
    var a26 = 0
    var a27 = 0
    var a28 = 0
    var a29 = 0
    var a30 = 0
    var a31 = 0
    var a32 = 0
    var a33 = 0
    var a34 = 0
    var a35 = 0
    var a36 = 0
    var a37 = 0
    var a38 = 0
    var a39 = 0
    var a40 = 0
    var a41 = 0
    var a42 = 0
    var a43 = 0
    var a44 = 0
    var a45 = 0
    var a46 = 0
    var a47 = 0
    var a48 = 0
    var a49 = 0
    var a50 = 0
    var a51 = 0
    var a52 = 0
    var a53 = 0
    var a54 = 0
    var a55 = 0
    var a56 = 0
    var a57 = 0
    var a58 = 0
    var a59 = 0
    var a60 = 0
    var a61 = 0
    var a62 = 0
    var a63 = 0
    var a64 = 0
    var a65 = 0
    var a66 = 0
    var a67 = 0
    var a68 = 0
    var a69 = 0
    var a70 = 0
    var a71 = 0
    var a72 = 0
    var a73 = 0
    var a74 = 0
    var a75 = 0
    var a76 = 0
    var a77 = 0
    var a78 = 0
    var a79 = 0
    var a80 = 0
    var a81 = 0
    var a82 = 0
    var a83 = 0
    var a84 = 0
    var a85 = 0
    var a86 = 0
    var a87 = 0
    var a88 = 0
    var a89 = 0
    var a90 = 0
    var a91 = 0
    var a92 = 0
    var a93 = 0
    var a94 = 0
    var a95 = 0
    var a96 = 0
    var a97 = 0
    var a98 = 0
    var a99 = 0
    var a100 = 0
    var a101 = 0
    var a102 = 0
    var a103 = 0
    var a104 = 0
    var a105 = 0
    var a106 = 0
    var a107 = 0
    var a108 = 0
    var a109 = 0
    var a110 = 0
    var a111 = 0
    var a112 = 0
    var a113 = 0
    var a114 = 0
    var a115 = 0
    var a116 = 0
    var a117 = 0
    var a118 = 0
    var a119 = 0
    var a120 = 0
    var a121 = 0
    var a122 = 0
    var a123 = 0
    var a124 = 0
    var a125 = 0
    var a126 = 0
    var a127 = 0
    var a128 = 0
    var a129 = 0
    var a130 = 0
}

func borrowVal(_ x: borrowing Large) {}

var bool: Bool { false }

func test() {
    let a = Large()

    if bool {

    } else {

    }

    if bool {

    } else {

    }

    borrowVal(a)
}
