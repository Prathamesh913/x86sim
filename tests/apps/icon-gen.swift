// x86Code: tiny icon generator, compiled and run on the Mac host (not part of any app).
// usage: mkicon <out.png> <size> <letter> <rrggbb>
import Foundation
import CoreGraphics
import CoreText
import ImageIO

let args = CommandLine.arguments
guard args.count == 5, let size = Int(args[2]), let hex = UInt32(args[4], radix: 16) else {
    FileHandle.standardError.write("usage: mkicon <out.png> <size> <letter> <rrggbb>\n".data(using: .utf8)!)
    exit(2)
}
let n = size
let ctx = CGContext(data: nil, width: n, height: n, bitsPerComponent: 8, bytesPerRow: 0,
                    space: CGColorSpaceCreateDeviceRGB(),
                    bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
ctx.setFillColor(CGColor(red: CGFloat((hex >> 16) & 255) / 255,
                         green: CGFloat((hex >> 8) & 255) / 255,
                         blue: CGFloat(hex & 255) / 255, alpha: 1))
ctx.fill(CGRect(x: 0, y: 0, width: n, height: n))

let font = CTFontCreateWithName("Helvetica-Bold" as CFString, CGFloat(n) * 0.6, nil)
let attrs: [NSAttributedString.Key: Any] = [
    NSAttributedString.Key(kCTFontAttributeName as String): font,
    NSAttributedString.Key(kCTForegroundColorAttributeName as String): CGColor(gray: 1, alpha: 1),
]
let line = CTLineCreateWithAttributedString(NSAttributedString(string: args[3], attributes: attrs))
let b = CTLineGetBoundsWithOptions(line, [])
ctx.textPosition = CGPoint(x: (CGFloat(n) - b.width) / 2 - b.minX, y: (CGFloat(n) - b.height) / 2 - b.minY)
CTLineDraw(line, ctx)

let image = ctx.makeImage()!
let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: args[1]) as CFURL, "public.png" as CFString, 1, nil)!
CGImageDestinationAddImage(dest, image, nil)
exit(CGImageDestinationFinalize(dest) ? 0 : 1)
