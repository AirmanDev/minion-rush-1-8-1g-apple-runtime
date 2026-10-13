import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct InstallerView: View {
  @Bindable var model: InstallerModel
  @State private var choosingArchive = false
  @State private var showLog = false
  @State private var followLog = true
  @State private var showSigned = true
  @State private var confirmReplace = false
  @State private var dropTarget = false

  private func text(_ key: String) -> String { model.text(key) }

  var body: some View {
    VStack(spacing: 0) {
      header
      Rectangle().fill(BuilderPalette.gold).frame(height: 4)
      ScrollViewReader { reader in
        ScrollView {
          VStack(alignment: .leading, spacing: 18) {
            buildCard
            actionRow
            statusCard
            activityLog.id("activityLog")
            signedSection
          }
          .padding(28)
        }
        .background(BuilderPalette.background)
        .onChange(of: showLog) {
          if showLog { reader.scrollTo("activityLog", anchor: .bottom) }
        }
      }
    }
    .frame(minWidth: model.layout.minimumWidth, minHeight: 650)
    .onChange(of: model.operation) {
      if let operation = model.operation, operation != .refresh { showLog = true }
    }
    .onChange(of: model.error) { if model.error != nil { showLog = true } }
    .fileImporter(isPresented: $choosingArchive, allowedContentTypes: [.zip]) { result in
      switch result {
      case .success(let url): model.selectArchive(url)
      case .failure(let error): model.error = error.localizedDescription
      }
    }
    .alert(text("replaceTitle"), isPresented: $confirmReplace) {
      Button(text("keepFile"), role: .cancel) {}
      Button(text("replace"), role: .destructive) { model.buildNative(overwrite: true) }
    } message: {
      Text(text("replaceHelp"))
    }
  }

  private var header: some View {
    HStack(alignment: .center, spacing: 20) {
      VStack(alignment: .leading, spacing: 7) {
        HStack(alignment: .firstTextBaseline, spacing: 12) {
          Text(model.layout.title)
            .font(.system(size: 30, weight: .semibold))
            .foregroundStyle(.white)
          if !model.appVersion.isEmpty {
            Text("v" + model.appVersion)
              .font(.system(size: 12, weight: .medium))
              .foregroundStyle(Color.white.opacity(0.75))
              .accessibilityIdentifier("installer.appVersion")
          }
        }
        Text(text("headerSubtitle"))
          .font(.system(size: 13))
          .foregroundStyle(Color.white.opacity(0.8))
      }
      Spacer(minLength: 16)
      HStack(spacing: 8) {
        Button(text("languageEnglish")) { model.interfaceLanguage = "en" }
          .buttonStyle(BuilderButtonStyle(role: .language(model.interfaceLanguage == "en")))
        Button(text("languageHungarian")) { model.interfaceLanguage = "hu" }
          .buttonStyle(BuilderButtonStyle(role: .language(model.interfaceLanguage == "hu")))
      }
    }
    .padding(.horizontal, 28)
    .padding(.vertical, 20)
    .background(BuilderPalette.navy)
  }

  private var buildCard: some View {
    BuilderPanel {
      VStack(alignment: .leading, spacing: 18) {
        BuilderInputRow(
          title: text("sourceLabel"),
          hint: text("sourceHint"),
          value: model.archiveURL?.path ?? text("noSource"),
          button: text("choose"),
          identifier: "installer.chooseArchive"
        ) {
          choosingArchive = true
        }
        .disabled(model.busy)
        .dropDestination(for: URL.self) { urls, _ in
          guard !model.busy, urls.count == 1,
            let file = urls.first, file.isFileURL,
            file.pathExtension.lowercased() == "zip"
          else {
            model.error = text("invalidDrop")
            return false
          }
          model.selectArchive(file)
          return true
        } isTargeted: {
          dropTarget = $0
        }
        .background(dropTarget ? BuilderPalette.gold.opacity(0.15) : Color.clear)
        if let summary = model.assets {
          Text(
            "\(text("assetsReady")) - "
              + ByteCountFormatter.string(fromByteCount: summary.bytes, countStyle: .file)
          )
          .font(.caption).foregroundStyle(BuilderPalette.success)
        }

        BuilderInputRow(
          title: text("outputLabel"),
          value: model.outputURL.path,
          button: text("saveAs"),
          identifier: "installer.saveAs"
        ) {
          chooseOutput()
        }.disabled(model.busy)

        VStack(alignment: .leading, spacing: 9) {
          HStack {
            Text(text("languageTitle")).font(.system(size: 15, weight: .semibold))
            Spacer()
            Text(text("languageHint")).font(.caption).foregroundStyle(BuilderPalette.muted)
          }.foregroundStyle(BuilderPalette.text)
          Button {
            model.includeAdditionalLanguages.toggle()
          } label: {
            HStack(spacing: 12) {
              RoundedRectangle(cornerRadius: 5)
                .fill(
                  model.includeAdditionalLanguages && model.hasAdditionalLanguages
                    ? BuilderPalette.success : BuilderPalette.card
                )
                .frame(width: 23, height: 23)
                .overlay {
                  RoundedRectangle(cornerRadius: 5)
                    .strokeBorder(
                      model.includeAdditionalLanguages && model.hasAdditionalLanguages
                        ? BuilderPalette.success : BuilderPalette.border)
                }
              Text(text("hungarian"))
                .font(.system(size: 14, weight: .semibold))
                .foregroundStyle(BuilderPalette.text)
              Spacer()
            }
            .frame(minHeight: 34)
            .contentShape(Rectangle())
          }
          .buttonStyle(.plain)
          .disabled(model.busy || !model.hasAdditionalLanguages)
          .opacity(model.hasAdditionalLanguages ? 1 : 0.5)
          .accessibilityIdentifier("installer.additionalLanguages")
          .accessibilityValue(
            model.includeAdditionalLanguages && model.hasAdditionalLanguages ? "On" : "Off"
          )
          if !model.nativeKitAvailable {
            Text(text("privateKitNeeded"))
              .font(.caption).foregroundStyle(BuilderPalette.muted)
          }
        }
      }
    }
  }

  private var actionRow: some View {
    HStack(spacing: 12) {
      Button(text("buildIPA")) {
        if FileManager.default.fileExists(atPath: model.outputURL.path) {
          confirmReplace = true
        } else {
          model.buildNative(overwrite: false)
        }
      }
      .buttonStyle(BuilderButtonStyle(role: .primary))
      .disabled(!model.canBuildIPA)
      .accessibilityIdentifier("installer.buildIPA")
      Button(text("cancel")) { model.cancel() }
        .buttonStyle(BuilderButtonStyle(role: .secondary))
        .disabled(!model.busy || model.cancelling)
        .accessibilityIdentifier("installer.cancel")
      Spacer(minLength: 0)
      Button(text("openOutput")) {
        NSWorkspace.shared.open(model.outputURL.deletingLastPathComponent())
      }
      .buttonStyle(BuilderButtonStyle(role: .secondary))
      .disabled(model.unsignedIPA == nil)
    }
  }

  private var statusCard: some View {
    BuilderPanel {
      VStack(alignment: .leading, spacing: 12) {
        HStack(spacing: 12) {
          Circle()
            .fill(
              model.error != nil
                ? BuilderPalette.failure
                : model.unsignedIPA != nil || model.installed || model.exportedIPA != nil
                  ? BuilderPalette.success
                  : model.busy ? BuilderPalette.gold : BuilderPalette.muted
            )
            .frame(width: 13, height: 13)
          Text(model.status)
            .font(.system(size: 16, weight: .semibold))
            .foregroundStyle(BuilderPalette.text)
            .accessibilityIdentifier("installer.status")
            .textSelection(.enabled)
          Spacer(minLength: 8)
          Button(text(showLog ? "hideDetails" : "showDetails")) { showLog.toggle() }
            .buttonStyle(.plain)
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(BuilderPalette.navy)
            .accessibilityIdentifier("installer.logDisclosure")
        }
        ProgressView(value: model.busy ? nil : 1)
          .tint(
            model.error != nil
              ? BuilderPalette.failure
              : model.busy ? BuilderPalette.gold : BuilderPalette.success
          )
          .frame(height: 7)
        if let path = model.unsignedIPA ?? model.exportedIPA {
          Text(path.path).font(.system(size: 12)).foregroundStyle(BuilderPalette.muted)
            .textSelection(.enabled).lineLimit(2).truncationMode(.middle)
        }
      }
    }
  }

  private var activityLog: some View {
    Group {
      if showLog {
        BuilderPanel {
          VStack(alignment: .leading, spacing: 14) {
            HStack {
              Text(text("diagnostics")).font(.headline).foregroundStyle(BuilderPalette.text)
              Spacer()
              Button(text("copyError")) {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(model.error ?? model.status, forType: .string)
              }
              .buttonStyle(BuilderButtonStyle(role: .secondary))
              .disabled(model.error == nil)
              Button(text("copyLog")) { copyLog() }
                .buttonStyle(BuilderButtonStyle(role: .secondary))
              Button(text("saveLog")) { saveLog() }
                .buttonStyle(BuilderButtonStyle(role: .secondary))
              Button(text("openLog")) {
                if let file = model.logFile { NSWorkspace.shared.open(file) }
              }
              .buttonStyle(BuilderButtonStyle(role: .secondary))
              .disabled(model.logFile == nil)
              Toggle(text("followLog"), isOn: $followLog).toggleStyle(.checkbox)
                .foregroundStyle(BuilderPalette.text)
            }
            ScrollViewReader { reader in
              ScrollView([.horizontal, .vertical]) {
                VStack(alignment: .leading, spacing: 0) {
                  Text(model.log.isEmpty ? text("emptyLog") : model.log)
                    .font(.system(size: 12, design: .monospaced))
                    .foregroundStyle(BuilderPalette.text)
                    .textSelection(.enabled)
                  Color.clear.frame(height: 1).id("logEnd")
                }.frame(maxWidth: .infinity, alignment: .leading)
              }
              .frame(height: 195)
              .padding(12)
              .background(BuilderPalette.field, in: RoundedRectangle(cornerRadius: 9))
              .onAppear { if followLog { reader.scrollTo("logEnd", anchor: .bottom) } }
              .onChange(of: model.log) {
                if followLog { reader.scrollTo("logEnd", anchor: .bottom) }
              }
            }
          }
        }
      }
    }
  }

  private var signedSection: some View {
    VStack(alignment: .leading, spacing: 12) {
      Button {
        showSigned.toggle()
      } label: {
        HStack {
          Text(text("signedSection")).font(.headline)
          Spacer()
          Image(systemName: showSigned ? "chevron.up" : "chevron.down")
        }
        .foregroundStyle(BuilderPalette.text)
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      Text(text("signedHelp")).font(.caption).foregroundStyle(BuilderPalette.muted)
      if showSigned {
        if model.assets == nil {
          Button(text("importForSigning")) { model.importArchiveForSigning() }
            .buttonStyle(BuilderButtonStyle(role: .secondary))
            .disabled(model.busy || model.archiveURL == nil)
            .accessibilityIdentifier("installer.importForSigning")
        }
        InstallerCard(title: text("device")) { deviceSection }
        InstallerCard(title: text("signedSection")) { signingSection }
        HStack(spacing: 12) {
          Button(text("exportIPA")) { chooseSignedExport() }
            .buttonStyle(BuilderButtonStyle(role: .secondary))
            .disabled(!model.canInstall)
            .accessibilityIdentifier("installer.exportIPA")
          Button(text("install")) { model.install() }
            .buttonStyle(BuilderButtonStyle(role: .primary))
            .disabled(!model.canInstall)
            .accessibilityIdentifier("installer.install")
          Spacer()
          Button(text("workspace")) { NSWorkspace.shared.open(model.workspace) }
            .buttonStyle(BuilderButtonStyle(role: .secondary))
        }
      }
    }
  }

  private var deviceSection: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack(alignment: .bottom, spacing: 12) {
        InstallerSelection(
          title: text("device"),
          options: [
            InstallerOption(
              id: "",
              label: text(model.devices.isEmpty ? "noDevices" : "selectDevice"))
          ]
            + model.devices.map { InstallerOption(id: $0.id, label: $0.name + " - " + $0.os) },
          selection: $model.selectedDevice, identifier: "installer.device"
        )
        .disabled(model.busy || model.devices.isEmpty)
        Button(text("refresh")) { model.refreshDevices() }
          .buttonStyle(BuilderButtonStyle(role: .secondary))
          .disabled(model.busy)
      }
      if !model.devices.isEmpty {
        Text("\(model.devices.count) " + text("devicesFound"))
          .font(.caption).foregroundStyle(BuilderPalette.muted)
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
      HStack(spacing: 14) {
        InstallerField(
          title: text("team"), value: $model.team,
          prompt: text("teamPlaceholder"),
          problem: model.team.isEmpty || model.validTeam ? nil : text("invalidTeam")
        )
        .frame(width: 180)
        InstallerField(
          title: text("bundle"), value: $model.bundleID,
          problem: model.bundleID.isEmpty || model.validBundle
            ? nil : text("invalidBundle"))
      }
      helpText("signingHelp")
      helpText("personalTeam")
      HStack(spacing: 12) {
        Button(text("openXcode")) {
          if let url = NSWorkspace.shared.urlForApplication(
            withBundleIdentifier: "com.apple.dt.Xcode")
          {
            NSWorkspace.shared.openApplication(
              at: url, configuration: NSWorkspace.OpenConfiguration())
          }
        }.buttonStyle(BuilderButtonStyle(role: .secondary))
        Link(
          text("accountHelp"),
          destination: URL(
            string:
              "https://developer.apple.com/help/account/basics/about-your-developer-account")!)
      }
    }.disabled(model.busy)
  }

  private func helpText(_ key: String) -> some View {
    Text(text(key)).font(.caption).foregroundStyle(BuilderPalette.muted)
      .fixedSize(horizontal: false, vertical: true)
  }

  private func chooseOutput() {
    let panel = NSSavePanel()
    panel.title = text("saveAs")
    panel.nameFieldStringValue = model.outputURL.lastPathComponent
    panel.directoryURL = model.outputURL.deletingLastPathComponent()
    panel.allowedContentTypes = [UTType(filenameExtension: "ipa") ?? .zip]
    panel.canCreateDirectories = true
    panel.begin { response in
      if response == .OK, let url = panel.url { model.outputURL = url }
    }
  }

  private func chooseSignedExport() {
    let panel = NSSavePanel()
    panel.title = text("exportIPA")
    panel.message = text("exportHelp")
    panel.nameFieldStringValue = "MinionRush-signed.ipa"
    panel.allowedContentTypes = [UTType(filenameExtension: "ipa") ?? .zip]
    panel.canCreateDirectories = true
    panel.begin { response in
      if response == .OK, let url = panel.url { model.exportIPA(to: url) }
    }
  }

  private func fullLog() -> String {
    model.logFile.flatMap { try? String(contentsOf: $0, encoding: .utf8) } ?? model.log
  }

  private func copyLog() {
    NSPasteboard.general.clearContents()
    NSPasteboard.general.setString(fullLog(), forType: .string)
  }

  private func saveLog() {
    let panel = NSSavePanel()
    panel.title = text("saveLog")
    panel.nameFieldStringValue = "MinionRush-build-log.txt"
    panel.allowedContentTypes = [.plainText]
    panel.canCreateDirectories = true
    panel.begin { result in
      guard result == .OK, let url = panel.url else { return }
      do {
        try fullLog().write(to: url, atomically: true, encoding: .utf8)
      } catch {
        model.error = error.localizedDescription
      }
    }
  }
}
