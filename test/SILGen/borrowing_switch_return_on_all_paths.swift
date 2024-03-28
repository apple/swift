// RUN: %target-swift-emit-silgen -enable-experimental-feature BorrowingSwitch -enable-experimental-feature NoncopyableGenerics -verify %s

struct Box<Wrapped: ~Copyable>: ~Copyable {
    var wrapped: Wrapped {
        _read { fatalError() }
        _modify { fatalError() }
    }
}

struct NoncopyableList<Element>: ~Copyable {
    struct Node: ~Copyable {
        var element: Element
        var next: Link
    }

    enum Link: ~Copyable {
        case empty
        case more(Box<Node>)
    }

    var head: Link = .empty
}

extension NoncopyableList.Link {
    func find(where predicate: (Element)->Bool) -> Maybe<Element> {
        switch self {
        case .empty: return .none
        case .more(let box):
            if predicate(box.wrapped.element) { return .some(box.wrapped.element) }
            return box.wrapped.next.find(where: predicate)
        }
    }
}


enum Maybe<Element: ~Copyable>: ~Copyable {
    case none
    case some(Element)
}
