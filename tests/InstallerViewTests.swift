import AppKit
import SwiftUI

@MainActor
enum InstallerViewTests {
  static func run(model: InstallerModel, snapshots: URL?) throws {
    let application = NSApplication.shared
    application.setActivationPolicy(.accessory)
    let window = NSWindow(
      contentRect: NSRect(x: 0, y: 0, width: 740, height: 880),
      styleMask: [.titled], backing: .buffered, defer: false)
    let host = NSHostingView(rootView: InstallerView(model: model))
    window.contentView = host
    window.center()
    window.orderFront(nil)
    defer { window.orderOut(nil) }
    if let snapshots {
      try FileManager.default.createDirectory(at: snapshots, withIntermediateDirectories: true)
    }

    func settle() {
      RunLoop.current.run(until: Date().addingTimeInterval(0.15))
      host.layoutSubtreeIfNeeded()
      host.displayIfNeeded()
    }

    func popUps(in view: NSView) -> [NSPopUpButton] {
      (view as? NSPopUpButton).map { [$0] } ?? view.subviews.flatMap { popUps(in: $0) }
    }

    func control(_ identifier: String) throws -> NSPopUpButton {
      guard
        let button = popUps(in: host).first(where: { $0.accessibilityIdentifier() == identifier })
      else {
        throw NSError(
          domain: "InstallerViewTests", code: 1,
          userInfo: [NSLocalizedDescriptionKey: "Missing native selector: " + identifier])
      }
      return button
    }

    func capture(_ name: String) throws {
      guard let snapshots else { return }
      let task = Process()
      task.executableURL = URL(fileURLWithPath: "/usr/sbin/screencapture")
      task.arguments = [
        "-x", "-o", "-l", String(window.windowNumber),
        snapshots.appendingPathComponent(name + ".png").path,
      ]
      try task.run()
      task.waitUntilExit()
      try InstallerTests.require(task.terminationStatus == 0, "Own-window screenshot failed")
    }

    model.busy = false
    model.error = nil
    model.assets = AssetSummary(files: 100, bytes: 749_900_000)
    model.team = "ABCDEFGHIJ"
    model.bundleID = "org.example.minionrush"
    model.teams = [SigningTeam(id: model.team, name: "Personal Team")]
    model.archiveName = "Minion Rush 1.8.1g.zip"
    window.appearance = NSAppearance(named: .aqua)
    settle()
    try capture("ready-light")
    let device = try control("installer.device")
    try InstallerTests.require(
      device.bounds.width > 350, "Device menu must span its row")
    try InstallerTests.require(device.bounds.height >= 44, "Device selector target height")
    if let parent = device.superview {
      for point in [NSPoint(x: 4, y: 4), NSPoint(x: device.bounds.width - 4, y: 40)] {
        try InstallerTests.require(
          device.hitTest(device.convert(point, to: parent)) === device,
          "The full selector rectangle must receive clicks")
      }
    }
    let team = try control("installer.savedTeam")
    try InstallerTests.require(team.bounds.width > 600, "Team selector must span its row")
    try InstallerTests.require(
      device.itemTitles.contains("Test iPhone - 27.0"), "Device selection labels")
    device.selectItem(at: 0)
    _ = application.sendAction(device.action!, to: device.target, from: device)
    try InstallerTests.require(model.selectedDevice.isEmpty, "Native selector writes its binding")
    device.selectItem(at: 1)
    _ = application.sendAction(device.action!, to: device.target, from: device)
    try InstallerTests.require(
      model.selectedDevice == "selected", "Native selection uses the exact ID")
    try capture("ready-light")
    window.appearance = NSAppearance(named: .darkAqua)
    settle()
    try capture("ready-dark")
    window.setContentSize(NSSize(width: 640, height: 660))
    settle()
    try capture("minimum-window")
    window.setContentSize(NSSize(width: 740, height: 880))
    model.error = "The selected device is unavailable. Unlock it and choose Refresh."
    settle()
    try capture("failure")
    model.error = nil
    model.busy = true
    model.phase = "Building signed application"
    settle()
    try InstallerTests.require(!device.isEnabled, "Busy selector is disabled")
    try InstallerTests.require(!model.canInstall, "Busy Install is disabled")
    try capture("working")
    print("INSTALLER: native selector and layout tests passed")
  }
}
