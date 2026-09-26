import Darwin
import Foundation

struct Device: Decodable, Identifiable, Sendable {
  let id: String
  let udid: String
  let name: String
  let os: String
}

struct AssetSummary: Decodable, Sendable {
  let files: Int
  let bytes: Int64
}

struct SigningTeam: Decodable, Identifiable, Sendable {
  let id: String
  let name: String
}

struct BackendEvent: Decodable, Sendable {
  let `protocol`: Int
  let type: String
  let message: String?
  let assets: AssetSummary?
  let devices: [Device]?
  let teams: [SigningTeam]?
  let runtime: String?
  let toolchain: String?
  let toolchainError: String?
  let assetError: String?
  let installed: Bool?
  let logPath: String?
}

// Process lifetime is shared with Cancel; all mutable state is protected by the lock.
final class BackendProcess: @unchecked Sendable {
  private let lock = NSLock()
  private var process: Process?
  private var cancelled = false

  func cancel() {
    lock.lock()
    cancelled = true
    let running = process
    lock.unlock()
    if let running, running.isRunning { running.terminate() }
  }

  func run(
    source: URL, workspace: URL, arguments: [String],
    event: @escaping @Sendable (BackendEvent) -> Void
  ) throws -> Int32 {
    let task = Process()
    let pipe = Pipe()
    let developer =
      ProcessInfo.processInfo.environment["DEVELOPER_DIR"]
      ?? "/Applications/Xcode.app/Contents/Developer"
    let python = URL(fileURLWithPath: developer).appendingPathComponent("usr/bin/python3")
    task.executableURL =
      FileManager.default.isExecutableFile(atPath: python.path)
      ? python : URL(fileURLWithPath: "/usr/bin/python3")
    // Isolated Python needs an explicit public tools path for local imports.
    task.arguments =
      [
        "-I", "-B", "-u", "-c",
        "import runpy,sys;sys.path.insert(0,sys.argv[1]);sys.argv=sys.argv[2:];runpy.run_path(sys.argv[0],run_name='__main__')",
        source.appendingPathComponent("tools").path,
        source.appendingPathComponent("tools/installer_backend.py").path,
        "--source", source.path, "--workspace", workspace.path,
      ] + arguments
    var environment = ProcessInfo.processInfo.environment
    if FileManager.default.fileExists(atPath: developer + "/Platforms/iPhoneOS.platform") {
      environment["DEVELOPER_DIR"] = developer
    }
    task.environment = environment
    task.standardInput = FileHandle.nullDevice
    task.standardOutput = pipe
    task.standardError = pipe
    lock.lock()
    if cancelled {
      lock.unlock()
      return 130
    }
    do {
      try task.run()
      process = task
    } catch {
      lock.unlock()
      throw error
    }
    lock.unlock()
    defer {
      try? pipe.fileHandleForReading.close()
      lock.lock()
      process = nil
      lock.unlock()
    }
    var pending = Data()
    func consume(_ line: Data) throws {
      if line.isEmpty { return }
      let item: BackendEvent
      do {
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        item = try decoder.decode(BackendEvent.self, from: line)
      } catch {
        throw NSError(
          domain: "InstallerBackend", code: 1,
          userInfo: [NSLocalizedDescriptionKey: String(decoding: line, as: UTF8.self)]
        )
      }
      guard item.protocol == 1 else {
        throw CocoaError(.fileReadCorruptFile)
      }
      event(item)
    }
    do {
      var buffer = [UInt8](repeating: 0, count: 16384)
      while true {
        // A pipe read returns available bytes without waiting for the buffer to fill.
        let count = Darwin.read(pipe.fileHandleForReading.fileDescriptor, &buffer, buffer.count)
        if count < 0 {
          if errno == EINTR { continue }
          throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
        if count == 0 { break }
        pending.append(contentsOf: buffer.prefix(count))
        while let newline = pending.firstIndex(of: 10) {
          try consume(Data(pending[..<newline]))
          pending.removeSubrange(...newline)
        }
        guard pending.count < 1024 * 1024 else {
          throw CocoaError(.fileReadTooLarge)
        }
      }
      try consume(pending)
    } catch {
      if task.isRunning { task.terminate() }
      task.waitUntilExit()
      throw error
    }
    task.waitUntilExit()
    return task.terminationStatus
  }
}
