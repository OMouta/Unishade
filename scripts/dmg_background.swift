// Draws the picture behind the icons in the window of Unishade's disk image for the Mac: assets/dmg/background.png, and
// assets/dmg/background@2x.png for Retina screens. scripts/dmg_settings.py puts the app's icon and the Applications
// folder's on it. Run it on a Mac, from the repository's folder, to change the picture:
//
//   swift scripts/dmg_background.swift

import CoreGraphics
import CoreText
import Foundation
import ImageIO

func fail(_ message: String) -> Never {
    FileHandle.standardError.write((message + "\n").data(using: .utf8)!)
    exit(1)
}

func color(_ rgb: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((rgb >> 16) & 0xFF) / 255, green: CGFloat((rgb >> 8) & 0xFF) / 255, blue: CGFloat(rgb & 0xFF) / 255, alpha: alpha)
}

// The window's size in points, and where the middles of the two icons are, measured from the top left as Finder does.
// scripts/dmg_settings.py has the same numbers.
let width: CGFloat = 660
let height: CGFloat = 400
let iconsY: CGFloat = 180
let appX: CGFloat = 170
let applicationsX: CGFloat = 490

let assets = URL(fileURLWithPath: "assets")
// The launcher's own font, for the line under the icons.
guard let fonts = CTFontManagerCreateFontDescriptorsFromURL(assets.appendingPathComponent("fonts/BricolageGrotesque-SemiBold.ttf") as CFURL)
        as? [CTFontDescriptor], let face = fonts.first
else { fail("Could not read assets/fonts/BricolageGrotesque-SemiBold.ttf. Run this from the repository's folder.") }

func picture(scale: CGFloat) -> CGImage {
    let space = CGColorSpace(name: CGColorSpace.sRGB)!
    guard let draw = CGContext(data: nil, width: Int(width * scale), height: Int(height * scale), bitsPerComponent: 8, bytesPerRow: 0, space: space,
                               bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
    else { fail("Could not start a picture.") }
    // Core Graphics measures from the bottom left, in points from here on.
    draw.scaleBy(x: scale, y: scale)

    // Black, like the launcher and the space around the logo.
    draw.setFillColor(color(0x000000))
    draw.fill(CGRect(x: 0, y: 0, width: width, height: height))

    // Finder writes the icons' names in black on any background a disk image sets, also on a dark desktop, so each
    // name gets a pill in the white of the logo's cube to stand on. A name's middle is 83 points below its icon's.
    draw.setFillColor(color(0xF2F2FB))
    for x in [appX, applicationsX] {
        let pill = CGRect(x: x - 58, y: height - iconsY - 83 - 13, width: 116, height: 26)
        draw.addPath(CGPath(roundedRect: pill, cornerWidth: 13, cornerHeight: 13, transform: nil))
    }
    draw.fillPath()

    // An arrow from the app to the Applications folder, in the gap between the two icons.
    let y = height - iconsY
    let middle = (appX + applicationsX) / 2
    draw.setStrokeColor(color(0xF2F2FB))
    draw.setLineWidth(4)
    draw.setLineCap(.round)
    draw.setLineJoin(.round)
    draw.move(to: CGPoint(x: middle - 34, y: y))
    draw.addLine(to: CGPoint(x: middle + 34, y: y))
    draw.move(to: CGPoint(x: middle + 16, y: y + 18))
    draw.addLine(to: CGPoint(x: middle + 34, y: y))
    draw.addLine(to: CGPoint(x: middle + 16, y: y - 18))
    draw.strokePath()

    let text = NSAttributedString(string: "Drag Unishade to Applications", attributes: [
        NSAttributedString.Key(kCTFontAttributeName as String): CTFontCreateWithFontDescriptor(face, 15, nil),
        NSAttributedString.Key(kCTForegroundColorAttributeName as String): color(0xA2A5C8),
    ])
    let line = CTLineCreateWithAttributedString(text)
    let ink = CTLineGetBoundsWithOptions(line, .useGlyphPathBounds)
    // Below the icons' names, and clear of the bottom edge, which a taller title bar cuts a few points off.
    draw.textPosition = CGPoint(x: width / 2 - ink.midX, y: 62 - ink.midY)
    CTLineDraw(line, draw)

    guard let image = draw.makeImage() else { fail("Could not draw the picture.") }
    return image
}

func write(_ image: CGImage, to name: String, scale: CGFloat) {
    let file = assets.appendingPathComponent("dmg").appendingPathComponent(name)
    guard let destination = CGImageDestinationCreateWithURL(file as CFURL, "public.png" as CFString, 1, nil) else { fail("Could not write \(file.path).") }
    // The resolution tells the two pictures apart as one picture at two sizes.
    let dots = 72 * scale
    CGImageDestinationAddImage(destination, image, [kCGImagePropertyDPIWidth: dots, kCGImagePropertyDPIHeight: dots] as CFDictionary)
    guard CGImageDestinationFinalize(destination) else { fail("Could not write \(file.path).") }
}

do {
    try FileManager.default.createDirectory(at: assets.appendingPathComponent("dmg"), withIntermediateDirectories: true)
} catch {
    fail("Could not create assets/dmg: \(error.localizedDescription)")
}
write(picture(scale: 1), to: "background.png", scale: 1)
write(picture(scale: 2), to: "background@2x.png", scale: 2)
print("Wrote assets/dmg/background.png and assets/dmg/background@2x.png")
