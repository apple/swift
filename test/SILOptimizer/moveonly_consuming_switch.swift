// RUN: %target-swift-frontend -emit-sil -enable-experimental-feature NoncopyableGenerics -enable-experimental-feature BorrowingSwitch -enable-experimental-feature MoveOnlyPartialConsumption -verify %s

// TODO: Remove this and just use the real `UnsafeMutablePointer` when
// noncopyable type support has been upstreamed.
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
}

struct Box<Wrapped: ~Copyable>: ~Copyable {
    private let _pointer: MyPointer<Wrapped>
    
    init(_ element: consuming Wrapped) {
        _pointer = .allocate(capacity: 1)
        print("allocating",_pointer)
        _pointer.initialize(to: element)
    }
        
    deinit {
        print("deallocating",_pointer)
        _pointer.deinitialize(count: 1)
        _pointer.deallocate()
    }
    
    consuming func move() -> Wrapped {
        let wrapped = _pointer.move()
        print("deallocating", _pointer)
        _pointer.deallocate()
        discard self
        return wrapped
    }
}

enum List<Element: ~Copyable>: ~Copyable {
    case empty
    case node(Element, Box<List>)
}

extension List {
    init() { self = .empty }
    
    mutating func push(_ element: consuming Element) {
        self = .node(element, Box(self))
    }
        
    mutating func pop() -> Element {
        switch consume self {
        case .node(let element, let box):
            self = box.move()
            return element
        case .empty:
            fatalError()
        }
    }
    
    var isEmpty: Bool {
        switch self {
        case .empty: true
        case .node: false
        }
    }
}
