import Foundation
import Combine
import OctopusCore

/// Explicit, per-launch consent. Never persists the experimental switches.
/// The native module checks a 15-second monotonic lease on every provider read.
@MainActor
final class NativeIntegration: ObservableObject {
    static let shared = NativeIntegration()
    @Published var observe = false { didSet { changed() } }
    @Published var synthetic = false { didSet { changed() } }
    @Published private(set) var detail = "Optional native companion required. Disabled by default."
    private(set) var token = ""
    private var active = false
    private var foreground = true
    private var leasedMode: UInt32 = 0
    private var renewal: Task<Void, Never>?

    private func changed() {
        if !observe && synthetic { synthetic = false; return }
        synchronize()
    }

    func sync(serviceActive: Bool) { active = serviceActive; synchronize() }
    func setForeground(_ value: Bool) { foreground = value; synchronize() }
    func revoke() { active = false; synchronize() }

    private func synchronize() {
        let wanted: UInt32 = observe && active && foreground ? (synthetic ? 2 : 1) : 0
        if wanted == leasedMode { return }
        renewal?.cancel(); renewal = nil
        leasedMode = wanted
        token = ""
        #if !targetEnvironment(simulator)
        _ = oct_native_io_remove_lease(OCT_NATIVE_DIRECTORY)
        #endif
        guard wanted != 0 else {
            detail = observe ? "Enable USB Access and keep Octopus open to authorize the native experiment."
                             : "Native experiment disabled. Any previous lease expires within 15 seconds."
            return
        }
        token = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        renew()
        renewal = Task { [weak self] in
            while !Task.isCancelled {
                do { try await Task.sleep(nanoseconds: 5_000_000_000) } catch { break }
                guard !Task.isCancelled else { break }
                self?.renew()
            }
        }
    }

    private func renew() {
        #if targetEnvironment(simulator)
        detail = "Native ptpd integration is unavailable in the Simulator."
        #else
        let now = oct_link_now_ms()
        guard leasedMode != 0, now > 0 else { return }
        // Parent is the same mobile-owned cache folder used by the existing log.
        let parent = (OCT_NATIVE_DIRECTORY as NSString).deletingLastPathComponent
        do { try FileManager.default.createDirectory(atPath: parent, withIntermediateDirectories: true) }
        catch { detail = "Native lease directory: \(error.localizedDescription)"; return }
        let result = oct_native_write_lease(OCT_NATIVE_DIRECTORY, token, leasedMode, UInt64(now))
        let error = errno
        detail = result == OCT_OK
            ? (leasedMode == 2 ? "Synthetic provider authorized while Octopus stays open. Run diagnostics after using Explorer."
                              : "Read-only native observer authorized while Octopus stays open. Run diagnostics after using Explorer.")
            : "Native lease failed: status \(result.rawValue), errno \(error)."
        #endif
    }
}

/// One bounded, immutable capture shared by the run's checks and ZIP export.
/// Correlation tokens identify an experiment; they are not a security boundary
/// against other software running as the same mobile user.
struct NativeCapture: Codable, Sendable {
    var data: Data?
    var problem: String
    var expectedLease: String
    var observedPID: Int32
    var capturedAt: Double
    var capturedMonotonicMs: UInt64 = 0
    var appUID: UInt32 = 0

    static func read(expectedLease: String) -> NativeCapture {
        var bytes = [UInt8](repeating: 0, count: 65536)
        var length = 0
        let result = bytes.withUnsafeMutableBytes {
            oct_native_io_read(OCT_NATIVE_DIRECTORY, "status", $0.baseAddress, $0.count, &length)
        }
        let error = errno
        var pid: Int32 = 0
        _ = oct_diag_find_process("ptpd", &pid)
        return NativeCapture(data: result == OCT_OK ? Data(bytes.prefix(length)) : nil,
                             problem: result == OCT_OK ? "" : "Native status unavailable: status \(result.rawValue), errno \(error). Install the optional companion and activate the observer, then reconnect USB.",
                             expectedLease: expectedLease, observedPID: pid, capturedAt: Date().timeIntervalSince1970,
                             capturedMonotonicMs: UInt64(max(0, oct_link_now_ms())), appUID: geteuid())
    }

    var report: NativeReport? {
        guard let data else { return nil }
        let decoder = JSONDecoder(); decoder.keyDecodingStrategy = .convertFromSnakeCase
        return try? decoder.decode(NativeReport.self, from: data)
    }

    var validationProblem: String? {
        guard let report else { return problem.isEmpty ? "Malformed native status JSON." : problem }
        guard report.schema == "octopus-native/1", report.generation != 0 else { return "Unsupported native status schema or generation." }
        guard capturedAt - report.generatedAt >= -1, capturedAt - report.generatedAt <= 5 else { return "Native status is stale or its clock is inconsistent." }
        guard observedPID > 0, report.pid == observedPID else { return "Native status does not match the running ptpd process." }
        guard report.uid == appUID else { return "App and ptpd do not share the same user identity; authorization is refused." }
        guard !expectedLease.isEmpty, report.lease == expectedLease, report.authorized else { return "No matching active authorization lease. Keep Octopus open with Enable USB Access and native observation enabled." }
        guard report.expiresAtMs > capturedMonotonicMs, report.expiresAtMs - capturedMonotonicMs <= 15000 else { return "Native authorization is expired or has an invalid deadline." }
        guard report.leasePairedResponses <= report.leaseCommands else { return "Inconsistent native transaction counters." }
        guard report.hooks == "Installed" else { return "Native ABI checks did not enable observation: \(report.hooks). \(report.compatibility)" }
        return nil
    }

    func result(transactions: Bool) -> DiagnosticResult {
        let id = transactions ? "native.usb_transactions" : "native.observer"
        let title = transactions ? "USB callbacks and paired native responses" : "Native observer compatibility and authorization"
        let problem = validationProblem
        let proven = problem == nil && (report?.leaseCommands ?? 0) > 0 && (report?.leasePairedResponses ?? 0) > 0
        let detail: String
        if let problem { detail = problem }
        else if let report {
            detail = transactions
                ? "Current authorization: \(report.leaseCommands) commands inside native USB receive callbacks; \(report.leasePairedResponses) paired successful response writes. \(report.leaseProviderEvents) provider events. \(report.lastProviderEvent). Host receipt, Explorer visibility and downloaded SHA-256 still need Windows verification."
                : "\(report.compatibility). Observer \(report.hooks); provider \(report.provider). PID \(report.pid), generation \(report.generation). Observer-only storage request prerequisite: \(report.providerEligible ? "met" : "awaiting GetStorageIDs and GetStorageInfo responses")."
        } else { detail = "No native report." }
        return DiagnosticResult(id: id, group: "Native integration", title: title,
                                status: problem == nil && (!transactions || proven) ? .pass : .blocked,
                                kind: transactions && proven ? .realUSB : .nativeObservation,
                                checks: problem == nil ? 1 : 0, durationMs: 0, detail: detail)
    }
}

struct NativeReport: Decodable, Sendable {
    let schema: String
    let pid: Int32
    let uid: UInt32
    let generation: UInt64
    let generatedAt: Double
    let lease: String
    let authorized: Bool
    let expiresAtMs: UInt64
    let providerEligible: Bool
    let compatibility: String
    let hooks: String
    let provider: String
    let leaseCommands: UInt64
    let leasePairedResponses: UInt64
    let leaseProviderEvents: UInt64
    let lastProviderEvent: String
}
