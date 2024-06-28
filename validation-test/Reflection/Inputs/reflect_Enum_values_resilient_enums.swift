
public enum E1_resilient {
case a
case b
}

public enum E2_resilient {
case c(E1_resilient)
case d(E1_resilient)
}
