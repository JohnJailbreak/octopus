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
    func revoke() { active = false; observe = false; synthetic = false; synchronize() }

    private func synchronize() {
        let wanted: UInt32 = observe && active && foreground ? (synthetic ? 2 : 1) : 0
        if wanted == leasedMode { return }
        renewal?.cancel(); renewal = nil
        leasedMode = wanted
        token = ""
        #if !targetEnvironment(simulator)
        _ = oct_native_io_remove_lease(OCT_NATIVE_DIRECTORY)
        // Withdraw the notification too, and move the generation on, so a word captured a moment
        // ago cannot be replayed as this app's grant. A companion that misses this still stops
        // within the 15-second deadline.
        generation = generation &+ 1
        var cleared = oct_native_lease_word()
        cleared.mode = UInt8(OCT_NATIVE_LEASE_NONE.rawValue)
        cleared.generation = generation
        _ = oct_native_lease_publish(&cleared)
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

    /// Changes on every grant and withdrawal, so a companion can see this app restart.
    private var generation: UInt16 = UInt16.random(in: 0...255)

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
        // The file above cannot be read from inside ptpd: the 9 November run measured EPERM opening
        // that folder there. So the same grant also goes out as a notification, which is the one
        // route proven to cross ptpd's sandbox. It only ever authorizes observation, whatever mode
        // is asked for, because any process can post to that name; the companion enforces that in
        // oct_native_lease_effective, and the file remains the only route that could ever carry
        // exposure. Both are published so a phone where the folder does work keeps the stronger one.
        var posted = OCT_E_UNSUPPORTED
        if leasedMode != 0 {
            var word = oct_native_lease_word()
            word.mode = UInt8(leasedMode == 2 ? OCT_NATIVE_LEASE_EXPOSE_SYNTHETIC.rawValue
                                              : OCT_NATIVE_LEASE_OBSERVE.rawValue)
            word.deadline_ms = UInt64(now) + UInt64(OCT_NATIVE_LEASE_MS)
            word.generation = generation
            posted = oct_native_lease_publish(&word)
        }
        let channel = posted == OCT_OK ? "notification"
                    : (result == OCT_OK ? "file" : "neither channel")
        detail = (result == OCT_OK || posted == OCT_OK)
            ? (leasedMode == 2
               ? "Synthetic storage authorized over \(channel) while Octopus stays open. Reconnect USB, then look for a second volume in Explorer."
               : "Read-only native observer authorized over \(channel) while Octopus stays open. Run diagnostics after using Explorer.")
            : "Native lease failed: file status \(result.rawValue) errno \(error); notification status \(posted.rawValue)."
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
    var companionFilesPresent = false

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
                             capturedMonotonicMs: UInt64(max(0, oct_link_now_ms())), appUID: geteuid(),
                             companionFilesPresent: FileManager.default.fileExists(atPath: "/var/jb/usr/lib/TweakInject/OctopusNative.dylib")
                                && FileManager.default.fileExists(atPath: "/var/jb/usr/lib/TweakInject/OctopusNative.plist"))
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
        guard !expectedLease.isEmpty, report.lease == expectedLease, report.authorized else { return "No matching active authorization lease. Keep Octopus open with Enable USB Access and native observation enabled. Native compatibility: \(report.compatibility); hooks: \(report.hooks)." }
        guard report.expiresAtMs > capturedMonotonicMs, report.expiresAtMs - capturedMonotonicMs <= 15000 else { return "Native authorization is expired or has an invalid deadline." }
        guard report.leasePairedResponses <= report.leaseCommands else { return "Inconsistent native transaction counters." }
        guard report.hooks == "Installed" else { return "Native ABI checks did not enable observation: \(report.hooks). \(report.compatibility)" }
        return nil
    }

    func result(transactions: Bool) -> DiagnosticResult {
        let id = transactions ? "native.usb_transactions" : "native.observer"
        let title = transactions ? "USB callbacks and paired native responses" : "Native observer compatibility and authorization"
        // The status file these used to read is written into a folder ptpd's sandbox refuses, so
        // both checks reported BLOCKED even with hooks installed inside ptpd. Fall back to the
        // runtime snapshot the companion posts by notification, which carries the same counters.
        var runtime = oct_native_runtime()
        let haveRuntime = oct_native_runtime_read(&runtime) == OCT_OK
        if report == nil, haveRuntime {
            let hooked = runtime.flags & UInt8(OCT_NATIVE_RUNTIME_HOOKED.rawValue) != 0
            let serving = runtime.flags & UInt8(OCT_NATIVE_RUNTIME_SERVING.rawValue) != 0
            let eligible = runtime.flags & UInt8(OCT_NATIVE_RUNTIME_ELIGIBLE.rawValue) != 0
            let sawIDs = runtime.flags & UInt8(OCT_NATIVE_RUNTIME_SAW_IDS.rawValue) != 0
            let sawInfo = runtime.flags & UInt8(OCT_NATIVE_RUNTIME_SAW_INFO.rawValue) != 0
            // Which provider hooks a host actually called. With only a count, a listing that drops
            // some objects cannot be diagnosed: it could be a hook that never ran or one that ran
            // and answered wrongly.
            var hooks = oct_native_hook_report()
            var hookText = ""
            if oct_native_hooks_read(&hooks) == OCT_OK {
                let bits: [(UInt16, String)] = [
                    (1 << 0, "deviceInfo"), (1 << 1, "storageIDs"), (1 << 2, "storageInfo"),
                    (1 << 3, "objectInfo"), (1 << 4, "handles"), (1 << 5, "bulkInfo"),
                    (1 << 6, "dataSource"), (1 << 7, "propValue"), (1 << 8, "propList"),
                    (1 << 9, "propsSupported"),
                ]
                let fired = bits.filter { hooks.fired & $0.0 != 0 }.map(\.1)
                let missing = bits.filter { hooks.fired & $0.0 == 0 }.map(\.1)
                hookText = " Provider hooks called: \(fired.isEmpty ? "none" : fired.joined(separator: ", "))."
                    + " Never called: \(missing.isEmpty ? "none" : missing.joined(separator: ", "))."
                    + " Text format added to a host's format list: \(hooks.formats_added ? "yes" : "no")."
                    + " Counts: deviceInfo \(hooks.device_info), handles \(hooks.handles), objectInfo \(hooks.object_info), reads \(hooks.get_object)."
            }
            let live = transactions
                ? "\(runtime.commands) commands inside native USB receive callbacks; \(runtime.responses) paired response writes; \(runtime.provider_events) provider events. Reported by notification, not the status file.\(hookText) Host receipt, Explorer visibility and the downloaded SHA-256 still need Windows verification."
                : "Observer hooks \(hooked ? "installed" : "not installed"); provider \(serving ? "serving" : (eligible ? "eligible, awaiting storage requests" : "not eligible")). Storage requests seen: GetStorageIDs \(sawIDs ? "yes" : "no"), GetStorageInfo \(sawInfo ? "yes" : "no"). Mode \(runtime.mode). Reported by notification, not the status file."
            let ok = transactions ? (runtime.commands > 0 && runtime.responses > 0) : hooked
            return DiagnosticResult(id: id, group: "Native integration", title: title,
                                    status: ok ? .pass : .blocked,
                                    kind: transactions && ok ? .realUSB : .nativeObservation,
                                    checks: 1, durationMs: 0, detail: live)
        }
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
