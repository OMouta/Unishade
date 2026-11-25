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

    // Finder writes the icons' names in black on a light desktop and in white on a dark one, whatever is behind them.
    // The launcher's accent color, darkened a little toward the bottom, is about as far from black as from white, so
    // both can be read.
    let shades = CGGradient(colorsSpace: space, colors: [color(0x6C76F5), color(0x5459D2)] as CFArray, locations: [0, 1])!
    draw.drawLinearGradient(shades, start: CGPoint(x: 0, y: height), end: CGPoint(x: 0, y: 0), options: [])

    // An arrow from the app to the Applications folder, in the gap between the two icons.
    let y = height - iconsY
    let middle = (appX + applicationsX) / 2
    draw.setStrokeColor(color(0xFFFFFF, 0.9))
    draw.setLineWidth(6)
    draw.setLineCap(.round)
    draw.setLineJoin(.round)
    draw.move(to: CGPoint(x: middle - 42, y: y))
    draw.addLine(to: CGPoint(x: middle + 42, y: y))
    draw.move(to: CGPoint(x: middle + 20, y: y + 22))
    draw.addLine(to: CGPoint(x: middle + 42, y: y))
    draw.addLine(to: CGPoint(x: middle + 20, y: y - 22))
    draw.strokePath()

    let text = NSAttributedString(string: "Drag Unishade to Applications", attributes: [
        NSAttributedString.Key(kCTFontAttributeName as String): CTFontCreateWithFontDescriptor(face, 17, nil),
        NSAttributedString.Key(kCTForegroundColorAttributeName as String): color(0xFFFFFF),
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
