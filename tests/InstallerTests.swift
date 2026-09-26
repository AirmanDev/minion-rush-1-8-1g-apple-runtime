import AppKit

final class EventRecorder: @unchecked Sendable {
  private let lock = NSLock()
  private var values: [BackendEvent] = []

  func append(_ event: BackendEvent) {
    lock.lock()
    defer { lock.unlock() }
    values.append(event)
  }

  var events: [BackendEvent] {
    lock.lock()
    defer { lock.unlock() }
    return values
  }
}

@main
struct InstallerTests {
  static func require(_ condition: @autoclosure () -> Bool, _ message: String) throws {
    if !condition() {
      throw NSError(
        domain: "InstallerTests", code: 1, userInfo: [NSLocalizedDescriptionKey: message])
    }
  }

  @MainActor
  static func main() throws {
    guard CommandLine.arguments.count == 2 else {
      fatalError("Usage: installer-tests <app-resources>")
    }
    let resources = URL(fileURLWithPath: CommandLine.arguments[1])
    let temporary = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    let tools = temporary.appendingPathComponent("tools")
    try FileManager.default.createDirectory(at: tools, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: temporary) }
    let backend = tools.appendingPathComponent("installer_backend.py")
    let response = """
      import json,sys
      print(json.dumps({'protocol':1,'type':'log','message':sys.argv[-1]}),flush=True)
      print(json.dumps({'protocol':1,'type':'result','assets':{'files':1,'bytes':4}}),flush=True)
      """
    try response.write(to: backend, atomically: true, encoding: .utf8)
    let recorder = EventRecorder()
    let runner = BackendProcess()
    let status = try runner.run(
      source: temporary, workspace: temporary, arguments: ["path with spaces; not a command"],
      event: { recorder.append($0) }
    )
    try require(status == 0, "Backend exit status")
    try require(recorder.events.count == 2, "JSON Lines event count")
    try require(
      recorder.events[0].message == "path with spaces; not a command", "Argument preservation")
    try require(recorder.events[1].assets?.bytes == 4, "Asset summary decoding")

    let acknowledgement = temporary.appendingPathComponent("received-event")
    let streaming = """
      import json,pathlib,sys,time
      print(json.dumps({'protocol':1,'type':'progress','message':'Building'}),flush=True)
      acknowledgement=pathlib.Path(sys.argv[-1])
      deadline=time.monotonic()+3
      while not acknowledgement.exists():
          if time.monotonic()>=deadline:
              sys.exit(1)
          time.sleep(0.01)
      print(json.dumps({'protocol':1,'type':'result','installed':True}),flush=True)
      """
    try streaming.write(to: backend, atomically: true, encoding: .utf8)
    let streamed = EventRecorder()
    let streamingStatus = try BackendProcess().run(
      source: temporary, workspace: temporary, arguments: [acknowledgement.path],
      event: {
        streamed.append($0)
        if $0.type == "progress" {
          _ = FileManager.default.createFile(atPath: acknowledgement.path, contents: Data())
        }
      }
    )
    try require(streamingStatus == 0, "Live progress must arrive before the worker exits")
    try require(streamed.events.last?.installed == true, "Streaming result")

    let fragmented = """
      import json,sys,time
      message=json.dumps({'protocol':1,'type':'log','message':'\\u0151\\u0171'*20000},ensure_ascii=False).encode()
      sys.stdout.buffer.write(message[:55])
      sys.stdout.buffer.flush()
      time.sleep(0.02)
      sys.stdout.buffer.write(message[55:]+b'\\n')
      sys.stdout.buffer.flush()
      """
    try fragmented.write(to: backend, atomically: true, encoding: .utf8)
    let fragmentedEvents = EventRecorder()
    let fragmentedStatus = try BackendProcess().run(
      source: temporary, workspace: temporary, arguments: [],
      event: { fragmentedEvents.append($0) })
    try require(fragmentedStatus == 0, "Fragmented process output")
    try require(
      fragmentedEvents.events.first?.message == String(repeating: "\u{0151}\u{0171}", count: 20000),
      "Long UTF-8 messages must survive partial reads")

    let cancellation = """
      import json,signal,sys
      def stop(*args):
          print(json.dumps({'protocol':1,'type':'cancelled'}),flush=True)
          sys.exit(130)
      signal.signal(signal.SIGTERM,stop)
      signal.pause()
      """
    try cancellation.write(to: backend, atomically: true, encoding: .utf8)
    let cancellable = BackendProcess()
    let cancelledEvents = EventRecorder()
    DispatchQueue.global().asyncAfter(deadline: .now() + 0.5) { cancellable.cancel() }
    let cancelled = try cancellable.run(
      source: temporary, workspace: temporary, arguments: [],
      event: { cancelledEvents.append($0) }
    )
    try require(cancelled == 130, "Cancellation exit status")
    try require(cancelledEvents.events.last?.type == "cancelled", "Cancellation result")
    let cancelledBeforeLaunch = BackendProcess()
    cancelledBeforeLaunch.cancel()
    let notLaunched = try cancelledBeforeLaunch.run(
      source: temporary, workspace: temporary, arguments: [], event: { _ in })
    try require(notLaunched == 130, "Cancellation before launch")

    try "print('Invalid backend output')".write(to: backend, atomically: true, encoding: .utf8)
    do {
      _ = try BackendProcess().run(
        source: temporary, workspace: temporary, arguments: [], event: { _ in })
      throw NSError(domain: "InstallerTests", code: 2)
    } catch {
      try require(
        error.localizedDescription.contains("Invalid backend output"), "Readable process errors")
    }

    let suite = "InstallerTests." + UUID().uuidString
    let defaults = UserDefaults(suiteName: suite)!
    defer { defaults.removePersistentDomain(forName: suite) }
    let model = try InstallerModel(resources: resources, workspace: temporary, defaults: defaults)
    try require(!model.canInstall, "Initial Install must be disabled")
    model.assets = AssetSummary(files: 1, bytes: 4)
    model.devices = [Device(id: "selected", udid: "sample", name: "Test iPhone", os: "27.0")]
    model.selectedDevice = "selected"
    model.toolchain = "Xcode 27"
    model.team = "ABCDEFGHIJ"
    try require(model.canInstall, "Complete prerequisites must enable Install")
    model.team = "ABCDEFGHIJ\n"
    try require(!model.canInstall, "Trailing Team ID newline must be rejected")
    model.team = "ABCDEFGHIJ"
    model.bundleID = "org.example.valid\n"
    try require(!model.canInstall, "Trailing bundle newline must be rejected")
    model.bundleID = "org.example;invalid"
    try require(!model.canInstall, "Invalid signing details must disable Install")
    model.bundleID = "org.example.valid"
    model.selectedDevice = "disconnected"
    try require(!model.canInstall, "Missing selected device must disable Install")
    model.selectedDevice = "selected"
    model.toolchain = ""
    try require(!model.canInstall, "Missing toolchain must disable Install")
    try require(model.status == model.layout.text("toolchainNeeded"), "Missing toolchain guidance")
    model.toolchain = "Xcode 27"
    model.busy = true
    try require(!model.canInstall, "Busy state must disable Install")
    print("INSTALLER: native process and model tests passed")
  }
}
