import AppKit
import SwiftUI
import UniformTypeIdentifiers

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
    .defaultSize(width: 720, height: 820)
    .commands { CommandGroup(replacing: .newItem) {} }
  }
}

struct InstallerView: View {
  @Bindable var model: InstallerModel
  @State private var choosingArchive = false
  @State private var dropTarget = false
  @State private var showLog = false
  @State private var followLog = true

  private func text(_ key: String) -> String { model.layout.text(key) }

  var body: some View {
    VStack(spacing: 0) {
      Form {
        Section {
          Text(model.layout.title).font(.largeTitle.weight(.semibold))
          Text(model.layout.subtitle).foregroundStyle(.secondary)
        }
        ForEach(model.layout.sections) { section in
          Section(section.title) {
            switch section.id {
            case "release": releaseSection
            case "device": deviceSection
            case "signing": signingSection
            default: EmptyView()
            }
          }
        }
        DisclosureGroup(text("logs"), isExpanded: $showLog) {
          HStack {
            Button(text("openLog")) {
              if let file = model.logFile { NSWorkspace.shared.open(file) }
            }.disabled(model.logFile == nil)
              .accessibilityIdentifier("installer.openLog")
            Spacer()
            Toggle(text("followLog"), isOn: $followLog).toggleStyle(.checkbox)
          }
          ScrollViewReader { reader in
            ScrollView {
              Text(model.log)
                .font(.system(.caption, design: .monospaced))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
              Color.clear.frame(height: 1).id("logEnd")
            }.frame(height: 160)
              .onChange(of: model.log) {
                if followLog { reader.scrollTo("logEnd", anchor: .bottom) }
              }
          }
        }
      }
      .formStyle(.grouped)
      Divider()
      HStack(spacing: 16) {
        if model.busy { ProgressView().controlSize(.small) }
        Text(model.status)
          .font(.callout)
          .foregroundStyle(model.error == nil ? Color.primary : Color.red)
          .textSelection(.enabled)
          .accessibilityIdentifier("installer.status")
        Spacer(minLength: 8)
        if model.busy {
          Button(text("cancel")) { model.cancel() }
            .disabled(model.cancelling)
            .accessibilityIdentifier("installer.cancel")
        }
        Button(text("install")) { model.install() }
          .buttonStyle(.glassProminent)
          .keyboardShortcut(.defaultAction)
          .disabled(!model.canInstall)
          .accessibilityIdentifier("installer.install")
      }.padding(20)
    }
    .frame(minWidth: model.layout.minimumWidth)
    .onChange(of: model.busy) {
      if model.busy { showLog = true }
    }
    .toolbar {
      ToolbarItem {
        Button(text("workspace"), systemImage: "folder") {
          NSWorkspace.shared.open(model.workspace)
        }.help(text("workspace"))
      }
    }
    .fileImporter(isPresented: $choosingArchive, allowedContentTypes: [.zip]) { result in
      switch result {
      case .success(let url): model.importArchive(url)
      case .failure(let error): model.error = error.localizedDescription
      }
    }
  }

  private var releaseSection: some View {
    VStack(alignment: .leading, spacing: 12) {
      VStack(spacing: 12) {
        Image(systemName: "archivebox").font(.system(size: 32)).foregroundStyle(.secondary)
        Text(text("drop")).font(.headline)
        Button(text("choose")) { choosingArchive = true }
          .disabled(model.busy)
          .accessibilityIdentifier("installer.chooseArchive")
      }
      .frame(maxWidth: .infinity)
      .padding(20)
      .background(dropTarget ? Color.accentColor.opacity(0.1) : Color.clear)
      .overlay {
        RoundedRectangle(cornerRadius: 12)
          .strokeBorder(.secondary, style: StrokeStyle(lineWidth: 1, dash: [6]))
      }
      .dropDestination(for: URL.self) { urls, _ in
        guard !model.busy, urls.count == 1, let url = urls.first,
          url.isFileURL, url.pathExtension.lowercased() == "zip"
        else { return false }
        model.importArchive(url)
        return true
      } isTargeted: {
        dropTarget = $0
      }
      if let summary = model.assets {
        Label(
          text("assetsReady") + " - "
            + ByteCountFormatter.string(fromByteCount: summary.bytes, countStyle: .file),
          systemImage: "checkmark.circle.fill"
        ).foregroundStyle(.green)
      } else {
        Text(text("noAssets")).foregroundStyle(.secondary)
      }
      Text(text("releaseHelp")).font(.caption).foregroundStyle(.secondary)
    }
  }

  private var deviceSection: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        Picker(text("device"), selection: $model.selectedDevice) {
          if model.devices.isEmpty { Text(text("noDevices")).tag("") }
          ForEach(model.devices) { device in
            Text(device.name + " - " + device.os).tag(device.id)
          }
        }.disabled(model.busy || model.devices.isEmpty)
          .accessibilityIdentifier("installer.device")
        Button(text("refresh")) { model.refresh() }
          .disabled(model.busy)
          .accessibilityIdentifier("installer.refresh")
      }
      Text(text("deviceHelp")).font(.caption).foregroundStyle(.secondary)
    }
  }

  private var signingSection: some View {
    VStack(alignment: .leading, spacing: 12) {
      if !model.teams.isEmpty {
        Picker(text("savedTeam"), selection: $model.team) {
          if !model.teams.contains(where: { $0.id == model.team }) {
            Text(text("manualTeam")).tag(model.team)
          }
          ForEach(model.teams) { team in
            Text(team.name + " - " + team.id).tag(team.id)
          }
        }
      }
      TextField(text("team"), text: $model.team, prompt: Text(text("teamPlaceholder")))
        .accessibilityIdentifier("installer.team")
      TextField(text("bundle"), text: $model.bundleID)
        .accessibilityIdentifier("installer.bundle")
      Text(text("signingHelp")).font(.caption).foregroundStyle(.secondary)
      Text(text("personalTeam")).font(.caption).foregroundStyle(.secondary)
      HStack {
        Button(text("openXcode")) {
          if let url = NSWorkspace.shared.urlForApplication(
            withBundleIdentifier: "com.apple.dt.Xcode"
          ) {
            NSWorkspace.shared.openApplication(
              at: url, configuration: NSWorkspace.OpenConfiguration()
            )
          }
        }
        Link(
          text("accountHelp"),
          destination: URL(
            string: "https://developer.apple.com/help/account/basics/about-your-developer-account"
          )!)
      }
    }.disabled(model.busy)
  }
}
