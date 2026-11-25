// Makes the Mac app's icon from the logo: assets/UnishadeMac.png, the logo on the black rounded square that Mac icons
// are, and assets/UnishadeMacBeta.png, the same with a beta's logo. macOS 26 puts a logo without that shape on a gray
// plate of its own. Run it on a Mac, from the repository's folder, when the logo changes, after scripts/beta_icon.swift:
//
//   swift scripts/mac_icon.swift

import CoreGraphics
import Foundation
import ImageIO
import SwiftUI

func fail(_ message: String) -> Never {
    FileHandle.standardError.write((message + "\n").data(using: .utf8)!)
    exit(1)
}

func color(_ rgb: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((rgb >> 16) & 0xFF) / 255, green: CGFloat((rgb >> 8) & 0xFF) / 255, blue: CGFloat(rgb & 0xFF) / 255, alpha: alpha)
}

let assets = URL(fileURLWithPath: "assets")

// Apple's grid for a Mac icon: a square of 824 points in the middle of 1024, with corners that curve the way the
// system's own do, and room around it for the shadow.
let size: CGFloat = 1024
let square = CGRect(x: 100, y: 100, width: 824, height: 824)
let shape = RoundedRectangle(cornerRadius: 185.4, style: .continuous).path(in: square).cgPath

func icon(from name: String, to output: String) {
    guard let source = CGImageSourceCreateWithURL(assets.appendingPathComponent(name) as CFURL, nil),
          let logo = CGImageSourceCreateImageAtIndex(source, 0, nil), logo.width == logo.height
    else { fail("assets/\(name) is not a square picture. Run this from the repository's folder.") }
    guard let draw = CGContext(data: nil, width: Int(size), height: Int(size), bitsPerComponent: 8, bytesPerRow: 0,
                               space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
    else { fail("Could not start a picture.") }
    draw.interpolationQuality = .high

    // Black, like the launcher and the space around the logo, with the soft shadow Mac icons stand on.
    draw.saveGState()
    draw.setShadow(offset: CGSize(width: 0, height: -10), blur: 20, color: color(0x000000, 0.3))
    draw.addPath(shape)
    draw.setFillColor(color(0x000000))
    draw.fillPath()
    draw.restoreGState()

    draw.saveGState()
    draw.addPath(shape)
    draw.clip()
    // The ring reaches almost from edge to edge of the square.
    let side: CGFloat = 800
    draw.draw(logo, in: CGRect(x: (size - side) / 2, y: (size - side) / 2, width: side, height: side))
    // A faint edge in the launcher's border color, so the square does not vanish on a black desktop or Dock. The
    // clip leaves the inner half of the line.
    draw.addPath(shape)
    draw.setStrokeColor(color(0x22233A))
    draw.setLineWidth(8)
    draw.strokePath()
    draw.restoreGState()

    guard let image = draw.makeImage() else { fail("Could not draw the icon.") }
    let file = assets.appendingPathComponent(output)
    guard let destination = CGImageDestinationCreateWithURL(file as CFURL, "public.png" as CFString, 1, nil) else { fail("Could not write \(file.path).") }
    CGImageDestinationAddImage(destination, image, nil)
    guard CGImageDestinationFinalize(destination) else { fail("Could not write \(file.path).") }
}

icon(from: "Unishade.png", to: "UnishadeMac.png")
icon(from: "UnishadeBeta.png", to: "UnishadeMacBeta.png")
print("Wrote assets/UnishadeMac.png and assets/UnishadeMacBeta.png")
