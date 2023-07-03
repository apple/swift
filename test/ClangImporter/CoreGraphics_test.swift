// RUN: %target-swift-frontend -module-name=cgtest -emit-ir -O %s | %FileCheck %s

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
// CHECK:   [[GEP:%.+]] = getelementptr inbounds [8 x i64], ptr [[SWITCHTABLE]], i64 0, i64 %{{.*}}
// CHECK:   [[LOAD:%.+]] = load i64, ptr [[GEP]], align 8
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

// CHECK:   call void @CGAffineTransformTranslate(ptr {{.*}}, ptr {{.*}},{{.*}}, {{.*}})
// CHECK:   call void @CGAffineTransformRotate(ptr {{.*}}, ptr {{.*}},{{.*}})
// CHECK:   call void @CGAffineTransformTranslate(ptr {{.*}}, ptr {{.*}},{{.*}}, {{.*}})
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
// CHECK:   call ptr @CGColorCreateGenericRGB(double 1.000000e+00, double 0.000000e+00, double 0.000000e+00, double 1.000000e+00)
// CHECK:   call void @CGContextSaveGState(ptr %{{.*}})
// CHECK:   call void @CGContextAddPath(ptr %{{.*}}, ptr %{{.*}})
// CHECK:   call void @CGContextSetStrokeColorWithColor(ptr %{{.*}}, ptr %{{.*}})
// CHECK:   call void @CGContextStrokePath(ptr %{{.*}})
// CHECK:   call void @CGContextRestoreGState(ptr %{{.*}})
//
// CHECK:   ret void
}

// CHECK-LABEL: define swiftcc void {{.*}}pdfOperations{{.*}} {
public func pdfOperations(_ context: CGContext) {
	context.beginPDFPage(nil)
	context.endPDFPage()
	context.closePDF()
// CHECK:   call void @CGPDFContextBeginPage(ptr %{{.*}}, ptr {{.*}})
// CHECK:   call void @CGPDFContextEndPage(ptr %{{.*}})
// CHECK:   call void @CGPDFContextClose(ptr %{{.*}})
//
// CHECK:   ret void
}

// Test some more recently renamed APIs

// CHECK-LABEL: define swiftcc void {{.*}}testColorRenames{{.*}} {
@available(macOS 10.11, *)
public func testColorRenames(color: CGColor,
                             intent: CGColorRenderingIntent) {
  let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
// CHECK:   %{{.*}} = load {{.*}}ptr @kCGColorSpaceSRGB{{.*}}, align 8
// CHECK:   %{{.*}} = {{.*}} call ptr @CGColorSpaceCreateWithName(ptr %{{.*}})

  let _ = color.converted(to: colorSpace, intent: intent, options: nil)
// CHECK:   %{{.*}} = {{.*}} call ptr @CGColorCreateCopyByMatchingToColorSpace(ptr nonnull %{{.*}}, i32 %{{.*}}, ptr %{{.*}}, ptr null)
//
// CHECK:   ret void
}

// CHECK-LABEL: define swiftcc void {{.*}}testRenames{{.*}} {
public func testRenames(transform: CGAffineTransform, context: CGContext,
                        point: CGPoint, size: CGSize, rect: CGRect,
                        image: CGImage,
                        edge: CGRectEdge) {
  let transform = transform.inverted().concatenating(transform)
// CHECK:   call void @CGAffineTransformInvert(ptr {{.*}}, ptr {{.*}})
// CHECK:   call void @CGAffineTransformConcat(ptr {{.*}}, ptr {{.*}}, ptr {{.*}})

  blackHole(point.applying(transform))
  var rect = rect.applying(transform)
  blackHole(size.applying(transform))
// CHECK:   %{{.*}} = {{(tail )?}}call { double, double } @CGPointApplyAffineTransform(double %{{.*}}, double %{{.*}}, ptr {{.*}})
// CHECK:   call void @CGRectApplyAffineTransform(ptr {{.*}}, ptr {{.*}}, ptr {{.*}})
// CHECK:   %{{.*}} = {{(tail )?}}call { double, double } @CGSizeApplyAffineTransform(double %{{.*}}, double %{{.*}}, ptr {{.*}})

  context.concatenate(transform)
  context.rotate(by: CGFloat.pi)
  context.scaleBy(x: 1.0, y: 1.0)
  context.translateBy(x: 1.0, y: 1.0)
// CHECK:   call void @CGContextConcatCTM(ptr [[CONTEXT:%[0-9]+]], ptr {{.*}})
// CHECK:   call void @CGContextRotateCTM(ptr [[CONTEXT]], double {{.*}})
// CHECK:   call void @CGContextScaleCTM(ptr [[CONTEXT]], double {{1\.0+.*}}, double {{1\.0+.*}})
// CHECK:   call void @CGContextTranslateCTM(ptr [[CONTEXT]], double {{1\.0+.*}}, double {{1\.0+.*}})

  context.clip(to: rect)
  context.clip(to: rect, mask: image)
// CHECK:   call void @CGContextClipToRect(ptr [[CONTEXT]], ptr nonnull byval({{.*}}) align 8 %{{.*}})
// CHECK:   call void @CGContextClipToMask(ptr [[CONTEXT]], ptr nonnull byval({{.*}}) align 8 %{{.*}}, ptr %{{.*}})

  var slice = CGRect.zero
  var remainder = CGRect.zero
  rect.__divided(slice: &slice, remainder: &remainder, atDistance: CGFloat(2.0),
          from: edge)
  assert((slice, remainder) == rect.divided(atDistance: CGFloat(2.0),
                                            from: edge))
// CHECK:   call void @CGRectDivide(ptr nonnull byval({{.*}}) align 8 %{{.*}}, ptr nonnull %{{.*}}, ptr nonnull %{{.*}}, double {{2\.0+.*}}, i32 %{{.*}})
//
// CHECK:   ret void
}

