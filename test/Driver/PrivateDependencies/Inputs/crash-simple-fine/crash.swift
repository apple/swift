# Fine-grained v0
---
allNodes:
   - key:
       kind:            sourceFileProvide
       aspect:          interface
       context:         ''
       name:            './crash.swiftdeps'
     fingerprint:     72e95f4a11b98227c1f6ad6ea7f6cdba
     sequenceNumber:  0
     defsIDependUpon: [ 5, 4, 2 ]
     isProvides:      true
   - key:
       kind:            sourceFileProvide
       aspect:          implementation
       context:         ''
       name:            './crash.swiftdeps'
     fingerprint:     72e95f4a11b98227c1f6ad6ea7f6cdba
     sequenceNumber:  1
     defsIDependUpon: [  ]
     isProvides:      true
   - key:
       kind:            topLevel
       aspect:          interface
       context:         ''
       name:            a
     sequenceNumber:  2
     defsIDependUpon: [ 0 ]
     isProvides:      true
   - key:
       kind:            topLevel
       aspect:          implementation
       context:         ''
       name:            a
     sequenceNumber:  3
     defsIDependUpon: [  ]
     isProvides:      true
   - key:
       kind:            topLevel
       aspect:          interface
       context:         ''
       name:            IntegerLiteralType
     sequenceNumber:  4
     defsIDependUpon: [  ]
     isProvides:      false
   - key:
       kind:            topLevel
       aspect:          interface
       context:         ''
       name:            FloatLiteralType
     sequenceNumber:  5
     defsIDependUpon: [  ]
     isProvides:      false
...
