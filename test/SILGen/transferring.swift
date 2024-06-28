// RUN: %target-swift-emit-silgen -disable-availability-checking -strict-concurrency=complete %s | %FileCheck %s

class NonSendable {}

// CHECK-LABEL: sil hidden [ossa] @$s12transferring15returnsSendableSSyF : $@convention(thin) () -> @sil_sending @owned String {
func returnsSendable() -> transferring String { fatalError() }
// CHECK-LABEL: sil hidden [ossa] @$s12transferring18returnsNonSendableAA0cD0CyF : $@convention(thin) () -> @sil_sending @owned NonSendable {
func returnsNonSendable() -> transferring NonSendable { fatalError() }
// CHECK-LABEL: sil hidden [ossa] @$s12transferring25genericReturnsNonSendablexylF : $@convention(thin) <T> () -> @sil_sending @out T {
func genericReturnsNonSendable<T>() -> transferring T { fatalError() }

actor MyActor {
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring7MyActorC15returnsSendableSSyF : $@convention(method) (@sil_isolated @guaranteed MyActor) -> @sil_sending @owned String {
  func returnsSendable() -> transferring String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring7MyActorC18returnsNonSendableAA0eF0CyF : $@convention(method) (@sil_isolated @guaranteed MyActor) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> transferring NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring7MyActorC25genericReturnsNonSendablexylF : $@convention(method) <T> (@sil_isolated @guaranteed MyActor) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> transferring T { fatalError() }
}

struct Struct {
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring6StructV15returnsSendableSSyF : $@convention(method) (Struct) -> @sil_sending @owned String {
  func returnsSendable() -> transferring String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring6StructV18returnsNonSendableAA0dE0CyF : $@convention(method) (Struct) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> transferring NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring6StructV25genericReturnsNonSendablexylF : $@convention(method) <T> (Struct) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> transferring T { fatalError() }
}

@MainActor
struct MainActorStruct {
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring15MainActorStructV15returnsSendableSSyF : $@convention(method) (MainActorStruct) -> @sil_sending @owned String {
  func returnsSendable() -> transferring String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring15MainActorStructV18returnsNonSendableAA0fG0CyF : $@convention(method) (MainActorStruct) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> transferring NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring15MainActorStructV25genericReturnsNonSendablexylF : $@convention(method) <T> (MainActorStruct) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> transferring T { fatalError() }
}

class Class {
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring5ClassC15returnsSendableSSyF : $@convention(method) (@guaranteed Class) -> @sil_sending @owned String {
  func returnsSendable() -> transferring String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring5ClassC18returnsNonSendableAA0dE0CyF : $@convention(method) (@guaranteed Class) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> transferring NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring5ClassC25genericReturnsNonSendablexylF : $@convention(method) <T> (@guaranteed Class) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> transferring T { fatalError() }
}

final class FinalClass {
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring10FinalClassC15returnsSendableSSyF : $@convention(method) (@guaranteed FinalClass) -> @sil_sending @owned String {
  func returnsSendable() -> transferring String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring10FinalClassC18returnsNonSendableAA0eF0CyF : $@convention(method) (@guaranteed FinalClass) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> transferring NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring10FinalClassC25genericReturnsNonSendablexylF : $@convention(method) <T> (@guaranteed FinalClass) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> transferring T { fatalError() }
}

@MainActor
class MainActorClass {
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring14MainActorClassC15returnsSendableSSyF : $@convention(method) (@guaranteed MainActorClass) -> @sil_sending @owned String {
  func returnsSendable() -> transferring String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring14MainActorClassC18returnsNonSendableAA0fG0CyF : $@convention(method) (@guaranteed MainActorClass) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> transferring NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring14MainActorClassC25genericReturnsNonSendablexylF : $@convention(method) <T> (@guaranteed MainActorClass) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> transferring T { fatalError() }
}

@MainActor
final class FinalMainActorClass {
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring19FinalMainActorClassC15returnsSendableSSyF : $@convention(method) (@guaranteed FinalMainActorClass) -> @sil_sending @owned String {
  func returnsSendable() -> transferring String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring19FinalMainActorClassC18returnsNonSendableAA0gH0CyF : $@convention(method) (@guaranteed FinalMainActorClass) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> transferring NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring19FinalMainActorClassC25genericReturnsNonSendablexylF : $@convention(method) <T> (@guaranteed FinalMainActorClass) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> transferring T { fatalError() }
}

enum Enum {
  case myCase
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring4EnumO15returnsSendableSSyF : $@convention(method) (Enum) -> @sil_sending @owned String {
  func returnsSendable() -> transferring String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring4EnumO18returnsNonSendableAA0dE0CyF : $@convention(method) (Enum) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> transferring NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring4EnumO25genericReturnsNonSendablexylF : $@convention(method) <T> (Enum) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> transferring T { fatalError() }
}

@MainActor
enum MainActorEnum {
  case myCase
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring13MainActorEnumO15returnsSendableSSyF : $@convention(method) (MainActorEnum) -> @sil_sending @owned String {
  func returnsSendable() -> transferring String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring13MainActorEnumO18returnsNonSendableAA0fG0CyF : $@convention(method) (MainActorEnum) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> transferring NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring13MainActorEnumO25genericReturnsNonSendablexylF : $@convention(method) <T> (MainActorEnum) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> transferring T { fatalError() }
}

protocol P {
  func protReturnsSendable() -> transferring String
  func protReturnsNonSendable() -> transferring NonSendable
  func protGenericReturnsNonSendable<T>() -> transferring T
}

extension P {
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring1PPAAE19protReturnsSendableSSyF : $@convention(method) <Self where Self : P> (@in_guaranteed Self) -> @sil_sending @owned String {
  func protReturnsSendable() -> transferring String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring1PPAAE22protReturnsNonSendableAA0dE0CyF : $@convention(method) <Self where Self : P> (@in_guaranteed Self) -> @sil_sending @owned NonSendable {
  func protReturnsNonSendable() -> transferring NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring1PPAAE29protGenericReturnsNonSendableqd__ylF : $@convention(method) <Self where Self : P><T> (@in_guaranteed Self) -> @sil_sending @out T {
  func protGenericReturnsNonSendable<T>() -> transferring T { fatalError() }
}

extension Struct : P {
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring6StructV19protReturnsSendableSSyF : $@convention(method) (Struct) -> @sil_sending @owned String {
  func protReturnsSendable() -> transferring String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring6StructV22protReturnsNonSendableAA0eF0CyF : $@convention(method) (Struct) -> @sil_sending @owned NonSendable {
  func protReturnsNonSendable() -> transferring NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s12transferring6StructV29protGenericReturnsNonSendablexylF : $@convention(method) <T> (Struct) -> @sil_sending @out T {
  func protGenericReturnsNonSendable<T>() -> transferring T { fatalError() }
}
