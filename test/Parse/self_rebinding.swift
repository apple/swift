// RUN: %target-typecheck-verify-swift

class Writer {
    private var articleWritten = 47

    func stop() {
        let rest: () -> Void = { [weak self] in
            let articleWritten = self?.articleWritten ?? 0
            guard let `self` = self else {
                return
            }

            self.articleWritten = articleWritten
        }

        fatalError("I'm running out of time")
        rest()
    }

    func nonStop() {
        let write: () -> Void = { [weak self] in
            self?.articleWritten += 1

            if let self = self {
                self.articleWritten += 1
            }

            if let `self` = self {
                self.articleWritten += 1
            }

            guard let self = self else {
                return
            }

            self.articleWritten += 1
        }

        write()
    }
}

struct T {
    var mutable: Int = 0
    func f() {
        // expected-error @+2 {{keyword 'self' cannot be used as an identifier here}}
        // expected-note @+1 {{if this name is unavoidable, use backticks to escape it}}
        let self = self
    }
}
