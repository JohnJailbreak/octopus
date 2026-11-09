//
//  Diagnostics.swift
//  Octopus
//
//  Settings > Run Full Diagnostics: every non-destructive check in one run, then one ZIP to send. The storage,
//  responder and transport-adapter checks are the core's (Octopus/Core/oct_diag.c); this file adds the app's own,
//  the read-only device observations, and the checks that need a computer, which are reported as BLOCKED with the
//  reason rather than simulated. Simulated communication is always labelled as such.
//

import Foundation
import CryptoKit
import OctopusCore
import OctopusSpawn

enum DiagnosticStatus: String, Codable, Sendable, CaseIterable {
    case pass = "PASS", fail = "FAIL", skipped = "SKIPPED", blocked = "BLOCKED"

    init(_ raw: oct_diag_status) {
        switch raw {
        case OCT_DIAG_PASS: self = .pass
        case OCT_DIAG_SKIPPED: self = .skipped
        case OCT_DIAG_BLOCKED: self = .blocked
        default: self = .fail
        }
    }
}

enum DiagnosticKind: String, Codable, Sendable {
    case local, simulated, external

    init(_ raw: oct_diag_kind) {
        switch raw {
        case OCT_DIAG_SIMULATED: self = .simulated
        case OCT_DIAG_EXTERNAL: self = .external
        default: self = .local
        }
    }
}

struct DiagnosticResult: Codable, Identifiable, Equatable, Sendable {
    let id: String
    let group: String
    let title: String
    let status: DiagnosticStatus
    let kind: DiagnosticKind
    let checks: UInt32
    let durationMs: Double
    let detail: String
}

/// What the checks may know about the running app, taken on the main thread when the run starts.
struct DiagnosticsEnvironment: Codable, Sendable {
    var serviceState: String
    var transportState: String
    var transportFailure: String
    var transportTransactions: UInt64
    var bridgeActive: Bool
    var bridgeSocketPath: String
    var bridgeClientConnected: Bool
    var settings: [String: String]

    @MainActor
    init(service: ServiceController) {
        serviceState = service.state.title
        transportState = ConnectionStage(transport: service.status.transportState).title
        transportFailure = service.status.transportFailure
        transportTransactions = service.status.transportTransactions
        bridgeActive = service.bridgeActive
        bridgeSocketPath = service.bridgeSocketPath
        bridgeClientConnected = service.bridgeClientConnected
        settings = Diagnostics.settingsSnapshot()
    }

    static let unknown = DiagnosticsEnvironment(serviceState: "unknown", transportState: "unknown",
                                                transportFailure: "", transportTransactions: 0,
                                                bridgeActive: false, bridgeSocketPath: "", bridgeClientConnected: false,
                                                settings: [:])

    init(serviceState: String, transportState: String, transportFailure: String, transportTransactions: UInt64,
         bridgeActive: Bool, bridgeSocketPath: String, bridgeClientConnected: Bool,
         settings: [String: String]) {
        self.serviceState = serviceState
        self.transportState = transportState
        self.transportFailure = transportFailure
        self.transportTransactions = transportTransactions
        self.bridgeActive = bridgeActive
        self.bridgeSocketPath = bridgeSocketPath
        self.bridgeClientConnected = bridgeClientConnected
        self.settings = settings
    }
}

struct DiagnosticStep: Sendable {
    let id: String
    let group: String
    let title: String
    let kind: DiagnosticKind
    let run: @Sendable () async -> DiagnosticResult
}

/// The core checks, stepped one at a time off the main thread.
final class CoreDiagnostics: @unchecked Sendable {
    private let handle: OpaquePointer?
    private let lock = NSLock()

    init() {
        var created: OpaquePointer? = nil
        oct_diag_create(&created)
        handle = created
    }

    deinit { oct_diag_destroy(handle) }

    var count: Int { Int(oct_diag_count(handle)) }

    func info(_ index: Int) -> oct_diag_result {
        var raw = oct_diag_result()
        oct_diag_info(handle, UInt32(index), &raw)
        return raw
    }

    func run(_ index: Int) -> DiagnosticResult {
        lock.lock()
        defer { lock.unlock() }
        var raw = oct_diag_result()
        oct_diag_run(handle, UInt32(index), &raw)
        return DiagnosticResult(id: Diagnostics.text(raw.id), group: Diagnostics.text(raw.group),
                                title: Diagnostics.text(raw.title), status: DiagnosticStatus(raw.status),
                                kind: DiagnosticKind(raw.kind), checks: raw.checks,
                                durationMs: Double(raw.duration_us) / 1000, detail: Diagnostics.text(raw.detail))
    }
}

@MainActor
final class DiagnosticsRunner: ObservableObject {
    @Published private(set) var results: [DiagnosticResult] = []
    @Published private(set) var planned = 0
    @Published private(set) var current: String? = nil
    @Published private(set) var running = false
    @Published private(set) var archive: URL? = nil
    @Published private(set) var exportProblem: String? = nil
    @Published private(set) var exporting = false
    private(set) var started: Date? = nil
    private(set) var finished: Date? = nil
    private(set) var environment: DiagnosticsEnvironment? = nil

    func count(_ status: DiagnosticStatus) -> Int { results.filter { $0.status == status }.count }

    var summaryLine: String {
        "\(results.count) checks: \(count(.pass)) passed, \(count(.fail)) failed, \(count(.skipped)) skipped, "
            + "\(count(.blocked)) blocked"
    }

    /// Runs every check, then builds the archive.
    func start(environment: DiagnosticsEnvironment) {
        guard !running else { return }
        Task {
            await run(environment: environment)
            await export(environment: environment)
        }
    }

    func run(environment: DiagnosticsEnvironment, includeNetwork: Bool = true) async {
        self.environment = environment
        running = true
        results = []
        archive = nil
        exportProblem = nil
        started = Date()
        finished = nil
        DebugLog.log(.info, "diagnostics", "start", "Run Full Diagnostics")
        let steps = Diagnostics.plan(environment: environment, includeNetwork: includeNetwork)
        planned = steps.count
        for step in steps {
            current = step.title
            let result = await Task.detached(priority: .userInitiated) { await step.run() }.value
            results.append(result)
            let level: DebugLog.Level = result.status == .fail ? .error : result.status == .pass ? .info : .warning
            DebugLog.log(level, "diagnostics", result.id, "\(result.status.rawValue) (\(result.kind.rawValue)): \(result.detail)")
        }
        current = nil
        finished = Date()
        running = false
        DebugLog.log(.info, "diagnostics", "done", summaryLine)
    }

    /// Builds the archive in the first folder that takes it (Diagnostics.exportFolders unless `directories`).
    func export(environment: DiagnosticsEnvironment, directories: [String]? = nil) async {
        exporting = true
        exportProblem = nil
        let results = self.results
        let started = self.started ?? Date()
        let finished = self.finished ?? Date()
        let folders = directories ?? Diagnostics.exportFolders
        do {
            let url = try await Task.detached(priority: .userInitiated) {
                try Diagnostics.makeArchive(results: results, environment: environment, started: started,
                                            finished: finished, directories: folders)
            }.value
            if let previous = archive, previous != url { try? FileManager.default.removeItem(at: previous) }
            archive = url
            DebugLog.log(.info, "diagnostics", "export", "Archive ready: \(Diagnostics.redact(url.path))")
        } catch {
            archive = nil
            exportProblem = "The archive couldn't be made. \(Diagnostics.redact(error.localizedDescription))"
            DebugLog.log(.error, "diagnostics", "export_failed", exportProblem ?? "")
        }
        exporting = false
    }

    /// Export Diagnostics again, after a failure, without running the checks again.
    func retryExport() {
        guard !running, !exporting, !results.isEmpty, let environment else { return }
        Task { await export(environment: environment) }
    }

    /// The results and the failures as plain text, for sharing when no archive can be written.
    var resultsText: String {
        Diagnostics.plainText(results: results, environment: environment ?? .unknown, started: started ?? Date(),
                              finished: finished ?? Date())
    }
}

enum Diagnostics {
    static let deviceFiles: [(event: String, path: String)] = DeviceReport.sources

    // MARK: - The plan

    static func plan(environment: DiagnosticsEnvironment, includeNetwork: Bool) -> [DiagnosticStep] {
        var steps: [DiagnosticStep] = [
            step("app.build", "Application", "Build information", .local) { build() },
            step("app.debug_log", "Application", "Persistent debug log", .local) { debugLog() },
            step("updates.helper", "Updates", "Update helper in the app", .local) { helperPresent() },
            step("updates.helper_root", "Updates", "Update helper starts as root", .local) { helperRuns() },
        ]
        if includeNetwork {
            steps.append(DiagnosticStep(id: "updates.release_check", group: "Updates",
                                        title: "Release list from GitHub", kind: .external,
                                        run: { await releaseCheck() }))
        }
        let core = CoreDiagnostics()
        for index in 0..<core.count {
            let info = core.info(index)
            steps.append(DiagnosticStep(id: text(info.id), group: text(info.group), title: text(info.title),
                                        kind: DiagnosticKind(info.kind), run: { core.run(index) }))
        }
        steps += [
            step("device.usb_configuration", "Device (read-only)", "USB device configurations", .local) {
                plistObservation("device.usb_configuration", "Device (read-only)", "USB device configurations",
                                 path: deviceFiles[0].path, highlight: ["PTP", "MTP"])
            },
            step("device.ptp_device_info", "Device (read-only)", "PTP DeviceInfo that ptpd answers with", .local) {
                plistObservation("device.ptp_device_info", "Device (read-only)", "PTP DeviceInfo that ptpd answers with",
                                 path: deviceFiles[1].path, highlight: ["Manufacturer", "Model", "DeviceVersion"])
            },
            step("device.ptpd_launch", "Device (read-only)", "How launchd starts ptpd", .local) {
                plistObservation("device.ptpd_launch", "Device (read-only)", "How launchd starts ptpd",
                                 path: deviceFiles[2].path,
                                 highlight: ["ProgramArguments", "UserName", "USBDeviceFunction", "IOProviderClass"])
            },
            step("device.ptp_registry", "Device (read-only)", "PTP interface discovery", .local) { ptpRegistry() },
            step("device.ptpd_identity", "Device (read-only)", "ptpd binary identity", .local) { ptpdIdentity() },
            step("device.ptpd_process", "Device (read-only)", "ptpd running now", .local) { ptpdProcess() },
            step("external.usb_transport", "External connection", "USB transport for Octopus", .external) {
                usbTransport()
            },
            step("external.host_requests", "External connection", "Requests from a computer", .external) {
                hostRequests(environment)
            },
            step("external.windows", "External connection", "Windows File Explorer", .external) { windows() },
            step("bridge.app_active", "Bridge", "App bridge socket server", .local) { bridgeAppActive(environment) },
            step("bridge.client_connected", "Bridge", "Integration client connected", .local) { bridgeClientState(environment) },
        ]
        return steps
    }

    private static func step(_ id: String, _ group: String, _ title: String, _ kind: DiagnosticKind,
                             _ check: @escaping @Sendable () -> DiagnosticResult) -> DiagnosticStep {
        DiagnosticStep(id: id, group: group, title: title, kind: kind, run: { check() })
    }

    private static func result(_ id: String, _ group: String, _ title: String, _ kind: DiagnosticKind,
                               _ status: DiagnosticStatus, _ detail: String, checks: UInt32 = 1,
                               started: Date = Date()) -> DiagnosticResult {
        DiagnosticResult(id: id, group: group, title: title, status: status, kind: kind, checks: checks,
                         durationMs: Date().timeIntervalSince(started) * 1000, detail: detail)
    }

    // MARK: - Application

    static func build() -> DiagnosticResult {
        let commit = AppInfo.commit
        return result("app.build", "Application", "Build information", .local, .pass,
                      "Octopus \(AppInfo.version), commit \(commit.isEmpty ? "unknown" : String(commit.prefix(12))), "
                          + "core \(OCT_CORE_VERSION), \(DebugLog.systemDescription)")
    }

    static func debugLog() -> DiagnosticResult {
        let started = Date()
        let marker = "probe-\(UUID().uuidString.prefix(8))"
        DebugLog.log(.info, "diagnostics", "probe", marker)
        DebugLog.flush()
        let stats = DebugLog.stats()
        let make = { (status: DiagnosticStatus, detail: String) in
            result("app.debug_log", "Application", "Persistent debug log", .local, status, detail, checks: 2,
                   started: started)
        }
        guard stats.fileOpen else {
            return make(.fail, "The log keeps its lines in memory only: \(redact(stats.lastError))")
        }
        let text = (try? String(contentsOfFile: stats.path, encoding: .utf8)) ?? ""
        guard text.contains(marker) else {
            return make(.fail, "A probe line wasn't found in \(redact(stats.path)) after flushing")
        }
        let fallback = Updater.downloadFolders.contains { stats.path.hasPrefix($0) }
        return make(.pass, "Writes reach \(redact(stats.path))\(fallback ? " (the fallback folder)" : ""); "
            + "\(stats.fileBytes) bytes, \(stats.rotations) rotations, \(stats.writeErrors) write errors, "
            + "\(stats.reopens) reopens")
    }

    // MARK: - Updates

    static var helperPath: String { Bundle.main.bundlePath + "/octopushelper" }

    static func helperPresent() -> DiagnosticResult {
        #if targetEnvironment(simulator)
        return result("updates.helper", "Updates", "Update helper in the app", .local, .skipped,
                      "Simulator builds carry no update helper")
        #else
        let present = FileManager.default.isExecutableFile(atPath: helperPath)
        return result("updates.helper", "Updates", "Update helper in the app", .local, present ? .pass : .fail,
                      present ? "octopushelper is in the app and executable"
                              : "octopushelper is missing from the app, so updates can't install in place")
        #endif
    }

    static func helperRuns() -> DiagnosticResult {
        let started = Date()
        let id = "updates.helper_root", group = "Updates", title = "Update helper starts as root"
        #if targetEnvironment(simulator)
        return result(id, group, title, .local, .skipped, "Simulator builds carry no update helper")
        #else
        guard FileManager.default.isExecutableFile(atPath: helperPath) else {
            return result(id, group, title, .local, .blocked, "The update helper is missing from the app")
        }
        let outcome = RootHelper.run(helperPath, ["version"]) // prints its version; changes nothing
        let reason = outcome.errors.last ?? "exit \(outcome.status)"
        if outcome.status == 0, let line = outcome.lines.first(where: { $0.contains("octopushelper") }) {
            let version = line.components(separatedBy: "\t").last ?? line
            return result(id, group, title, .local, .pass, "Started as root through TrollStore's persona spawn: \(version)",
                          started: started)
        }
        if outcome.status == -1 || outcome.status == 3 {
            return result(id, group, title, .local, .blocked,
                          "It can't run as root, so updates install from the releases page instead: \(reason)",
                          started: started)
        }
        return result(id, group, title, .local, .fail, reason, started: started)
        #endif
    }

    static func releaseCheck() async -> DiagnosticResult {
        let started = Date()
        let id = "updates.release_check", group = "Updates", title = "Release list from GitHub"
        let outcome: Result<[OctopusRelease], Updater.FetchFailure> = await withCheckedContinuation { continuation in
            Updater.fetchReleases { continuation.resume(returning: $0) }
        }
        switch outcome {
        case .success(let releases):
            let installed = Updater.versionParts(AppInfo.shortVersion)
            let chosen = Updater.select(from: releases, app: installed)
            let newest = releases.max { Updater.compare($0.version, $1.version) < 0 }?.name ?? "none"
            return result(id, group, title, .external, .pass,
                          "\(releases.count) releases, newest \(newest); "
                              + (chosen.available.map { "\($0.name) is available" } ?? "this build is up to date"),
                          started: started)
        case .failure(let failure):
            let offline = failure.reason.contains("offline")
            return result(id, group, title, .external, offline ? .skipped : .fail,
                          offline ? "No network: \(failure.reason)" : failure.reason, started: started)
        }
    }

    // MARK: - Device, read-only

    static func plistObservation(_ id: String, _ group: String, _ title: String, path: String,
                                 highlight: [String]) -> DiagnosticResult {
        let started = Date()
        guard let lines = flattened(path) else {
            return result(id, group, title, .local, .skipped, "\(path) isn't present or readable on this device",
                          started: started)
        }
        let picked = lines.filter { line in highlight.contains { line.contains($0) } }
        let shown = picked.prefix(4).joined(separator: "; ")
        return result(id, group, title, .local, .pass,
                      "\(lines.count) values read\(picked.isEmpty ? "" : "; \(shown)")",
                      checks: UInt32(min(lines.count, Int(UInt32.max))), started: started)
    }

    /// Every value of a property list as key.path = value lines, or nil when it can't be read.
    static func flattened(_ path: String) -> [String]? {
        guard let data = FileManager.default.contents(atPath: path),
              let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) else { return nil }
        var lines: [String] = []
        DeviceReport.flatten(plist, path: "", depth: 0, into: &lines)
        return lines
    }

    static func ptpdProcess() -> DiagnosticResult {
        let id = "device.ptpd_process", group = "Device (read-only)", title = "ptpd running now"
        var pid: Int32 = 0
        let status = oct_diag_find_process("ptpd", &pid)
        guard status == OCT_OK else {
            return result(id, group, title, .local, .skipped, "The process table can't be read here")
        }
        return result(id, group, title, .local, .pass,
                      pid > 0 ? "Apple's ptpd is running (pid \(pid)); this does not prove host attachment or Octopus USB traffic"
                              : "Apple's ptpd was not found in this process snapshot; host state is unknown")
    }

    static func ptpRegistry() -> DiagnosticResult {
        var probe = oct_device_probe()
        let status = oct_device_probe_ptp(&probe)
        return result("device.ptp_registry", "Device (read-only)", "PTP interface discovery", .local,
                      status == OCT_OK ? .pass : .blocked, text(probe.detail), checks: status == OCT_OK ? 1 : 0)
    }

    static func ptpdIdentity() -> DiagnosticResult {
        let id = "device.ptpd_identity", group = "Device (read-only)", title = "ptpd binary identity"
        do {
            let file = try FileHandle(forReadingFrom: URL(fileURLWithPath: "/usr/libexec/ptpd"))
            defer { try? file.close() }
            // Bound allocation; capture no binary bytes in the exported report.
            let data = try file.read(upToCount: 8 * 1024 * 1024 + 1) ?? Data()
            guard data.count <= 8 * 1024 * 1024 else {
                return result(id, group, title, .local, .blocked, "ptpd exceeds the 8 MiB observation limit", checks: 0)
            }
            return ptpdIdentity(data: data)
        } catch {
            let e = error as NSError
            return result(id, group, title, .local, .blocked,
                          "Cannot read ptpd: \(e.domain) code \(e.code); \(redact(e.localizedDescription))", checks: 0)
        }
    }

    static func ptpdIdentity(data: Data) -> DiagnosticResult {
        var identity = oct_macho_identity()
        let status = data.withUnsafeBytes { bytes in
            oct_device_parse_identity(bytes.bindMemory(to: UInt8.self).baseAddress, bytes.count, &identity)
        }
        let id = "device.ptpd_identity", group = "Device (read-only)", title = "ptpd binary identity"
        guard status == OCT_OK else {
            return result(id, group, title, .local, .blocked, "Unsupported or malformed Mach-O identity: \(status.rawValue)", checks: 0)
        }
        let uuid = text(identity.uuid)
        let digest = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        let reference = uuid == "FFD54FC7-0508-3AB8-8D17-2F698B611E10"
            && identity.cpu_type == 0x0100000c && identity.cpu_subtype == 0x80000002
            && identity.file_type == 2 && identity.platform == 2 && identity.minimum_os == 0x00110000
        let exact = digest == "a1742358da3e257204a0f190211cda26a155dfe0dabb3c9fa572117460085dcb"
        return result(id, group, title, .local, .pass,
                      "UUID \(uuid); SHA-256 \(digest); reference metadata match=\(reference), exact bytes=\(exact). "
                        + "Identity observation only; native integration remains disabled. A re-signed reference may differ.")
    }

    // MARK: - External connection

    static func usbTransport() -> DiagnosticResult {
        result("external.usb_transport", "External connection", "USB transport for Octopus", .external, .blocked,
               "This build has no native USB driver registered with the transport adapter. Read-only PTP discovery "
                   + "does not open Apple's interface or prove ownership. Private transport ABI and provider coexistence "
                   + "still require device evidence.", checks: 0)
    }

    static func hostRequests(_ environment: DiagnosticsEnvironment) -> DiagnosticResult {
        let id = "external.host_requests", group = "External connection", title = "Requests from a computer"
        return result(id, group, title, .external, .blocked,
                      "No verified native USB request evidence is available. Generic transaction count: \(environment.transportTransactions). "
                          + "Transport: \(environment.transportState)"
                          + (environment.transportFailure.isEmpty ? "" : ", last failure: \(environment.transportFailure)"),
                      checks: 0)
    }

    static func windows() -> DiagnosticResult {
        result("external.windows", "External connection", "Windows File Explorer", .external, .blocked,
               "Needs a Windows PC and manual steps, and the USB transport above. With the iPhone connected and "
                   + "unlocked, run tools\\windows\\octopus-usb-diagnostics.ps1 and send its report with this ZIP.",
               checks: 0)
    }

    // MARK: - Bridge state checks

    /// Whether the app's bridge socket is active, based on the environment snapshot taken when the run started.
    static func bridgeAppActive(_ environment: DiagnosticsEnvironment) -> DiagnosticResult {
        let id = "bridge.app_active", group = "Bridge", title = "App bridge socket server"
        guard environment.bridgeActive else {
            return result(id, group, title, .local, .blocked,
                          "The bridge socket server is not running. Enable USB Access to start it at "
                              + (environment.bridgeSocketPath.isEmpty
                                 ? String(cString: OCT_BRIDGE_DEFAULT_SOCKET)
                                 : environment.bridgeSocketPath)
                              + ". The separate bridge.app_socket core check probes it at run time.",
                          checks: 0)
        }
        let path = environment.bridgeSocketPath.isEmpty
            ? String(cString: OCT_BRIDGE_DEFAULT_SOCKET) : environment.bridgeSocketPath
        return result(id, group, title, .local, .pass,
                      "Bridge socket server is running at \(path). "
                          + "The core check bridge.app_socket probes it via PING; "
                          + "install OctopusBridge and enable it in Sileo to route native PTP traffic from Windows.")
    }

    /// Whether a native integration client (the OctopusBridge tweak or a diagnostic probe) is connected.
    static func bridgeClientState(_ environment: DiagnosticsEnvironment) -> DiagnosticResult {
        let id = "bridge.client_connected", group = "Bridge", title = "Integration client connected"
        guard environment.bridgeActive else {
            return result(id, group, title, .local, .blocked,
                          "No bridge socket server is running; there can be no client. Enable USB Access first.",
                          checks: 0)
        }
        if environment.bridgeClientConnected {
            return result(id, group, title, .local, .pass,
                          "A client is connected to the bridge socket. "
                              + "If OctopusBridge is installed in Sileo and enabled, this is the ptpd integration. "
                              + "Otherwise it may be the bridge.app_socket core check running concurrently.")
        }
        return result(id, group, title, .local, .blocked,
                      "The bridge socket is listening but no client is connected. "
                          + "Install OctopusBridge from Sileo (Octopus/Bridge/control), enable it, "
                          + "then connect the iPhone to Windows and plug in a USB cable to trigger ptpd.")
    }

    // MARK: - The archive

    struct BuildInfo: Codable, Sendable {
        let version: String
        let build: String
        let commit: String
        let bundle: String
        let core: String
        let system: String
    }

    struct Report: Codable, Sendable {
        var schema = "octopus-diagnostics/1"
        let build: BuildInfo
        let started: String
        let finished: String
        let durationSeconds: Double
        let summary: [String: Int]
        let environment: DiagnosticsEnvironment
        let results: [DiagnosticResult]
    }

    static var buildInfo: BuildInfo {
        BuildInfo(version: AppInfo.shortVersion, build: AppInfo.buildNumber, commit: AppInfo.commit,
                  bundle: Bundle.main.bundleIdentifier ?? "", core: OCT_CORE_VERSION,
                  system: DebugLog.systemDescription)
    }

    /// Where an export may be written, tried in order: the app's temporary folder; then Octopus's own folders, which
    /// an unsandboxed TrollStore app can write when its container refuses (on iOS 17 the container's folders can't
    /// be created); then the folder the debug log is open in, which has taken writes this launch.
    static var exportFolders: [String] {
        var folders = [FileManager.default.temporaryDirectory.path]
        folders += Updater.downloadFolders.map { $0 + "/Diagnostics" }
        let log = DebugLog.stats()
        if log.fileOpen, !log.path.isEmpty { folders.append((log.path as NSString).deletingLastPathComponent) }
        var seen = Set<String>()
        return folders.filter { !$0.isEmpty && seen.insert(($0 as NSString).standardizingPath).inserted }
    }

    static let archivePrefix = "octopus-diagnostics-"

    static func makeReport(results: [DiagnosticResult], environment: DiagnosticsEnvironment, started: Date,
                           finished: Date) -> Report {
        let iso = ISO8601DateFormatter()
        var summary: [String: Int] = [:]
        for status in DiagnosticStatus.allCases { summary[status.rawValue] = results.filter { $0.status == status }.count }
        return Report(build: buildInfo, started: iso.string(from: started), finished: iso.string(from: finished),
                      durationSeconds: finished.timeIntervalSince(started), summary: summary,
                      environment: environment, results: results.map(redacted))
    }

    /// results.txt and failures.txt in one text, for when no archive can be written.
    static func plainText(results: [DiagnosticResult], environment: DiagnosticsEnvironment, started: Date,
                          finished: Date) -> String {
        textReport(makeReport(results: results, environment: environment, started: started, finished: finished))
            + "\n" + failures(results)
    }

    static func makeArchive(results: [DiagnosticResult], environment: DiagnosticsEnvironment, started: Date,
                            finished: Date, directories: [String] = Diagnostics.exportFolders) throws -> URL {
        DebugLog.flush()
        var created: OpaquePointer? = nil
        guard oct_zip_create(&created) == OCT_OK, let zip = created else { throw ArchiveError("the archive couldn't start") }
        defer { oct_zip_destroy(zip) }
        let modified = Int64(finished.timeIntervalSince1970)
        func add(_ name: String, _ data: Data) throws {
            let status = data.withUnsafeBytes { raw in oct_zip_add(zip, name, raw.baseAddress, raw.count, modified) }
            guard status == OCT_OK else { throw ArchiveError("\(name) couldn't be added") }
        }
        let report = makeReport(results: results, environment: environment, started: started, finished: finished)
        let summary = report.summary
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.keyEncodingStrategy = .convertToSnakeCase
        try add("README.txt", Data(readme(summary: summary).utf8))
        try add("results.json", try encoder.encode(report))
        try add("results.txt", Data(textReport(report).utf8))
        try add("failures.txt", Data(failures(results).utf8))
        try add("build.json", try encoder.encode(report.build))
        try add("settings.json", try encoder.encode(environment.settings))
        try add("connectivity.txt", Data(connectivity(environment).utf8))
        try add("device-observations.json", try encoder.encode(report.results.filter { $0.id.hasPrefix("device.") }))
        var timeline = ["Saved log observations, not proof of native USB traffic."]

        for file in DebugLog.files() {
            let text = (try? String(contentsOf: file, encoding: .utf8)) ?? ""
            try add("logs/\(file.lastPathComponent)", Data(redact(text).utf8))
            timeline += redact(text).components(separatedBy: "\n").filter {
                $0.contains(" link ") || $0.contains(" service ") || $0.contains(" diagnostics ")
            }

        }
        try add("connection-timeline.txt", Data(timeline.joined(separator: "\n").utf8))
        let stamp = DateFormatter()
        stamp.locale = Locale(identifier: "en_US_POSIX")
        stamp.dateFormat = "yyyyMMdd-HHmmss"
        let name = "\(archivePrefix)\(AppInfo.shortVersion)-\(stamp.string(from: finished)).zip"
        let folders = directories.filter { !$0.isEmpty }
        guard !folders.isEmpty else { throw ArchiveError("No folder was given for the archive.") }
        // Each folder is checked (it exists or can be made, and takes a file), written through a temporary file
        // synced and renamed, and read back with every CRC checked; the next folder is tried when one fails.
        var written = [CChar](repeating: 0, count: 4096)
        let capacity = written.count
        let status = withCStrings(folders) { list in
            oct_zip_write_first(zip, list, folders.count, name, &written, capacity)
        }
        let reasons = String(cString: oct_zip_error(zip))
        guard status == OCT_OK else {
            throw ArchiveError(reasons.isEmpty ? "No folder took the archive." : reasons)
        }
        let url = URL(fileURLWithPath: string(written))
        if !reasons.isEmpty {
            DebugLog.log(.info, "diagnostics", "export_fallback",
                         "The archive went to \(redact(url.deletingLastPathComponent().path)) after: \(redact(reasons))")
        }
        removeOlderArchives(beside: url)
        return url
    }

    /// Earlier exports in the same folder; the newest is the one to share.
    static func removeOlderArchives(beside url: URL) {
        let folder = url.deletingLastPathComponent()
        let names = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        for name in names where name.hasPrefix(archivePrefix) && name.hasSuffix(".zip") && name != url.lastPathComponent {
            try? FileManager.default.removeItem(at: folder.appendingPathComponent(name))
        }
    }

    /// The strings as a C array of C strings, valid while `body` runs.
    static func withCStrings<T>(_ strings: [String], _ body: (UnsafePointer<UnsafePointer<CChar>?>) -> T) -> T {
        let copies = strings.map { strdup($0) }
        defer { copies.forEach { free($0) } }
        let pointers = copies.map { UnsafePointer($0) }
        return pointers.withUnsafeBufferPointer { buffer in body(buffer.baseAddress!) }
    }

    /// A NUL-terminated C buffer as a String.
    static func string(_ buffer: [CChar]) -> String {
        String(decoding: buffer.prefix(while: { $0 != 0 }).map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }

    struct ArchiveError: LocalizedError {
        let message: String
        init(_ message: String) { self.message = message }
        var errorDescription: String? { message }
    }

    static func readme(summary: [String: Int]) -> String {
        """
        Octopus diagnostics, \(AppInfo.version), \(DebugLog.systemDescription).

        PASS \(summary["PASS"] ?? 0), FAIL \(summary["FAIL"] ?? 0), SKIPPED \(summary["SKIPPED"] ?? 0), BLOCKED \(summary["BLOCKED"] ?? 0).

        results.json      every check: id, group, title, status, kind, checks, duration, detail
        results.txt       the same, to read
        failures.txt      the checks that didn't pass, and recent warnings and errors from the log
        build.json        version, build, commit, core and system
        settings.json     Octopus's settings
        connectivity.txt  the transport's state, and how this iPhone presents itself over USB (read-only)
        logs/             the debug log files

        If the app's temporary folder can't take the archive, it is written to Octopus's folder in
        /var/mobile/Library/Caches instead; the debug log says which folder was used and why.

        Kinds: local runs inside the app; simulated talks to a simulated host through the real transport adapter
        and responder, never a computer; external is a real connection to something outside the iPhone.
        Paths have app container IDs replaced with <app-container>. Nothing here holds file contents.
        """
    }

    static func textReport(_ report: Report) -> String {
        var lines = ["Octopus \(report.build.version) (\(report.build.build)), commit \(report.build.commit), "
                        + report.build.system,
                     "Started \(report.started), \(String(format: "%.1f", report.durationSeconds)) s", ""]
        var group = ""
        for result in report.results {
            if result.group != group {
                group = result.group
                lines += ["", group]
            }
            lines.append("  [\(result.status.rawValue)] \(result.title) (\(result.kind.rawValue), \(result.checks) checks, "
                         + "\(String(format: "%.1f", result.durationMs)) ms): \(result.detail)")
        }
        return lines.joined(separator: "\n") + "\n"
    }

    static func failures(_ results: [DiagnosticResult]) -> String {
        let notPassed = results.filter { $0.status != .pass }
        var lines = notPassed.isEmpty ? ["Every check passed."]
            : notPassed.map { "[\($0.status.rawValue)] \($0.id): \(redact($0.detail))" }
        let recent = DebugLog.entries(limit: 4000).filter { $0.level >= .warning }.suffix(200)
        lines += ["", "Recent warnings and errors from the debug log (\(recent.count)):"]
        lines += recent.map { redact($0.line) }
        return lines.joined(separator: "\n") + "\n"
    }

    static func connectivity(_ environment: DiagnosticsEnvironment) -> String {
        let socketPath = environment.bridgeSocketPath.isEmpty
            ? String(cString: OCT_BRIDGE_DEFAULT_SOCKET) : environment.bridgeSocketPath
        var lines = ["Service: \(environment.serviceState)", "Transport: \(environment.transportState)",
                     "Transport failure: \(environment.transportFailure.isEmpty ? "none" : environment.transportFailure)",
                     "Requests from a computer this launch: \(environment.transportTransactions)",
                     "Bridge socket: \(environment.bridgeActive ? "running at \(socketPath)" : "not running")",
                     "Bridge client: \(environment.bridgeClientConnected ? "connected" : "none")"]
        var pid: Int32 = 0
        let found = oct_diag_find_process("ptpd", &pid) == OCT_OK
        lines.append("ptpd: \(found ? (pid > 0 ? "running, pid \(pid)" : "not running") : "process table unreadable")")
        for file in deviceFiles {
            lines += ["", "\(file.path):"]
            lines += (flattened(file.path) ?? ["not present or readable"]).map { "  \($0)" }
        }
        return redact(lines.joined(separator: "\n")) + "\n"
    }

    // MARK: - Helpers

    static func settingsSnapshot() -> [String: String] {
        let defaults = UserDefaults.standard
        let keys = [Preferences.haptics, Preferences.selfTestOnEnable, Preferences.scratchFolder,
                    Preferences.updateNotifications, Preferences.detailedLogging]
        var settings: [String: String] = [:]
        for key in keys { settings[key] = defaults.object(forKey: key).map { "\($0)" } ?? "default" }
        return settings
    }

    private static let container = try? NSRegularExpression(
        pattern: "(/[Cc]ontainers/(?:Data|Bundle|Shared/AppGroup)/Application/)[0-9A-Fa-f-]{36}")

    /// Paths keep their shape, but app container IDs become <app-container>.
    static func redact(_ text: String) -> String {
        guard let container else { return text }
        let range = NSRange(text.startIndex..., in: text)
        return container.stringByReplacingMatches(in: text, options: [], range: range, withTemplate: "$1<app-container>")
    }

    static func redacted(_ result: DiagnosticResult) -> DiagnosticResult {
        DiagnosticResult(id: result.id, group: result.group, title: result.title, status: result.status,
                         kind: result.kind, checks: result.checks, durationMs: result.durationMs,
                         detail: redact(result.detail))
    }

    static func text<T>(_ tuple: T) -> String {
        withUnsafeBytes(of: tuple) { raw in String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self) }
    }
}
