import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

private let sourceSize = 576
private let outputSize = 1024
private let minimumRed = 200
private let maximumGreen = 150
private let maximumBlue = 130

private func fail(_ message: String, status: Int32 = 1) -> Never {
  fputs("ERROR: \(message)\n", stderr)
  exit(status)
}

guard CommandLine.arguments.count == 3 else {
  fail("usage: prepare_app_icon.swift <source> <destination>", status: 2)
}

let sourceURL = URL(fileURLWithPath: CommandLine.arguments[1]) as CFURL
let destinationURL = URL(fileURLWithPath: CommandLine.arguments[2]) as CFURL
guard
  let imageSource = CGImageSourceCreateWithURL(sourceURL, nil),
  let source = CGImageSourceCreateImageAtIndex(imageSource, 0, nil),
  let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)
else {
  fail("cannot read source icon")
}
guard source.width == sourceSize, source.height == sourceSize else {
  fail("source icon must be 576x576 pixels")
}

let width = source.width
let height = source.height
let bytesPerRow = width * 4
let byteCount = bytesPerRow * height
let pixels = UnsafeMutablePointer<UInt8>.allocate(capacity: byteCount)
pixels.initialize(repeating: 0, count: byteCount)
defer { pixels.deallocate() }

let sourceAlpha = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue)
let sourceBitmapInfo = CGBitmapInfo.byteOrder32Big.union(sourceAlpha)
guard
  let sourceContext = CGContext(
    data: pixels,
    width: width,
    height: height,
    bitsPerComponent: 8,
    bytesPerRow: bytesPerRow,
    space: colorSpace,
    bitmapInfo: sourceBitmapInfo.rawValue
  )
else {
  fail("cannot create source icon canvas")
}
sourceContext.draw(source, in: CGRect(x: 0, y: 0, width: width, height: height))

private func isOuterRed(_ pixel: Int) -> Bool {
  let offset = pixel * 4
  let alpha = Int(pixels[offset + 3])
  if alpha == 0 {
    return true
  }
  let red = min(255, Int(pixels[offset]) * 255 / alpha)
  let green = min(255, Int(pixels[offset + 1]) * 255 / alpha)
  let blue = min(255, Int(pixels[offset + 2]) * 255 / alpha)
  return red >= minimumRed && green <= maximumGreen && blue <= maximumBlue
}

let cardInset = width * 9 / 100
let cardRadius = width / 8
private func isInsideCard(_ x: Int, _ y: Int) -> Bool {
  let minimum = cardInset
  let maximum = width - cardInset - 1
  if x < minimum || x > maximum || y < minimum || y > maximum {
    return false
  }
  let centerX = min(max(x, minimum + cardRadius), maximum - cardRadius)
  let centerY = min(max(y, minimum + cardRadius), maximum - cardRadius)
  let deltaX = x - centerX
  let deltaY = y - centerY
  return deltaX * deltaX + deltaY * deltaY <= cardRadius * cardRadius
}

private func isForegroundEvidence(_ pixel: Int) -> Bool {
  if isOuterRed(pixel) {
    return false
  }
  let offset = pixel * 4
  let alpha = Int(pixels[offset + 3])
  if alpha == 0 {
    return false
  }
  let red = min(255, Int(pixels[offset]) * 255 / alpha)
  let green = min(255, Int(pixels[offset + 1]) * 255 / alpha)
  let blue = min(255, Int(pixels[offset + 2]) * 255 / alpha)
  return min(red, green, blue) < 210
}

private func hasInwardSubjectSupport(_ x: Int, _ y: Int) -> Bool {
  let direction: Int
  if y < cardInset {
    direction = 1
  } else if y >= height - cardInset {
    direction = -1
  } else {
    return false
  }
  var evidence = 0
  for distance in 1...8 {
    let sampleY = y + direction * distance
    if sampleY >= 0 && sampleY < height
      && isForegroundEvidence(sampleY * width + x)
    {
      evidence += 1
    }
  }
  return evidence >= 4
}

var visited = [Bool](repeating: false, count: width * height)
var queue: [Int] = []
queue.reserveCapacity(width * height)
private func enqueue(_ pixel: Int) {
  if !visited[pixel] && isOuterRed(pixel) {
    visited[pixel] = true
    queue.append(pixel)
  }
}

for x in 0..<width {
  enqueue(x)
  enqueue((height - 1) * width + x)
}
for y in 0..<height {
  enqueue(y * width)
  enqueue(y * width + width - 1)
}

var head = 0
while head < queue.count {
  let pixel = queue[head]
  head += 1
  let x = pixel % width
  let y = pixel / width
  if x > 0 { enqueue(pixel - 1) }
  if x + 1 < width { enqueue(pixel + 1) }
  if y > 0 { enqueue(pixel - width) }
  if y + 1 < height { enqueue(pixel + width) }
}

var protected = [Bool](repeating: false, count: width * height)
for pixel in 0..<(width * height) {
  let x = pixel % width
  let y = pixel / width
  protected[pixel] = !isInsideCard(x, y) && hasInwardSubjectSupport(x, y)
}

for pixel in 0..<(width * height) {
  let offset = pixel * 4
  let x = pixel % width
  let y = pixel / width
  let outsideCard = !isInsideCard(x, y)
  if visited[pixel] || (outsideCard && (!protected[pixel] || isOuterRed(pixel))) {
    pixels[offset] = 255
    pixels[offset + 1] = 255
    pixels[offset + 2] = 255
    pixels[offset + 3] = 255
    continue
  }
  let alpha = Int(pixels[offset + 3])
  pixels[offset] = UInt8(min(255, Int(pixels[offset]) + 255 - alpha))
  pixels[offset + 1] = UInt8(min(255, Int(pixels[offset + 1]) + 255 - alpha))
  pixels[offset + 2] = UInt8(min(255, Int(pixels[offset + 2]) + 255 - alpha))
  pixels[offset + 3] = 255
}

guard let flattened = sourceContext.makeImage() else {
  fail("cannot flatten source icon")
}

let outputAlpha = CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue)
let outputBitmapInfo = CGBitmapInfo.byteOrder32Big.union(outputAlpha)
guard
  let outputContext = CGContext(
    data: nil,
    width: outputSize,
    height: outputSize,
    bitsPerComponent: 8,
    bytesPerRow: outputSize * 4,
    space: colorSpace,
    bitmapInfo: outputBitmapInfo.rawValue
  )
else {
  fail("cannot create destination icon canvas")
}
outputContext.setFillColor(CGColor(gray: 1, alpha: 1))
outputContext.fill(CGRect(x: 0, y: 0, width: outputSize, height: outputSize))
outputContext.interpolationQuality = CGInterpolationQuality.high
outputContext.draw(
  flattened,
  in: CGRect(x: 0, y: 0, width: outputSize, height: outputSize)
)

guard
  let output = outputContext.makeImage(),
  let destination = CGImageDestinationCreateWithURL(
    destinationURL,
    UTType.png.identifier as CFString,
    1,
    nil
  )
else {
  fail("cannot create destination icon")
}
CGImageDestinationAddImage(destination, output, nil)
guard CGImageDestinationFinalize(destination) else {
  fail("cannot write destination icon")
}
