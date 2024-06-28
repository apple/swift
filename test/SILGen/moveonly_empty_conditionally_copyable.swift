// RUN: %target-swift-frontend -emit-sil -verify -primary-file %s

struct G<T: ~Copyable>: ~Copyable { }

extension G: Copyable where T: Copyable {}
