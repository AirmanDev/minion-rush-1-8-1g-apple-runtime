import AppKit
import Observation

struct InstallerLayout: Decodable {
  struct SigningRules: Decodable {
    let team: String
    let bundle: String
  }
  struct Section: Decodable, Identifiable {
    let id: String
    let title: String
  }
  let title: String
  let subtitle: String
  let minimumWidth: Double
  let signingRules: SigningRules
  let sections: [Section]
  let labels: [String: String]

  func text(_ key: String) -> String { labels[key] ?? key }
}

@MainActor @Observable
final class InstallerModel {
  enum Operation {
    case refresh, importArchive, install, exportIPA
  }
  let layout: InstallerLayout
  let workspace: URL
  private let source: URL
  private let defaults: UserDefaults
  private var worker: BackendProcess?
  private var job = UUID()
  private var receivedResult = false
  private var quitAfterCancellation = false
  private var pendingArchiveName: String?
  var operation: Operation?
  var archiveName: String?
  var busy = false
  var cancelling = false
  var phase = ""
  var error: String?
  var log = ""
  var logFile: URL?
  var assets: AssetSummary?
  var devices: [Device] = []
  var teams: [SigningTeam] = []
  var selectedDevice = ""
  var toolchain = ""
  var runtime: URL?
  var installed = false
  var exportedIPA: URL?
  var team: String {
    didSet { defaults.set(team, forKey: "developmentTeam") }
  }
  var bundleID: String {
    didSet { defaults.set(bundleID, forKey: "gameBundleIdentifier") }
  }

  init(resources: URL, workspace: URL? = nil, defaults: UserDefaults = .standard) throws {
    self.defaults = defaults
    archiveName = defaults.string(forKey: "releaseArchiveName")
    source = resources.appendingPathComponent("Runtime")
    layout = try JSONDecoder().decode(
      InstallerLayout.self,
      from: Data(contentsOf: resources.appendingPathComponent("installer_ui.json"))
    )
    self.workspace =
      try workspace
      ?? FileManager.default.url(
        for: .applicationSupportDirectory, in: .userDomainMask,
        appropriateFor: nil, create: true
      ).appendingPathComponent("MinionRushInstaller")
    team = defaults.string(forKey: "developmentTeam") ?? ""
    bundleID =
      defaults.string(forKey: "gameBundleIdentifier")
      ?? "org.minionrush.runtime.u" + UUID().uuidString.prefix(8).lowercased()
    defaults.set(bundleID, forKey: "gameBundleIdentifier")
  }

  var validTeam: Bool { matches(team, rule: layout.signingRules.team) }
  var validBundle: Bool { matches(bundleID, rule: layout.signingRules.bundle) }
  var validSigning: Bool { validTeam && validBundle }
  var selectedDeviceInfo: Device? { devices.first { $0.id == selectedDevice } }

  private func matches(_ value: String, rule: String) -> Bool {
    !value.isEmpty
      && value.range(of: rule, options: .regularExpression)
        == value.startIndex..<value.endIndex
  }

  var canInstall: Bool {
    !busy && assets != nil && !toolchain.isEmpty && validSigning
      && devices.contains { $0.id == selectedDevice }
  }

  var status: String {
    if busy { return phase }
    if let error { return error }
    if installed { return layout.text("installed") }
    if exportedIPA != nil { return layout.text("ipaExported") }
    if assets == nil { return layout.text("importNeeded") }
    if toolchain.isEmpty { return layout.text("toolchainNeeded") }
    if devices.isEmpty || selectedDevice.isEmpty { return layout.text("deviceNeeded") }
    if !validSigning { return layout.text("signingNeeded") }
    return layout.text("ready")
  }

  func refresh() { start(["status"], phase: layout.text("checking"), operation: .refresh) }

  func importArchive(_ url: URL) {
    guard !busy, url.isFileURL, url.pathExtension.lowercased() == "zip" else { return }
    pendingArchiveName = url.lastPathComponent
    start(["import", url.path], phase: layout.text("working"), operation: .importArchive)
  }

  func install() {
    guard canInstall else { return }
    start(
      ["install", "--device", selectedDevice, "--team", team, "--bundle", bundleID],
      phase: layout.text("working"), operation: .install
    )
  }

  func exportIPA(to destination: URL) {
    guard canInstall, destination.isFileURL else { return }
    start(
      [
        "export", "--device", selectedDevice, "--team", team, "--bundle", bundleID,
        "--destination", destination.path,
      ],
      phase: layout.text("working"), operation: .exportIPA
    )
  }

  func cancel(quit: Bool = false) {
    guard busy else { return }
    quitAfterCancellation = quitAfterCancellation || quit
    guard !cancelling else { return }
    cancelling = true
    phase = layout.text("cancelling")
    worker?.cancel()
  }

  private func appendLog(_ message: String) {
    log.append(message + "\n")
    if log.count > 128 * 1024 { log = String(log.suffix(96 * 1024)) }
  }

  private func receive(_ item: BackendEvent, token: UUID) {
    guard job == token else { return }
    apply(item)
  }

  func apply(_ item: BackendEvent) {
    switch item.type {
    case "session":
      if let path = item.logPath { logFile = URL(fileURLWithPath: path) }
    case "log": appendLog(item.message ?? "")
    case "progress":
      if !cancelling { phase = item.message ?? layout.text("working") }
    case "error":
      let problem = item.message ?? "Backend operation failed."
      error = problem
      appendLog(problem)
    case "cancelled":
      phase = layout.text("cancelled")
      appendLog(item.message ?? phase)
    case "result":
      receivedResult = true
      if let summary = item.assets { assets = summary }
      if let problem = item.assetError {
        assets = nil
        appendLog(problem)
      }
      if let items = item.devices {
        devices = items
        if !items.contains(where: { $0.id == selectedDevice }) {
          selectedDevice = items.count == 1 ? items[0].id : ""
        }
      }
      if let items = item.teams {
        teams = items
        if team.isEmpty, items.count == 1 { team = items[0].id }
      }
      if let path = item.runtime { runtime = URL(fileURLWithPath: path) }
      if let chain = item.toolchain { toolchain = chain }
      if let problem = item.toolchainError {
        toolchain = ""
        error = problem
        appendLog(problem)
      }
      installed = item.installed == true
      if let path = item.ipaPath { exportedIPA = URL(fileURLWithPath: path) }
    default: error = "Unsupported installer response."
    }
  }

  private func start(_ arguments: [String], phase: String, operation: Operation) {
    guard !busy else { return }
    busy = true
    cancelling = false
    error = nil
    installed = false
    exportedIPA = nil
    receivedResult = false
    self.phase = phase
    self.operation = operation
    logFile = nil
    appendLog("\n" + phase)
    let token = UUID()
    job = token
    let process = BackendProcess()
    worker = process
    let source = source
    let workspace = workspace
    DispatchQueue.global(qos: .userInitiated).async {
      var result: Int32 = -1
      var problem: String?
      do {
        result = try process.run(source: source, workspace: workspace, arguments: arguments) {
          item in
          DispatchQueue.main.async { self.receive(item, token: token) }
        }
      } catch {
        problem = error.localizedDescription
      }
      let exitStatus = result
      let failure = problem
      DispatchQueue.main.async {
        guard self.job == token else { return }
        let wasCancelled = self.cancelling || exitStatus == 130
        if wasCancelled {
          self.error = self.layout.text("cancelled")
        } else if exitStatus != 0 || !self.receivedResult {
          self.error = self.error ?? failure ?? "Installer backend exited without a result."
        }
        if self.error == nil, operation == .importArchive, self.assets != nil {
          self.archiveName = self.pendingArchiveName
          self.defaults.set(self.archiveName, forKey: "releaseArchiveName")
        }
        self.pendingArchiveName = nil
        self.busy = false
        self.cancelling = false
        self.worker = nil
        self.operation = nil
        if self.quitAfterCancellation { NSApp.reply(toApplicationShouldTerminate: true) }
      }
    }
  }
}
