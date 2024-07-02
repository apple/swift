// REQUIRES: OS=macosx
// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend %t/test.swift -enable-testing -disable-availability-checking -emit-ir -o %t/test.ll -plugin-path %swift-plugin-dir -emit-tbd -emit-tbd-path %t/test.tbd -I %t -tbd-install_name distributed
// X: %swift -target arm64-apple-macos13.0 -target-variant arm64-apple-ios16.0-macabi -plugin-path %swift-plugin-dir -typecheck -parse-as-library %t/test.swift -emit-tbd -emit-tbd-path %t/distributed.tbd -tbd-install_name Distributed
// RUN: %llvm-readtapi --compare %t/test.tbd %t/expected.tbd


//--- test.swift
import Distributed

@Resolvable
public protocol AchievementServiceProtocol: DistributedActor where ActorSystem == LocalTestingDistributedActorSystem {
  distributed func clearCache() async throws
}

distributed actor SomeDistributedActor: AchievementServiceProtocol {
  distributed func clearCache() async throws {}
}

distributed actor AnotherDistributedActor: AchievementServiceProtocol {
  distributed func clearCache() async throws {}
}

//--- expected.tbd
{
  "main_library": {
  "compatibility_versions": [
    {
      "version": "0"
    }
  ],
  "current_versions": [
    {
      "version": "0"
    }
  ],
  "exported_symbols": [
    {
      "data": {
      "global": [
        "_$s4test20SomeDistributedActorC10clearCacheyyYaKFTu",
        "_$s4test27$AchievementServiceProtocolCN",
        "_$s4test20SomeDistributedActorCMm",
        "_$s4test20SomeDistributedActorCAA26AchievementServiceProtocolAAWP",
        "_$s4test26AchievementServiceProtocolPAA11Distributed01_E9ActorStubRzrlE10clearCacheyyYaKFTETu",
        "_$s4test23AnotherDistributedActorCAA26AchievementServiceProtocolAAWP",
        "_$s4test23AnotherDistributedActorCMm",
        "_$s4test27$AchievementServiceProtocolCAA0bcD0AAWP",
        "_$s4test20SomeDistributedActorCN",
        "_$s4test27$AchievementServiceProtocolCMm",
        "_$s4test23AnotherDistributedActorC10clearCacheyyYaKFTu",
        "_$s4test23AnotherDistributedActorC10clearCacheyyYaKFTETu",
        "_$s4test26AchievementServiceProtocolPAA11Distributed01_E9ActorStubRzrlE10clearCacheyyYaKFTu",
        "_$s4test20SomeDistributedActorC10clearCacheyyYaKFTETu",
        "_$s4test23AnotherDistributedActorCN"
      ]
    },
      "text": {
      "global": [
        "_$s4test20SomeDistributedActorC2id0C0012LocalTestingD2IDVvpWvd",
        "_$s4test20SomeDistributedActorC15unownedExecutorScevg",
        "_$s4test27$AchievementServiceProtocolC11actorSystemAC11Distributed012LocalTestingg5ActorF0C_tcfc",
        "_$s4test20SomeDistributedActorC2id0C0012LocalTestingD2IDVvpMV",
        "_$s4test23AnotherDistributedActorCSQAAMc",
        "_$s4test26AchievementServiceProtocolPAA11Distributed01_E9ActorStubRzrlE10clearCacheyyYaKFTE",
        "_$s4test23AnotherDistributedActorCs12IdentifiableAAMc",
        "_$s4test23AnotherDistributedActorC11actorSystemAC0C0012LocalTestingcdF0C_tcfC",
        "_$s4test26AchievementServiceProtocolTL",
        "_$s4test23AnotherDistributedActorC2id0C0012LocalTestingD2IDVvpWvd",
        "_$s4test27$AchievementServiceProtocolCMn",
        "_$s4test23AnotherDistributedActorC2id0C0012LocalTestingD2IDVvpMV",
        "_$s4test27$AchievementServiceProtocolCSQAAMc",
        "_$s4test27$AchievementServiceProtocolC2id11Distributed19LocalTestingActorIDVvpWvd",
        "_$s4test20SomeDistributedActorCSHAAMc",
        "_$s4test20SomeDistributedActorC15unownedExecutorScevpMV",
        "_$s4test26AchievementServiceProtocolP11Distributed0E5ActorTb",
        "_$s4test20SomeDistributedActorC7resolve2id5usingAC0C0012LocalTestingD2IDV_AG0hicD6SystemCtKFZ",
        "_$s4test23AnotherDistributedActorCfd",
        "_$s4test27$AchievementServiceProtocolC15unownedExecutorScevg",
        "_$s4test23AnotherDistributedActorC7resolve2id5usingAC0C0012LocalTestingD2IDV_AG0hicD6SystemCtKFZ",
        "_$s4test23AnotherDistributedActorC9hashValueSivgTq",
        "_$s4test20SomeDistributedActorC9hashValueSivpMV",
        "_$s4test27$AchievementServiceProtocolCAA0bcD0AAMc",
        "_$s4test23AnotherDistributedActorC9hashValueSivpMV",
        "_$s4test23AnotherDistributedActorC11actorSystem0C0012LocalTestingcdF0CvpMV",
        "_$s4test20SomeDistributedActorC10clearCacheyyYaKFTE",
        "_$s4test23AnotherDistributedActorC10clearCacheyyYaKF",
        "_$s4test27$AchievementServiceProtocolCSEAAMc",
        "_$s4test20SomeDistributedActorC10clearCacheyyYaKF",
        "_$s4test27$AchievementServiceProtocolC2id11Distributed19LocalTestingActorIDVvg",
        "_$s4test27$AchievementServiceProtocolCs12IdentifiableAAMc",
        "_$s4test23AnotherDistributedActorCSHAAMc",
        "_$s4test20SomeDistributedActorCAA26AchievementServiceProtocolAAMc",
        "_$s4test20SomeDistributedActorC11actorSystem0C0012LocalTestingcdF0CvpWvd",
        "_$s4test20SomeDistributedActorCfd",
        "_$s4test20SomeDistributedActorC11actorSystemAC0C0012LocalTestingcdF0C_tcfCTq",
        "_$s4test27$AchievementServiceProtocolC11Distributed01_E9ActorStubAAMc",
        "_$s4test23AnotherDistributedActorC11actorSystem0C0012LocalTestingcdF0CvpWvd",
        "_$s4test26AchievementServiceProtocolMp",
        "_$s4test23AnotherDistributedActorC10clearCacheyyYaKFTE",
        "_main",
        "_$s4test20SomeDistributedActorCfD",
        "_$s4test23AnotherDistributedActorCMa",
        "_$s4test27$AchievementServiceProtocolC7resolve2id5usingAC11Distributed19LocalTestingActorIDV_AG0ijhK6SystemCtKFZ",
        "_$s4test23AnotherDistributedActorC10clearCacheyyYaKFTq",
        "_$s4test20SomeDistributedActorCSeAAMc",
        "_$s4test27$AchievementServiceProtocolC11Distributed0E5ActorAAMc",
        "_$s4test23AnotherDistributedActorC15unownedExecutorScevg",
        "_$s4test23AnotherDistributedActorCAA26AchievementServiceProtocolAAMc",
        "_$s4test23AnotherDistributedActorC2id0C0012LocalTestingD2IDVvg",
        "_$s4test20SomeDistributedActorC0C00cD0AAMc",
        "_$s4test27$AchievementServiceProtocolC9hashValueSivg",
        "_$s4test20SomeDistributedActorC11actorSystem0C0012LocalTestingcdF0Cvg",
        "_$s4test23AnotherDistributedActorCMn",
        "_$s4test20SomeDistributedActorCMa",
        "_$s4test23AnotherDistributedActorC11actorSystem0C0012LocalTestingcdF0Cvg",
        "_$s4test23AnotherDistributedActorC15unownedExecutorScevpMV",
        "_$s4test23AnotherDistributedActorCSEAAMc",
        "_$s4test23AnotherDistributedActorC0C00cD0AAMc",
        "_$s4test20SomeDistributedActorCSEAAMc",
        "_$s4test26AchievementServiceProtocolPAA11Distributed01_E9ActorStubRzrlE10clearCacheyyYaKF",
        "_$s4test27$AchievementServiceProtocolCSHAAMc",
        "_$s4test20SomeDistributedActorC11actorSystemAC0C0012LocalTestingcdF0C_tcfc",
        "_$s4test27$AchievementServiceProtocolCMa",
        "_$s4test20SomeDistributedActorC10clearCacheyyYaKFTq",
        "_$s4test20SomeDistributedActorCMn",
        "_$s4test23AnotherDistributedActorC9hashValueSivg",
        "_$s4test23AnotherDistributedActorC11actorSystemAC0C0012LocalTestingcdF0C_tcfCTq",
        "_$s4test20SomeDistributedActorC2id0C0012LocalTestingD2IDVvg",
        "_$s4test27$AchievementServiceProtocolC2id11Distributed19LocalTestingActorIDVvpMV",
        "_$s4test27$AchievementServiceProtocolCfd",
        "_$s4test20SomeDistributedActorC11actorSystemAC0C0012LocalTestingcdF0C_tcfC",
        "_$s4test20SomeDistributedActorCSQAAMc",
        "_$s4test20SomeDistributedActorC9hashValueSivgTq",
        "_$s4test23AnotherDistributedActorCfD",
        "_$s4test27$AchievementServiceProtocolC9hashValueSivpMV",
        "_$s4test27$AchievementServiceProtocolC15unownedExecutorScevpMV",
        "_$s4test27$AchievementServiceProtocolC11actorSystemAC11Distributed012LocalTestingg5ActorF0C_tcfCTq",
        "_$s4test27$AchievementServiceProtocolC11actorSystemAC11Distributed012LocalTestingg5ActorF0C_tcfC",
        "_$s4test20SomeDistributedActorCs12IdentifiableAAMc",
        "_$s4test23AnotherDistributedActorCSeAAMc",
        "_$s4test20SomeDistributedActorC9hashValueSivg",
        "_$s4test27$AchievementServiceProtocolCSeAAMc",
        "_$s4test27$AchievementServiceProtocolC11actorSystem11Distributed012LocalTestingg5ActorF0CvpMV",
        "_$s4test23AnotherDistributedActorC11actorSystemAC0C0012LocalTestingcdF0C_tcfc",
        "_$s4test27$AchievementServiceProtocolC11actorSystem11Distributed012LocalTestingg5ActorF0Cvg",
        "_$s4test27$AchievementServiceProtocolCfD",
        "_$s4test27$AchievementServiceProtocolC9hashValueSivgTq",
        "_$s4test27$AchievementServiceProtocolC11actorSystem11Distributed012LocalTestingg5ActorF0CvpWvd",
        "_$s4test20SomeDistributedActorC11actorSystem0C0012LocalTestingcdF0CvpMV"
      ]
    }
    }
  ],
  "flags": [
    {
      "attributes": [
      "not_app_extension_safe"
    ]
    }
  ],
  "install_names": [
    {
      "name": "distributed"
    }
  ],
  "swift_abi": [
    {
      "abi": 7
    }
  ],
  "target_info": [
    {
      "min_deployment": "11.0.0",
      "target": "arm64-macos"
    }
  ]
},
  "tapi_tbd_version": 5
}