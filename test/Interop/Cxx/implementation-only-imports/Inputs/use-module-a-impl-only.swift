@_implementationOnly import UserA

public func testUsesA() {
  let wrapper = MagicWrapperDerived()
  // This will force Swift to use __swift_interopStaticCast from CxxShim here.
  print(wrapper.baseMethod())
}
