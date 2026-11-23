// Makes a beta's icon from the logo: assets/UnishadeBeta.png, the logo with a BETA label across its bottom, and
// assets/UnishadeBeta.ico, the same picture at the sizes Windows draws. Run it on a Mac, from the repository's folder,
// when the logo changes:
//
//   swift scripts/beta_icon.swift

import CoreGraphics
import CoreText
import Foundation
import ImageIO

func fail(_ message: String) -> Never {
    FileHandle.standardError.write((message + "\n").data(using: .utf8)!)
    exit(1)
}

func context(_ size: Int) -> CGContext {
    CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!,
              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
}

func png(_ image: CGImage) -> Data {
    let data = NSMutableData()
    let destination = CGImageDestinationCreateWithData(data, "public.png" as CFString, 1, nil)!
    CGImageDestinationAddImage(destination, image, nil)
    guard CGImageDestinationFinalize(destination) else { fail("Could not write a PNG.") }
    return data as Data
}

let assets = URL(fileURLWithPath: "assets")
guard let source = CGImageSourceCreateWithURL(assets.appendingPathComponent("Unishade.png") as CFURL, nil),
      let logo = CGImageSourceCreateImageAtIndex(source, 0, nil), logo.width == logo.height
else { fail("assets/Unishade.png is not a square picture. Run this from the repository's folder.") }
// The launcher's own font, for the label.
guard let fonts = CTFontManagerCreateFontDescriptorsFromURL(assets.appendingPathComponent("fonts/BricolageGrotesque-ExtraBold.ttf") as CFURL)
        as? [CTFontDescriptor], let face = fonts.first
else { fail("Could not read assets/fonts/BricolageGrotesque-ExtraBold.ttf.") }

// Core Graphics measures from the bottom left. The label is as wide as the cube and its ring's inner edge, low
// enough to leave the cube alone.
let size = logo.width
let unit = CGFloat(size) / 512
let label = CGRect(x: 96 * unit, y: 20 * unit, width: 320 * unit, height: 124 * unit)
let draw = context(size)
draw.draw(logo, in: CGRect(x: 0, y: 0, width: size, height: size))
// A gap around the label, so the ring's glow does not run into it.
let gap = label.insetBy(dx: -14 * unit, dy: -14 * unit)
draw.setBlendMode(.clear)
draw.addPath(CGPath(roundedRect: gap, cornerWidth: gap.height / 2, cornerHeight: gap.height / 2, transform: nil))
draw.fillPath()
draw.setBlendMode(.normal)
// The yellow of the launcher's badges.
draw.setFillColor(red: 0xF8 / 255.0, green: 0xE4 / 255.0, blue: 0x4C / 255.0, alpha: 1)
draw.addPath(CGPath(roundedRect: label, cornerWidth: label.height / 2, cornerHeight: label.height / 2, transform: nil))
draw.fillPath()

let font = CTFontCreateWithFontDescriptor(face, 84 * unit, nil)
let text = NSAttributedString(string: "BETA", attributes: [
    NSAttributedString.Key(kCTFontAttributeName as String): font,
    NSAttributedString.Key(kCTForegroundColorAttributeName as String): CGColor(red: 0, green: 0, blue: 0, alpha: 1),
    NSAttributedString.Key(kCTKernAttributeName as String): 3 * unit,
])
let line = CTLineCreateWithAttributedString(text)
// Centered on the letters themselves, which are all capitals: the kern after the last one is not part of them.
let ink = CTLineGetBoundsWithOptions(line, .useGlyphPathBounds)
draw.textPosition = CGPoint(x: label.midX - ink.midX, y: label.midY - ink.midY)
CTLineDraw(line, draw)
guard let beta = draw.makeImage() else { fail("Could not draw the icon.") }

do {
    try png(beta).write(to: assets.appendingPathComponent("UnishadeBeta.png"))

    // An .ico is a list of pictures, here each a PNG, as Unishade.ico has them.
    let sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    let pictures = sizes.map { side -> Data in
        let small = context(side)
        small.interpolationQuality = .high
        small.draw(beta, in: CGRect(x: 0, y: 0, width: side, height: side))
        return png(small.makeImage()!)
    }
    var ico = Data()
    func append<T: FixedWidthInteger>(_ value: T) { withUnsafeBytes(of: value.littleEndian) { ico.append(contentsOf: $0) } }
    append(UInt16(0)); append(UInt16(1)); append(UInt16(sizes.count))
    var offset = 6 + 16 * sizes.count
    for (side, picture) in zip(sizes, pictures) {
        // A width and height of 0 stand for 256.
        append(UInt8(side % 256)); append(UInt8(side % 256)); append(UInt8(0)); append(UInt8(0))
        append(UInt16(1)); append(UInt16(32)); append(UInt32(picture.count)); append(UInt32(offset))
        offset += picture.count
    }
    pictures.forEach { ico.append($0) }
    try ico.write(to: assets.appendingPathComponent("UnishadeBeta.ico"))
} catch {
    fail("Could not write the icon: \(error.localizedDescription)")
}
print("Wrote assets/UnishadeBeta.png and assets/UnishadeBeta.ico")
