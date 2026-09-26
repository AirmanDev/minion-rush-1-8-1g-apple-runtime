import AppKit

guard CommandLine.arguments.count == 2 else {
  fatalError("Usage: prepare_installer_icon.swift <iconset-directory>")
}
let destination = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
for size in [16, 32, 128, 256, 512] {
  for scale in [1, 2] {
    let pixels = size * scale
    guard
      let bitmap = NSBitmapImageRep(
        bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels,
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
        colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0
      ), let context = NSGraphicsContext(bitmapImageRep: bitmap)
    else { fatalError("Cannot create installer icon") }
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = context
    let transform = NSAffineTransform()
    transform.scale(by: CGFloat(pixels) / 1024)
    transform.concat()
    NSColor(calibratedRed: 1, green: 0.76, blue: 0.16, alpha: 1).setFill()
    NSBezierPath(rect: NSRect(x: 0, y: 0, width: 1024, height: 1024)).fill()
    NSColor(calibratedWhite: 0.12, alpha: 1).setStroke()
    let box = NSBezierPath(
      roundedRect: NSRect(x: 235, y: 230, width: 554, height: 395),
      xRadius: 48, yRadius: 48)
    box.lineWidth = 44
    box.stroke()
    let lid = NSBezierPath()
    lid.move(to: NSPoint(x: 225, y: 605))
    lid.line(to: NSPoint(x: 799, y: 605))
    lid.lineWidth = 44
    lid.stroke()
    NSColor(calibratedWhite: 0.12, alpha: 1).setFill()
    let arrow = NSBezierPath()
    arrow.move(to: NSPoint(x: 474, y: 815))
    arrow.line(to: NSPoint(x: 550, y: 815))
    arrow.line(to: NSPoint(x: 550, y: 470))
    arrow.line(to: NSPoint(x: 636, y: 470))
    arrow.line(to: NSPoint(x: 512, y: 340))
    arrow.line(to: NSPoint(x: 388, y: 470))
    arrow.line(to: NSPoint(x: 474, y: 470))
    arrow.close()
    arrow.fill()
    NSGraphicsContext.restoreGraphicsState()
    guard let data = bitmap.representation(using: .png, properties: [:])
    else { fatalError("Cannot encode installer icon") }
    let suffix = scale == 2 ? "@2x" : ""
    try data.write(to: destination.appendingPathComponent("icon_\(size)x\(size)\(suffix).png"))
  }
}
