// RUN: %target-swift-frontend %use_no_opaque_pointers -module-name=cgtest -emit-ir -O %s | %FileCheck %s
// RUN: %target-swift-frontend -module-name=cgtest -emit-ir -O %s

// Test some imported CG APIs
import CoreGraphics

// REQUIRES: OS=macosx
// REQUIRES: CPU=x86_64

@_silgen_name("blackHole")
func blackHole<T>(_ value: T) -> Void

// CHECK: [[SWITCHTABLE:@.*]] = private unnamed_addr constant [8 x i64] [i64 0, i64 12, i64 23, i64 34, i64 45, i64 55, i64 67, i64 71]

// CHECK-LABEL: define swiftcc i64 {{.*}}testEnums{{.*}} {
public func testEnums(_ model: CGColorSpaceModel) -> Int {
  switch model {
     case .unknown : return 0
     case .monochrome : return 12
     case .rgb : return 23
     case .cmyk : return 34
     case .lab : return 45
     case .deviceN : return 55
     case .indexed : return 67
     case .pattern : return 71

     default: return -1
  }
// CHECK:   [[GEP:%.+]] = getelementptr inbounds [8 x i64], [8 x i64]* [[SWITCHTABLE]], i64 0, i64 %{{.*}}
// CHECK:   [[LOAD:%.+]] = load i64, i64* [[GEP]], align 8
// CHECK:   [[PHI:%.*]] = phi i64 [ [[LOAD]], %{{.*}} ], [ -1, %{{.*}} ]
// CHECK:   ret i64 [[PHI]] 
}

// CHECK-LABEL: define swiftcc void {{.*}}rotationAround{{.*}} {
// Get a transform that will rotate around a given offset
public func rotationAround(offset: CGPoint, angle: CGFloat,
        transform: CGAffineTransform = .identity) -> CGAffineTransform {
  // FIXME: consistent API namings
  return transform.translatedBy(x: offset.x, y: offset.y)
                  .rotated(by: angle)
                  .translatedBy(x: -offset.x, y: -offset.y)

// CHECK:   call void @CGAffineTransformTranslate(%{{.*}}.CGAffineTransform* {{.*}}, %{{.*}}.CGAffineTransform* {{.*}},{{.*}}, {{.*}})
// CHECK:   call void @CGAffineTransformRotate(%{{.*}}.CGAffineTransform* {{.*}}, %{{.*}}.CGAffineTransform* {{.*}},{{.*}})
// CHECK:   call void @CGAffineTransformTranslate(%{{.*}}.CGAffineTransform* {{.*}}, %{{.*}}.CGAffineTransform* {{.*}},{{.*}}, {{.*}})
//
// CHECK:   ret void
}

// CHECK-LABEL: define swiftcc void {{.*}}trace{{.*}} {
public func trace(in context: CGContext, path: CGPath) {
  let red = CGColor(red: 1, green: 0, blue: 0, alpha: 1)
  context.saveGState()
  context.addPath(path)
  context.setStrokeColor(red)
  context.strokePath()
  context.restoreGState()
// CHECK:   call %{{.*}}.CGColor* @CGColorCreateGenericRGB(double 1.000000e+00, double 0.000000e+00, double 0.000000e+00, double 1.000000e+00)
// CHECK:   call void @CGContextSaveGState(%{{.*}}.CGContext* %{{.*}})
// CHECK:   call void @CGContextAddPath(%{{.*}}.CGContext* %{{.*}}, %{{.*}}.CGPath* %{{.*}})
// CHECK:   call void @CGContextSetStrokeColorWithColor(%{{.*}}.CGContext* %{{.*}}, %{{.*}}.CGColor* %{{.*}})
// CHECK:   call void @CGContextStrokePath(%{{.*}}.CGContext* %{{.*}})
// CHECK:   call void @CGContextRestoreGState(%{{.*}}.CGContext* %{{.*}})
//
// CHECK:   ret void
}

// CHECK-LABEL: define swiftcc void {{.*}}pdfOperations{{.*}} {
public func pdfOperations(_ context: CGContext) {
	context.beginPDFPage(nil)
	context.endPDFPage()
	context.closePDF()
// CHECK:   call void @CGPDFContextBeginPage(%{{.*}}.CGContext* %{{.*}}, %{{.*}}.__CFDictionary* {{.*}})
// CHECK:   call void @CGPDFContextEndPage(%{{.*}}.CGContext* %{{.*}})
// CHECK:   call void @CGPDFContextClose(%{{.*}}.CGContext* %{{.*}})
//
// CHECK:   ret void
}

// Test some more recently renamed APIs

// CHECK-LABEL: define swiftcc void {{.*}}testColorRenames{{.*}} {
@available(macOS 10.11, *)
public func testColorRenames(color: CGColor,
                             intent: CGColorRenderingIntent) {
  let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
// CHECK:   %{{.*}} = load {{.*}}%struct.__CFString** @kCGColorSpaceSRGB{{.*}}, align 8
// CHECK:   %{{.*}} = {{.*}} call %struct.CGColorSpace* @CGColorSpaceCreateWithName(%struct.__CFString* %{{.*}})

  let _ = color.converted(to: colorSpace, intent: intent, options: nil)
// CHECK:   %{{.*}} = {{.*}} call %struct.CGColor* @CGColorCreateCopyByMatchingToColorSpace(%struct.CGColorSpace* nonnull %{{.*}}, i32 %{{.*}}, %struct.CGColor* %{{.*}}, %struct.__CFDictionary* null)
//
// CHECK:   ret void
}

// CHECK-LABEL: define swiftcc void {{.*}}testRenames{{.*}} {
public func testRenames(transform: CGAffineTransform, context: CGContext,
                        point: CGPoint, size: CGSize, rect: CGRect,
                        image: CGImage,
                        edge: CGRectEdge) {
  let transform = transform.inverted().concatenating(transform)
// CHECK:   call void @CGAffineTransformInvert(%struct.CGAffineTransform* {{.*}}, %struct.CGAffineTransform* {{.*}})
// CHECK:   call void @CGAffineTransformConcat(%struct.CGAffineTransform* {{.*}}, %struct.CGAffineTransform* {{.*}}, %struct.CGAffineTransform* {{.*}})

  blackHole(point.applying(transform))
  var rect = rect.applying(transform)
  blackHole(size.applying(transform))
// CHECK:   %{{.*}} = {{(tail )?}}call { double, double } @CGPointApplyAffineTransform(double %{{.*}}, double %{{.*}}, %struct.CGAffineTransform* {{.*}})
// CHECK:   call void @CGRectApplyAffineTransform(%struct.CGRect* {{.*}}, %struct.CGRect* {{.*}}, %struct.CGAffineTransform* {{.*}})
// CHECK:   %{{.*}} = {{(tail )?}}call { double, double } @CGSizeApplyAffineTransform(double %{{.*}}, double %{{.*}}, %struct.CGAffineTransform* {{.*}})

  context.concatenate(transform)
  context.rotate(by: CGFloat.pi)
  context.scaleBy(x: 1.0, y: 1.0)
  context.translateBy(x: 1.0, y: 1.0)
// CHECK:   call void @CGContextConcatCTM(%struct.CGContext* [[CONTEXT:%[0-9]+]], %struct.CGAffineTransform* {{.*}})
// CHECK:   call void @CGContextRotateCTM(%struct.CGContext* [[CONTEXT]], double {{.*}})
// CHECK:   call void @CGContextScaleCTM(%struct.CGContext* [[CONTEXT]], double {{1\.0+.*}}, double {{1\.0+.*}})
// CHECK:   call void @CGContextTranslateCTM(%struct.CGContext* [[CONTEXT]], double {{1\.0+.*}}, double {{1\.0+.*}})

  context.clip(to: rect)
  context.clip(to: rect, mask: image)
// CHECK:   call void @CGContextClipToRect(%struct.CGContext* [[CONTEXT]], %struct.CGRect* nonnull byval({{.*}}) align 8 %{{.*}})
// CHECK:   call void @CGContextClipToMask(%struct.CGContext* [[CONTEXT]], %struct.CGRect* nonnull byval({{.*}}) align 8 %{{.*}}, %struct.CGImage* %{{.*}})

  var slice = CGRect.zero
  var remainder = CGRect.zero
  rect.__divided(slice: &slice, remainder: &remainder, atDistance: CGFloat(2.0),
          from: edge)
  assert((slice, remainder) == rect.divided(atDistance: CGFloat(2.0),
                                            from: edge))
// CHECK:   call void @CGRectDivide(%struct.CGRect* nonnull byval({{.*}}) align 8 %{{.*}}, %struct.CGRect* nonnull %{{.*}}, %struct.CGRect* nonnull %{{.*}}, double {{2\.0+.*}}, i32 %{{.*}})
//
// CHECK:   ret void
}

