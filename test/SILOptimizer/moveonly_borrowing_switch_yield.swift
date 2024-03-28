// RUN: %target-swift-frontend -enable-experimental-feature NoncopyableGenerics -enable-experimental-feature BorrowingSwitch -enable-experimental-feature MoveOnlyPartialConsumption -parse-as-library -O -emit-sil -verify %s

extension List {
    var peek: Element {
        _read {
            switch self.head {
            case .empty: fatalError()
            case .more(let box):
                yield box.wrapped.element
            }
            //yield head.peek
        }
    }
}

struct MyPointer<Wrapped: ~Copyable>: Copyable {
    var v: UnsafeMutablePointer<Int>

    static func allocate(capacity: Int) -> Self {
        fatalError()
    }

    func initialize(to: consuming Wrapped) {
    }
    func deinitialize(count: Int) {
    }
    func deallocate() {
    }
    func move() -> Wrapped {
        fatalError()
    }

    var pointee: Wrapped {
        _read { fatalError() }
    }
}

struct Box<Wrapped: ~Copyable>: ~Copyable {
    private let _pointer: MyPointer<Wrapped>
    
    init(_ element: consuming Wrapped) {
        _pointer = .allocate(capacity: 1)
        print("allocatin", _pointer)
        _pointer.initialize(to: element)
    }
        
    deinit {
        print("not deallocatin", _pointer)
        _pointer.deinitialize(count: 1)
        _pointer.deallocate()
    }
    
    consuming func move() -> Wrapped {
        let wrapped = _pointer.move()
        print("deallocatin", _pointer)
        _pointer.deallocate()
        discard self
        return wrapped
    }
    
    var wrapped: Wrapped {
        _read { yield _pointer.pointee }
    }
}

struct List<Element>: ~Copyable {
    var head: Link<Element> = .empty
    var bool = false
}

enum Link<Element>: ~Copyable {
    case empty
    case more(Box<Node<Element>>)

    var peek: Element {
        _read {
            switch self {
            case .empty: fatalError()
            case .more(let box):
                yield box.wrapped.element
            }
        }
    }
}

struct Node<Element>: ~Copyable {
    let element: Element
    let next: Link<Element>
    var bool = true
}

extension List {
    mutating func append(_ element: consuming Element) {
        self = List(head: .more(Box(Node(element: element, next: self.head))))
    }

    var isEmpty: Bool {
        switch self.head {
        case .empty: true
        default: false
        }
    }
    
    mutating func pop() -> Element {
        let h = self.head
        switch consume h {
        case .empty: fatalError()
        case .more(let box):
            let node = box.move()
            self = .init(head: node.next)
            return node.element
        }
    }

}

@main
struct Main {
    static func main() {
        var l: List<Int> = .init()
        l.append(1)
        l.append(2)
        l.append(3)
        print(l.pop())
        print(l.pop())
        print(l.pop())
        print(l.isEmpty)
    }
}
