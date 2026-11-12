import Foundation
import Combine
import OctopusCore

/// The USB transport as it actually runs: Apple's own ptpd, which owns the PTP function on iOS,
/// with Octopus's companion inside it serving the storage a host sees.
///
/// The USB Connection screen used to read only the app's portable responder, which has no USB
/// driver of its own and so reported "Transport unavailable" permanently, even while a host was
/// browsing the phone through ptpd. This reads the companion instead.
///
/// Liveness is a heartbeat, not the last value read. The companion posts its runtime snapshot about
/// once a second for as long as ptpd runs, and ptpd runs only while a host is connected; but the
/// snapshot's state outlives ptpd, so trusting it alone would show "Session active" indefinitely
/// after the cable is pulled. Every decision here is made in C, where it is tested, and this class
/// only asks and displays.
final class NativeTransport: ObservableObject {
    static let shared = NativeTransport()

    @Published private(set) var stage: ConnectionStage = .transportUnavailable
    @Published private(set) var commands = 0
    @Published private(set) var responses = 0
    /// Every stage observed since launch, so the screen can mark them Seen.
    @Published private(set) var seen: Set<ConnectionStage> = [.transportUnavailable]
    /// Whether the companion has ever been heard from since launch: the transport exists on this
    /// phone, whether or not a host is connected right now.
    @Published private(set) var everHeard = false

    /// How long without a post before the companion counts as gone. Five polls' worth.
    private static let windowMs: UInt64 = 5000
    private var lastHeardMs: UInt64 = 0
    private var timer: Timer?

    private init() {
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            self?.refresh()
        }
    }

    /// A companion heard from within the window, with hooks in: the transport is up, whether or not
    /// a host has sent anything yet. Disconnected is not live, which an earlier draft got wrong.
    var live: Bool { stage == .waiting || stage == .hostConnected || stage == .sessionActive }

    /// Monotonic, so a clock change cannot make a stale snapshot look fresh.
    private static func nowMs() -> UInt64 {
        DispatchTime.now().uptimeNanoseconds / 1_000_000
    }

    func refresh() {
        let now = Self.nowMs()
        var posted = false
        if oct_native_runtime_check(&posted) == OCT_OK, posted {
            lastHeardMs = now
            everHeard = true
        }
        var runtime = oct_native_runtime()
        let readable = oct_native_runtime_read(&runtime) == OCT_OK
        let fresh = readable && oct_native_transport_fresh(now, lastHeardMs, Self.windowMs)
        let native = oct_native_transport_stage_of(&runtime, fresh)

        let next: ConnectionStage
        if native == OCT_NATIVE_TRANSPORT_SERVING {
            next = .sessionActive
        } else if native == OCT_NATIVE_TRANSPORT_HOST {
            next = .hostConnected
        } else if native == OCT_NATIVE_TRANSPORT_WAITING {
            next = .waiting
        } else {
            // Heard before and silent now is a disconnect, which is worth saying; never heard is
            // simply unavailable.
            next = everHeard ? .disconnected : .transportUnavailable
        }
        if stage != next { stage = next }
        if !seen.contains(next) { seen.insert(next) }
        let liveCommands = fresh ? Int(runtime.commands) : 0
        busy = liveCommands > commands
        let liveResponses = fresh ? Int(runtime.responses) : 0
        if commands != liveCommands { commands = liveCommands }
        if responses != liveResponses { responses = liveResponses }
    }

    /// Whether a host is mid-transfer: the command count moved since the previous poll. Restarting
    /// the responder then would interrupt a copy, so callers wait.
    private(set) var busy = false

    /// One line for the screen's status row.
    var summary: String {
        switch stage {
        case .sessionActive: return "Session active"
        case .hostConnected: return "Host connected"
        case .waiting: return "Waiting for a connection"
        case .disconnected: return "Disconnected"
        default: return "Transport unavailable"
        }
    }
}
