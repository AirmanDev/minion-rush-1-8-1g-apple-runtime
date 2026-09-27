import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct InstallerView: View {
  @Bindable var model: InstallerModel
  @State private var choosingArchive = false
  @State private var dropTarget = false
  @State private var showLog = false
  @State private var followLog = true

  private func text(_ key: String) -> String { model.layout.text(key) }

  var body: some View {
    VStack(spacing: 0) {
      ScrollViewReader { reader in
        ScrollView {
          VStack(alignment: .leading, spacing: 18) {
            VStack(alignment: .leading, spacing: 6) {
              Text(model.layout.title).font(.largeTitle.weight(.semibold))
              Text(model.layout.subtitle).foregroundStyle(.secondary)
            }.padding(.vertical, 4)
            ForEach(model.layout.sections) { section in
              switch section.id {
              case "release":
                InstallerCard(title: section.title, complete: model.assets != nil) {
                  releaseSection
                }
              case "device":
                InstallerCard(title: section.title, complete: model.selectedDeviceInfo != nil) {
                  deviceSection
                }
              case "signing":
                InstallerCard(title: section.title, complete: model.validSigning) { signingSection }
              default: EmptyView()
              }
            }
            activityLog.id("activityLog")
          }.padding(24)
        }
        .background(Color(nsColor: .windowBackgroundColor))
        .onChange(of: showLog) {
          if showLog { reader.scrollTo("activityLog", anchor: .bottom) }
        }
      }
      Divider()
      footer
    }
    .frame(minWidth: model.layout.minimumWidth)
    .onChange(of: model.operation) {
      if let operation = model.operation, operation != .refresh { showLog = true }
    }
    .onChange(of: model.error) { if model.error != nil { showLog = true } }
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
      Button {
        choosingArchive = true
      } label: {
        HStack(spacing: 16) {
          Image(systemName: "archivebox").font(.system(size: 28)).foregroundStyle(.secondary)
          VStack(alignment: .leading, spacing: 5) {
            Text(text("drop")).font(.headline)
            Text(model.archiveName ?? text("choose")).foregroundStyle(.secondary)
              .lineLimit(1).truncationMode(.middle)
          }
          Spacer(minLength: 0)
          Image(systemName: "plus.circle").font(.title2).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, minHeight: 68, alignment: .leading)
        .padding(16)
        .background(dropTarget ? Color.accentColor.opacity(0.1) : Color.clear)
        .overlay {
          RoundedRectangle(cornerRadius: 10)
            .strokeBorder(.secondary, style: StrokeStyle(lineWidth: 1, dash: [6]))
        }
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .keyboardShortcut("o", modifiers: .command)
      .disabled(model.busy)
      .accessibilityIdentifier("installer.chooseArchive")
      .accessibilityLabel(text("choose"))
      .dropDestination(for: URL.self) { urls, _ in
        guard !model.busy else { return false }
        guard urls.count == 1, let url = urls.first,
          url.isFileURL, url.pathExtension.lowercased() == "zip"
        else {
          model.error = text("invalidDrop")
          return false
        }
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
        ).font(.callout).foregroundStyle(.green)
      } else {
        Text(text("noAssets")).font(.callout).foregroundStyle(.secondary)
      }
      helpText("releaseHelp")
    }
  }

  private var deviceSection: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack(alignment: .bottom, spacing: 12) {
        InstallerSelection(
          title: text("device"),
          options: [
            InstallerOption(
              id: "", label: text(model.devices.isEmpty ? "noDevices" : "selectDevice"))
          ]
            + model.devices.map { InstallerOption(id: $0.id, label: $0.name + " - " + $0.os) },
          selection: $model.selectedDevice, identifier: "installer.device"
        ).disabled(model.busy || model.devices.isEmpty)
        Button(text("refresh"), systemImage: "arrow.clockwise") { model.refresh() }
          .controlSize(.large).frame(minHeight: 44).disabled(model.busy)
          .accessibilityIdentifier("installer.refresh")
      }
      helpText("deviceHelp")
    }
  }

  private var signingSection: some View {
    VStack(alignment: .leading, spacing: 12) {
      if !model.teams.isEmpty {
        InstallerSelection(
          title: text("savedTeam"),
          options: [
            InstallerOption(
              id: model.teams.contains(where: { $0.id == model.team }) ? "" : model.team,
              label: text("manualTeam")
            )
          ] + model.teams.map { InstallerOption(id: $0.id, label: $0.name + " - " + $0.id) },
          selection: $model.team, identifier: "installer.savedTeam"
        )
      }
      HStack(alignment: .top, spacing: 14) {
        InstallerField(
          title: text("team"), value: $model.team, prompt: text("teamPlaceholder"),
          problem: model.team.isEmpty || model.validTeam ? nil : text("invalidTeam")
        ).frame(width: 180).accessibilityIdentifier("installer.team")
        InstallerField(
          title: text("bundle"), value: $model.bundleID,
          problem: model.bundleID.isEmpty || model.validBundle ? nil : text("invalidBundle")
        ).accessibilityIdentifier("installer.bundle")
      }
      helpText("signingHelp")
      helpText("personalTeam")
      HStack(spacing: 16) {
        Button(text("openXcode")) {
          if let url = NSWorkspace.shared.urlForApplication(
            withBundleIdentifier: "com.apple.dt.Xcode")
          {
            NSWorkspace.shared.openApplication(
              at: url, configuration: NSWorkspace.OpenConfiguration())
          }
        }.controlSize(.large)
        Link(
          text("accountHelp"),
          destination: URL(
            string: "https://developer.apple.com/help/account/basics/about-your-developer-account"
          )!)
      }
    }.disabled(model.busy)
  }

  private var activityLog: some View {
    VStack(alignment: .leading, spacing: 0) {
      Button {
        showLog.toggle()
      } label: {
        HStack {
          Label(text("logs"), systemImage: "text.alignleft")
          Spacer()
          Image(systemName: showLog ? "chevron.up" : "chevron.down")
        }
        .font(.subheadline.weight(.medium)).padding(.horizontal, 16)
        .frame(maxWidth: .infinity, minHeight: 48).contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .accessibilityIdentifier("installer.logDisclosure")
      .accessibilityValue(text(showLog ? "expanded" : "collapsed"))
      if showLog {
        Divider()
        VStack(alignment: .leading, spacing: 12) {
          HStack {
            Button(text("openLog")) {
              if let file = model.logFile { NSWorkspace.shared.open(file) }
            }.disabled(model.logFile == nil).accessibilityIdentifier("installer.openLog")
            Spacer()
            Toggle(text("followLog"), isOn: $followLog).toggleStyle(.checkbox)
          }
          ScrollViewReader { reader in
            ScrollView([.horizontal, .vertical]) {
              VStack(alignment: .leading, spacing: 0) {
                Text(model.log.isEmpty ? text("emptyLog") : model.log)
                  .font(.system(.caption, design: .monospaced)).textSelection(.enabled)
                Color.clear.frame(height: 1).id("logEnd")
              }.frame(maxWidth: .infinity, alignment: .leading)
            }.frame(height: 180)
              .onAppear { if followLog { reader.scrollTo("logEnd", anchor: .bottom) } }
              .onChange(of: model.log) {
                if followLog { reader.scrollTo("logEnd", anchor: .bottom) }
              }
              .onChange(of: followLog) {
                if followLog { reader.scrollTo("logEnd", anchor: .bottom) }
              }
          }
        }.padding(16)
      }
    }
    .background(.background, in: RoundedRectangle(cornerRadius: 12))
    .overlay { RoundedRectangle(cornerRadius: 12).strokeBorder(.primary.opacity(0.08)) }
  }

  private var footer: some View {
    HStack(spacing: 16) {
      if model.busy {
        ProgressView().controlSize(.small)
      } else if model.error != nil {
        Image(systemName: "exclamationmark.circle.fill").foregroundStyle(.red)
      } else if model.canInstall || model.installed {
        Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
      }
      Text(model.status).font(.callout)
        .foregroundStyle(model.error == nil ? Color.primary : Color.red)
        .textSelection(.enabled).accessibilityIdentifier("installer.status")
        .fixedSize(horizontal: false, vertical: true)
      Spacer(minLength: 8)
      if model.busy {
        Button(text("cancel")) { model.cancel() }
          .controlSize(.large).disabled(model.cancelling)
          .accessibilityIdentifier("installer.cancel")
      } else {
        Button(text("exportIPA")) { chooseExportDestination() }
          .controlSize(.large).disabled(!model.canInstall)
          .help(text("exportHelp"))
          .accessibilityIdentifier("installer.exportIPA")
      }
      Button(text("install")) { model.install() }
        .buttonStyle(.glassProminent).controlSize(.large)
        .keyboardShortcut(.defaultAction).disabled(!model.canInstall)
        .accessibilityIdentifier("installer.install")
    }.padding(20).frame(minHeight: 76)
  }

  private func helpText(_ key: String) -> some View {
    Text(text(key)).font(.caption).foregroundStyle(.secondary)
      .fixedSize(horizontal: false, vertical: true)
  }

  private func chooseExportDestination() {
    let panel = NSSavePanel()
    panel.title = text("exportIPA")
    panel.message = text("exportHelp")
    panel.nameFieldStringValue = "MinionRush.ipa"
    panel.allowedContentTypes = [UTType(filenameExtension: "ipa") ?? .zip]
    panel.canCreateDirectories = true
    panel.begin { response in
      if response == .OK, let url = panel.url { model.exportIPA(to: url) }
    }
  }
}
