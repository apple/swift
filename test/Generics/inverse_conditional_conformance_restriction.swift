// RUN: %target-typecheck-verify-swift \
// RUN:   -enable-experimental-feature NonescapableTypes \
// RUN:   -enable-experimental-feature SuppressedAssociatedTypes

protocol P {}
protocol Q {}
class DoggoClass {}

struct Blah<T: ~Copyable & ~Escapable>: ~Copyable, ~Escapable {}
extension Blah: Copyable where T: Copyable & Escapable {}
// expected-error@-1 {{conditional conformance to suppressible protocol 'Copyable' cannot depend on 'T: Escapable'}}

extension Blah: Escapable where T: Copyable, T: Escapable {}
// expected-error@-1 {{conditional conformance to suppressible protocol 'Escapable' cannot depend on 'T: Copyable'}}

struct Fixed<T: ~Copyable & ~Escapable>: ~Copyable, ~Escapable {}
extension Fixed: Copyable where T: Copyable {}
extension Fixed: Escapable where T: Escapable {}

struct TryConformance<Whatever: ~Copyable>: ~Copyable {}
extension TryConformance: Copyable
    where Whatever: P, Whatever: Q, Whatever: Sendable {}
// expected-error@-2 {{conditional conformance to suppressible protocol 'Copyable' cannot depend on 'Whatever: P'}}
// expected-error@-3 {{conditional conformance to suppressible protocol 'Copyable' cannot depend on 'Whatever: Q'}}
// expected-error@-4 {{conditional conformance to suppressible protocol 'Copyable' cannot depend on 'Whatever: Sendable'}}

struct TrySameType<Whatever: ~Copyable>: ~Copyable {}
extension TrySameType: Copyable
    where Whatever == Int {}
// expected-error@-2 {{conditional conformance to suppressible protocol 'Copyable' cannot depend on 'Whatever == Int'}}

struct TryClassAndLayoutConstraints<Whatever: ~Copyable, Heckin>: ~Copyable {}
extension TryClassAndLayoutConstraints: Copyable
    where Heckin: DoggoClass, Whatever: AnyObject {}
// expected-error@-2 {{conditional conformance to suppressible protocol 'Copyable' cannot depend on 'Whatever: AnyObject'}}
// expected-error@-3 {{conditional conformance to suppressible protocol 'Copyable' cannot depend on 'Heckin: DoggoClass'}}

protocol Queue: ~Copyable { associatedtype Job: ~Copyable }
struct Scheduler<Q: Queue>: ~Copyable {}
extension Scheduler: Copyable where Q.Job: Copyable {}
// expected-error@-1 {{conditional conformance to suppressible protocol 'Copyable' cannot depend on 'Q.Job: Copyable'}}
