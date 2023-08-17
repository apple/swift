import Foundation
import Observation

@available(SwiftStdlib 5.9, *)
@Observable final public class ObservableClass {
  public var state: State = .unused
}
