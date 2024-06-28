struct S {
  var _x: Int
  public var xImmutablePublic: Int {
    @storageRestrictions(initializes: _x)
    init {
      _x = initialValue
    }
    get {
      return _x
    }
  }
  
  public var xMutablePublic: Int {
    @storageRestrictions(initializes: _x)
    init {
      _x = initialValue
    }
    get {
      return _x
    }
    set {
      _x = newValue
    }
  }
  
  internal var xImmutableInternal: Int {
    @storageRestrictions(initializes: _x)
    init {
      _x = initialValue
    }
    get {
      return _x
    }
  }
  
  internal var xMutableInternal: Int {
    @storageRestrictions(initializes: _x)
    init {
      _x = initialValue
    }
    get {
      return _x
    }
    set {
      _x = newValue
    }
  }
}