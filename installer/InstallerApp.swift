import AppKit
import SwiftUI

@MainActor
final class InstallerDelegate: NSObject, NSApplicationDelegate {
  var model: InstallerModel?

  func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
    guard let model, model.busy else { return .terminateNow }
    let alert = NSAlert()
    alert.messageText = model.layout.text("quitTitle")
    alert.informativeText = model.layout.text("quitHelp")
    alert.addButton(withTitle: model.layout.text("keepRunning"))
    alert.addButton(withTitle: model.layout.text("quit"))
    guard alert.runModal() == .alertSecondButtonReturn else { return .terminateCancel }
    model.cancel(quit: true)
    return .terminateLater
  }
}

@main
struct InstallerApp: App {
  @NSApplicationDelegateAdaptor(InstallerDelegate.self) private var delegate
  @State private var model: InstallerModel?
  @State private var startupError: String?

  var body: some Scene {
    Window("Minion Rush Installer", id: "installer") {
      Group {
        if let model {
          InstallerView(model: model)
        } else {
          ContentUnavailableView(
            "Installer unavailable", systemImage: "exclamationmark.triangle",
            description: Text(startupError ?? "Loading...")
          )
        }
      }
      .frame(minWidth: 640, minHeight: 660)
      .task {
        guard model == nil, startupError == nil else { return }
        do {
          guard let resources = Bundle.main.resourceURL else {
            throw CocoaError(.fileNoSuchFile)
          }
          let loaded = try InstallerModel(resources: resources)
          model = loaded
          delegate.model = loaded
          loaded.refresh()
        } catch {
          startupError = error.localizedDescription
        }
      }
    }
    .defaultSize(width: 740, height: 880)
    .commands { CommandGroup(replacing: .newItem) {} }
  }
}
