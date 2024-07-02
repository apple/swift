// RUN: %target-swift-frontend  -disable-availability-checking %s -emit-sil -o /dev/null -verify -strict-concurrency=complete

// REQUIRES: concurrency
// REQUIRES: asserts

// === OK cases

let _ = await withTaskGroup { group in
  group.addTask { }
  group.addTask { }
}
let _ = await withThrowingTaskGroup { group in
  group.addTask { }
  group.addTask { }
}

let _ = await withTaskGroup { group in
  group.addTask { 1 }
  group.addTask { 2 }
  return "result"
}

// === Cases where inference fails

let _: Int = await withTaskGroup { group in
  // expected-error@-1{{generic parameter 'ChildTaskResult' could not be inferred}}
  return 1
}
let _: Int = await withThrowingTaskGroup { group in
  // expected-error@-1{{generic parameter 'ChildTaskResult' could not be inferred}}
  return 1
}

// we infer the ChildTaskResult from a addTask, so if some other method
// precedes it, inference will fail.

let _: Int = await withTaskGroup { group in
  // expected-error@-1{{generic parameter 'ChildTaskResult' could not be inferred}}
  await group.next()
  return 1
}
let _: Int = await withThrowingTaskGroup { group in
  // expected-error@-1{{generic parameter 'ChildTaskResult' could not be inferred}}
  try await group.next()
  return 1
}

let _: Int = await withTaskGroup { group in
  // expected-error@-1{{generic parameter 'ChildTaskResult' could not be inferred}}
  await group.next()
  group.addTask { 1 }

  return 1
}
let _: Int = await withThrowingTaskGroup { group in
  // expected-error@-1{{generic parameter 'ChildTaskResult' could not be inferred}}
  try await group.next()
  group.addTask { 1 }

  return 1
}

let _ = await withTaskGroup { group in
  // expected-error@-1{{generic parameter 'ChildTaskResult' could not be inferred}}
}
let _ = await withThrowingTaskGroup { group in
  // expected-error@-1{{generic parameter 'ChildTaskResult' could not be inferred}}
}

// Notable exception, defer does not cause inference to break:

let _: Int = await withTaskGroup { group in
  defer { group.cancelAll() }
  group.addTask { 1 }

  return 1
}
let _: Int = await withThrowingTaskGroup { group in
  defer { group.cancelAll() }
  group.addTask { 1 }

  return 1
}

// Check conflicting types inside addTask {}

let _ = await withTaskGroup { group in
  group.addTask { 1 }
  group.addTask { "x" } // expected-error{{cannot convert value of type 'String' to closure result type 'Int'}}

  return 1
}
let _ = await withThrowingTaskGroup { group in
  group.addTask { 1 }
  group.addTask { "x" } // expected-error{{cannot convert value of type 'String' to closure result type 'Int'}}

  return 1
}

