struct STest {
  var x = requireEquatable(S())
}

@ConformanceViaExtension
class Child: Parent {}
