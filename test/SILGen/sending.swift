// RUN: %target-swift-emit-silgen -module-name sending -disable-availability-checking -enable-upcoming-feature RegionBasedIsolation -enable-experimental-feature SendingArgsAndResults -strict-concurrency=complete %s | %FileCheck %s

class NonSendable {}

// CHECK-LABEL: sil hidden [ossa] @$s7sending15returnsSendableSSyF : $@convention(thin) () -> @sil_sending @owned String {
func returnsSendable() -> sending String { fatalError() }

// CHECK-LABEL: sil hidden [ossa] @$s7sending18returnsNonSendableAA0cD0CyF : $@convention(thin) () -> @sil_sending @owned NonSendable {
func returnsNonSendable() -> sending NonSendable { fatalError() }

// CHECK-LABEL: sil hidden [ossa] @$s7sending25genericReturnsNonSendablexylF : $@convention(thin) <T> () -> @sil_sending @out T {
func genericReturnsNonSendable<T>() -> sending T { fatalError() }

actor MyActor {
  // CHECK-LABEL: sil hidden [ossa] @$s7sending7MyActorC15returnsSendableSSyF : $@convention(method) (@sil_isolated @guaranteed MyActor) -> @sil_sending @owned String {
  func returnsSendable() -> sending String { fatalError() }

  // CHECK-LABEL: sil hidden [ossa] @$s7sending7MyActorC18returnsNonSendableAA0eF0CyF : $@convention(method) (@sil_isolated @guaranteed MyActor) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> sending NonSendable { fatalError() }

  // CHECK-LABEL: sil hidden [ossa] @$s7sending7MyActorC25genericReturnsNonSendablexylF : $@convention(method) <T> (@sil_isolated @guaranteed MyActor) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> sending T { fatalError() }
}

struct Struct {
  // CHECK-LABEL: sil hidden [ossa] @$s7sending6StructV15returnsSendableSSyF : $@convention(method) (Struct) -> @sil_sending @owned String {
  func returnsSendable() -> sending String { fatalError() }

  // CHECK-LABEL: sil hidden [ossa] @$s7sending6StructV18returnsNonSendableAA0dE0CyF : $@convention(method) (Struct) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> sending NonSendable { fatalError() }

  // CHECK-LABEL: sil hidden [ossa] @$s7sending6StructV25genericReturnsNonSendablexylF : $@convention(method) <T> (Struct) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> sending T { fatalError() }
}

@MainActor
struct MainActorStruct {
  // CHECK-LABEL: sil hidden [ossa] @$s7sending15MainActorStructV15returnsSendableSSyF : $@convention(method) (MainActorStruct) -> @sil_sending @owned String {
  func returnsSendable() -> sending String { fatalError() }

  // CHECK-LABEL: sil hidden [ossa] @$s7sending15MainActorStructV18returnsNonSendableAA0fG0CyF : $@convention(method) (MainActorStruct) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> sending NonSendable { fatalError() }

  // CHECK-LABEL: sil hidden [ossa] @$s7sending15MainActorStructV25genericReturnsNonSendablexylF : $@convention(method) <T> (MainActorStruct) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> sending T { fatalError() }
}

class Class {
  // CHECK-LABEL: sil hidden [ossa] @$s7sending5ClassC15returnsSendableSSyF : $@convention(method) (@guaranteed Class) -> @sil_sending @owned String {
  func returnsSendable() -> sending String { fatalError() }

  // CHECK-LABEL: sil hidden [ossa] @$s7sending5ClassC18returnsNonSendableAA0dE0CyF : $@convention(method) (@guaranteed Class) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> sending NonSendable { fatalError() }

  // CHECK-LABEL: sil hidden [ossa] @$s7sending5ClassC25genericReturnsNonSendablexylF : $@convention(method) <T> (@guaranteed Class) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> sending T { fatalError() }
}

final class FinalClass {
  // CHECK-LABEL: sil hidden [ossa] @$s7sending10FinalClassC15returnsSendableSSyF : $@convention(method) (@guaranteed FinalClass) -> @sil_sending @owned String {
  func returnsSendable() -> sending String { fatalError() }

  // CHECK-LABEL: sil hidden [ossa] @$s7sending10FinalClassC18returnsNonSendableAA0eF0CyF : $@convention(method) (@guaranteed FinalClass) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> sending NonSendable { fatalError() }

  // CHECK-LABEL: sil hidden [ossa] @$s7sending10FinalClassC25genericReturnsNonSendablexylF : $@convention(method) <T> (@guaranteed FinalClass) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> sending T { fatalError() }
}

@MainActor
class MainActorClass {
  // CHECK-LABEL: sil hidden [ossa] @$s7sending14MainActorClassC15returnsSendableSSyF : $@convention(method) (@guaranteed MainActorClass) -> @sil_sending @owned String {
  func returnsSendable() -> sending String { fatalError() }

  // CHECK-LABEL: sil hidden [ossa] @$s7sending14MainActorClassC18returnsNonSendableAA0fG0CyF : $@convention(method) (@guaranteed MainActorClass) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> sending NonSendable { fatalError() }

  // CHECK-LABEL: sil hidden [ossa] @$s7sending14MainActorClassC25genericReturnsNonSendablexylF : $@convention(method) <T> (@guaranteed MainActorClass) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> sending T { fatalError() }
}

@MainActor
final class FinalMainActorClass {
  // CHECK-LABEL: sil hidden [ossa] @$s7sending19FinalMainActorClassC15returnsSendableSSyF : $@convention(method) (@guaranteed FinalMainActorClass) -> @sil_sending @owned String {
  func returnsSendable() -> sending String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s7sending19FinalMainActorClassC18returnsNonSendableAA0gH0CyF : $@convention(method) (@guaranteed FinalMainActorClass) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> sending NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s7sending19FinalMainActorClassC25genericReturnsNonSendablexylF : $@convention(method) <T> (@guaranteed FinalMainActorClass) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> sending T { fatalError() }
}

enum Enum {
  case myCase
  // CHECK-LABEL: sil hidden [ossa] @$s7sending4EnumO15returnsSendableSSyF : $@convention(method) (Enum) -> @sil_sending @owned String {
  func returnsSendable() -> sending String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s7sending4EnumO18returnsNonSendableAA0dE0CyF : $@convention(method) (Enum) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> sending NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s7sending4EnumO25genericReturnsNonSendablexylF : $@convention(method) <T> (Enum) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> sending T { fatalError() }
}

@MainActor
enum MainActorEnum {
  case myCase
  // CHECK-LABEL: sil hidden [ossa] @$s7sending13MainActorEnumO15returnsSendableSSyF : $@convention(method) (MainActorEnum) -> @sil_sending @owned String {
  func returnsSendable() -> sending String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s7sending13MainActorEnumO18returnsNonSendableAA0fG0CyF : $@convention(method) (MainActorEnum) -> @sil_sending @owned NonSendable {
  func returnsNonSendable() -> sending NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s7sending13MainActorEnumO25genericReturnsNonSendablexylF : $@convention(method) <T> (MainActorEnum) -> @sil_sending @out T {
  func genericReturnsNonSendable<T>() -> sending T { fatalError() }
}

protocol P {
  func protReturnsSendable() -> sending String
  func protReturnsNonSendable() -> sending NonSendable
  func protGenericReturnsNonSendable<T>() -> sending T
}

extension P {
  // CHECK-LABEL: sil hidden [ossa] @$s7sending1PPAAE19protReturnsSendableSSyF : $@convention(method) <Self where Self : P> (@in_guaranteed Self) -> @sil_sending @owned String {
  func protReturnsSendable() -> sending String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s7sending1PPAAE22protReturnsNonSendableAA0dE0CyF : $@convention(method) <Self where Self : P> (@in_guaranteed Self) -> @sil_sending @owned NonSendable {
  func protReturnsNonSendable() -> sending NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s7sending1PPAAE29protGenericReturnsNonSendableqd__ylF : $@convention(method) <Self where Self : P><T> (@in_guaranteed Self) -> @sil_sending @out T {
  func protGenericReturnsNonSendable<T>() -> sending T { fatalError() }
}

extension Struct : P {
  // CHECK-LABEL: sil hidden [ossa] @$s7sending6StructV19protReturnsSendableSSyF : $@convention(method) (Struct) -> @sil_sending @owned String {
  func protReturnsSendable() -> sending String { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s7sending6StructV22protReturnsNonSendableAA0eF0CyF : $@convention(method) (Struct) -> @sil_sending @owned NonSendable {
  func protReturnsNonSendable() -> sending NonSendable { fatalError() }
  // CHECK-LABEL: sil hidden [ossa] @$s7sending6StructV29protGenericReturnsNonSendablexylF : $@convention(method) <T> (Struct) -> @sil_sending @out T {
  func protGenericReturnsNonSendable<T>() -> sending T { fatalError() }
}
