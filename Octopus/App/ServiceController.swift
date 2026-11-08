import Foundation
import SwiftUI
import UIKit
import OctopusCore

// MARK: - Values from the core

/// The app's view of the core service state machine (oct_service_state).
enum ServiceState: CaseIterable, Hashable, Sendable {
    case disabled, starting, ready, waitingForConnection, connected, error

    init(_ raw: oct_service_state) {
        switch raw {
        case OCT_SERVICE_STARTING: self = .starting
        case OCT_SERVICE_READY: self = .ready
        case OCT_SERVICE_WAITING: self = .waitingForConnection
        case OCT_SERVICE_CONNECTED: self = .connected
        case OCT_SERVICE_ERROR: self = .error
        default: self = .disabled
        }
    }

    var title: String {
        switch self {
        case .disabled: return "Disabled"
        case .starting: return "Starting"
        case .ready: return "Ready"
        case .waitingForConnection: return "Waiting for connection"
        case .connected: return "Connected"
        case .error: return "Error"
        }
    }

    /// Waiting and Connected only exist when a USB transport is present.
    func isSupported(withTransport transport: Bool) -> Bool {
        switch self {
        case .waitingForConnection, .connected: return transport
        default: return true
        }
    }
}

struct CoreEvent: Sendable, Equatable {
    enum Kind: Sendable, Equatable {
        case state, operationBegin, stepBegin, stepEnd, operationEnd, log
    }

    let sequence: UInt32
    let kind: Kind
    let state: ServiceState
    let step: UInt32
    let failed: Bool
    let date: Date
    let text: String

    init(_ raw: oct_event) {
        var copy = raw
        switch raw.kind {
        case OCT_EVENT_STATE: kind = .state
        case OCT_EVENT_OPERATION_BEGIN: kind = .operationBegin
        case OCT_EVENT_STEP_BEGIN: kind = .stepBegin
        case OCT_EVENT_STEP_END: kind = .stepEnd
        case OCT_EVENT_OPERATION_END: kind = .operationEnd
        default: kind = .log
        }
        sequence = raw.sequence
        state = ServiceState(raw.state)
        step = raw.step
        failed = raw.failed
        date = Date(timeIntervalSince1970: TimeInterval(raw.time_ms) / 1000)
        text = String(cString: oct_event_text(&copy))
    }
}

/// The transport's own state, as the core reports it (oct_transport_state).
enum TransportState: Hashable, Sendable {
    case unavailable, initializing, waiting, hostConnected, sessionActive, error, disconnected

    init(_ raw: oct_transport_state) {
        switch raw {
        case OCT_TRANSPORT_INITIALIZING: self = .initializing
        case OCT_TRANSPORT_WAITING: self = .waiting
        case OCT_TRANSPORT_HOST_CONNECTED: self = .hostConnected
        case OCT_TRANSPORT_SESSION_ACTIVE: self = .sessionActive
        case OCT_TRANSPORT_ERROR: self = .error
        case OCT_TRANSPORT_DISCONNECTED: self = .disconnected
        default: self = .unavailable
        }
    }
}

/// What the USB Connection window lists: the service's states, then the transport's. A stage is marked only when
/// the core reports it, so nothing here is assumed.
enum ConnectionStage: CaseIterable, Hashable, Sendable {
    case serviceDisabled, serviceInitializing, responderReady, serviceError
    case transportUnavailable, transportInitializing, waiting, hostConnected, sessionActive, transportError, disconnected

    static let service: [ConnectionStage] = [.serviceDisabled, .serviceInitializing, .responderReady, .serviceError]
    static let transport: [ConnectionStage] = [.transportUnavailable, .transportInitializing, .waiting, .hostConnected,
                                               .sessionActive, .transportError, .disconnected]

    init(service state: ServiceState) {
        switch state {
        case .disabled: self = .serviceDisabled
        case .starting: self = .serviceInitializing
        case .ready, .waitingForConnection, .connected: self = .responderReady
        case .error: self = .serviceError
        }
    }

    init(transport state: TransportState) {
        switch state {
        case .unavailable: self = .transportUnavailable
        case .initializing: self = .transportInitializing
        case .waiting: self = .waiting
        case .hostConnected: self = .hostConnected
        case .sessionActive: self = .sessionActive
        case .error: self = .transportError
        case .disconnected: self = .disconnected
        }
    }

    var title: String {
        switch self {
        case .serviceDisabled: return "Service disabled"
        case .serviceInitializing: return "Service initializing"
        case .responderReady: return "Responder ready"
        case .serviceError: return "Service error"
        case .transportUnavailable: return "Transport unavailable"
        case .transportInitializing: return "Transport initializing"
        case .waiting: return "Waiting for a connection"
        case .hostConnected: return "Host connected"
        case .sessionActive: return "Session active"
        case .transportError: return "Transport error"
        case .disconnected: return "Disconnected"
        }
    }

    /// Every transport stage but Unavailable needs a transport, which this build doesn't have on the iPhone.
    var needsTransport: Bool { ConnectionStage.transport.contains(self) && self != .transportUnavailable }
}

struct CoreStatus: Equatable, Sendable {
    var state: ServiceState = .disabled
    var transportAvailable = false
    var transportName = ""
    var sessionOpen = false
    var sessionID: UInt32 = 0
    var storageID: UInt32 = 0
    var objectCount: UInt32 = 0
    var operationCount: UInt32 = 0
    var transactions: UInt64 = 0
    var scratch = false
    var starts: UInt32 = 0
    var stops: UInt32 = 0
    var transportState: TransportState = .unavailable
    var transportFailure = ""
    var transportTransactions: UInt64 = 0
}

struct StorageSnapshot: Equatable, Sendable {
    let name: String
    let capacity: UInt64
    let free: UInt64
    let readOnly: Bool
    let objectCount: Int
}

struct StorageObject: Identifiable, Equatable, Sendable {
    let id: UInt32
    let parent: UInt32
    let name: String
    let isDirectory: Bool
    let writeProtected: Bool
    let size: UInt64
    let format: UInt16
    let modified: Date

    init(_ raw: oct_object_info) {
        var copy = raw
        id = raw.handle
        parent = raw.parent
        name = String(cString: oct_object_info_name(&copy))
        isDirectory = raw.is_directory
        writeProtected = raw.write_protected
        size = raw.size
        format = raw.format
        modified = Date(timeIntervalSince1970: TimeInterval(raw.modified))
    }

    var formatName: String {
        let name: String
        switch format {
        case 0x3001: name = "Folder"
        case 0x3004: name = "Text"
        default: name = "Undefined"
        }
        return name + String(format: " (0x%04X)", format)
    }

    var caption: String {
        let access = writeProtected ? "read-only" : "writable"
        return isDirectory ? "Folder, \(access)" : "\(size.byteString), \(access)"
    }
}

struct StorageEntry: Identifiable, Equatable, Sendable {
    let object: StorageObject
    let depth: Int

    var id: UInt32 { object.id }
}

struct LogEntry: Identifiable, Equatable {
    let id: UInt32
    let date: Date
    let text: String

    var isFailure: Bool { text.hasPrefix("! ") }
    var line: String { LogEntry.clock.string(from: date) + "  " + text }

    private static let clock: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        return formatter
    }()
}

struct CoreConfiguration: Sendable {
    var scratch: Bool
    var selfTest: Bool
}

/// How long the activity sheet shows each step.
struct Pacing: Sendable {
    var stepDisplay: Double
    var stepSettle: Double
    var foldDelay: Double

    static let standard = Pacing(stepDisplay: 0.35, stepSettle: 0.2, foldDelay: 1.6)
    static let immediate = Pacing(stepDisplay: 0, stepSettle: 0, foldDelay: 0)
}

/// Reads a fixed-size C character array (imported as a tuple) as UTF-8.
private func string<Tuple>(fromCArray tuple: Tuple) -> String {
    withUnsafeBytes(of: tuple) { bytes in
        String(decoding: bytes.prefix(while: { $0 != 0 }), as: UTF8.self)
    }
}

// MARK: - Bridge to the C core

/// Owns one oct_service. Lifecycle calls run on a private serial queue; the
/// core is internally synchronised, so queries may be made from any thread.
/// A live USB transport will attach to the same oct_service through
/// oct_service_set_transport without changing this interface.
final class CoreService: @unchecked Sendable {
    static let version = OCT_CORE_VERSION

    private let handle: OpaquePointer
    private let queue = DispatchQueue(label: "octopus.core.service")

    init() {
        var created: OpaquePointer?
        let status = oct_service_create(&created)
        guard status == OCT_OK, let created else {
            fatalError("The Octopus core could not be created (status \(status.rawValue)).")
        }
        handle = created
    }

    deinit {
        oct_service_destroy(handle)
    }

    func perform<Value: Sendable>(_ work: @escaping @Sendable (CoreService) -> Value) async -> Value {
        await withCheckedContinuation { continuation in
            queue.async {
                continuation.resume(returning: work(self))
            }
        }
    }

    func setIdentity(name: String, serial: String, system: String) {
        _ = oct_service_set_identity(handle, name, serial, system)
    }

    func start(_ configuration: CoreConfiguration) -> [CoreEvent] {
        var config = oct_service_config()
        config.seed_time = 0
        config.scratch = configuration.scratch
        config.self_test = configuration.selfTest
        _ = oct_service_start(handle, &config)
        return drainEvents()
    }

    func stop() -> [CoreEvent] {
        _ = oct_service_stop(handle)
        return drainEvents()
    }

    func selfTest() -> [CoreEvent] {
        _ = oct_service_self_test(handle)
        return drainEvents()
    }

    func state() -> ServiceState {
        ServiceState(oct_service_get_state(handle))
    }

    func lastError() -> String? {
        var buffer = [CChar](repeating: 0, count: 256)
        let recorded = buffer.withUnsafeMutableBufferPointer { pointer in
            oct_service_last_error(handle, pointer.baseAddress, pointer.count)
        }
        guard recorded else { return nil }
        return buffer.withUnsafeBufferPointer { pointer in String(cString: pointer.baseAddress!) }
    }

    func drainEvents() -> [CoreEvent] {
        var events: [CoreEvent] = []
        var buffer = [oct_event](repeating: oct_event(), count: 64)
        while true {
            let count = buffer.withUnsafeMutableBufferPointer { pointer in
                oct_service_drain_events(handle, pointer.baseAddress, pointer.count)
            }
            events.append(contentsOf: buffer.prefix(count).map(CoreEvent.init))
            if count < buffer.count { return events }
        }
    }

    func status() -> CoreStatus {
        var raw = oct_service_status()
        guard oct_service_get_status(handle, &raw) == OCT_OK else { return CoreStatus() }
        return CoreStatus(state: ServiceState(raw.state),
                          transportAvailable: raw.transport_available,
                          transportName: string(fromCArray: raw.transport_name),
                          sessionOpen: raw.session_open,
                          sessionID: raw.session_id,
                          storageID: raw.storage_id,
                          objectCount: raw.object_count,
                          operationCount: raw.operation_count,
                          transactions: raw.transactions,
                          scratch: raw.scratch,
                          starts: raw.starts,
                          stops: raw.stops,
                          transportState: TransportState(raw.transport_state),
                          transportFailure: string(fromCArray: raw.transport_failure),
                          transportTransactions: raw.transport_transactions)
    }

    func storage() -> StorageSnapshot? {
        var raw = oct_storage_info()
        guard oct_service_storage_info(handle, &raw) == OCT_OK else { return nil }
        return StorageSnapshot(name: string(fromCArray: raw.description),
                               capacity: raw.max_capacity,
                               free: raw.free_space,
                               readOnly: raw.access_capability != 0,
                               objectCount: Int(raw.object_count))
    }

    func children(of parent: UInt32) -> [StorageObject] {
        var total = 0
        var items = [oct_object_info](repeating: oct_object_info(), count: 32)
        for _ in 0..<2 {
            let status = items.withUnsafeMutableBufferPointer { pointer in
                oct_service_list(handle, parent, pointer.baseAddress, pointer.count, &total)
            }
            guard status == OCT_OK else { return [] }
            if total <= items.count { break }
            items = [oct_object_info](repeating: oct_object_info(), count: total)
        }
        return items.prefix(min(total, items.count)).map(StorageObject.init)
    }

    /// The whole hierarchy, depth first, parents before children.
    func tree(maximumDepth: Int = 16) -> [StorageEntry] {
        var entries: [StorageEntry] = []
        func walk(_ parent: UInt32, depth: Int) {
            for object in children(of: parent) {
                entries.append(StorageEntry(object: object, depth: depth))
                if object.isDirectory && depth < maximumDepth { walk(object.id, depth: depth + 1) }
            }
        }
        walk(0, depth: 0)
        return entries
    }

    func read(_ object: UInt32, length: Int) -> Data? {
        var data = Data(count: length)
        var count = 0
        let status = data.withUnsafeMutableBytes { buffer in
            oct_service_read(handle, object, 0, buffer.baseAddress, buffer.count, &count)
        }
        return status == OCT_OK ? data.prefix(count) : nil
    }
}

// MARK: - Controller

/// Connects the SwiftUI screens to the core: runs operations off the main
/// thread, publishes state, and drives the activity sheet from core events.
@MainActor
final class ServiceController: ObservableObject {
    @Published private(set) var state: ServiceState = .disabled
    /// The stages the core has reported since launch.
    @Published private(set) var observedStages: Set<ConnectionStage> = []
    @Published private(set) var isBusy = false
    @Published private(set) var lastError: String?
    @Published private(set) var status = CoreStatus()
    @Published private(set) var storage: StorageSnapshot?
    @Published private(set) var tree: [StorageEntry] = []
    @Published private(set) var log: [LogEntry] = []
    @Published private(set) var sheet = ActivitySheetState()

    let core: CoreService
    private let pacing: Pacing
    private var operation: Task<Void, Never>?
    private var lineCounter = 0

    init(core: CoreService = CoreService(), pacing: Pacing = .standard) {
        self.core = core
        self.pacing = pacing
        let device = UIDevice.current
        core.setIdentity(name: device.name,
                         serial: device.identifierForVendor?.uuidString ?? "unknown",
                         system: device.systemVersion)
        refresh()
    }

    var isActive: Bool { state == .ready || state == .waitingForConnection || state == .connected }
    var primaryTitle: String { isActive ? "Disable USB Access" : "Enable USB Access" }

    /// The header's second line becomes a warning when something needs attention.
    var headerWarning: String? {
        switch state {
        case .error:
            return lastError ?? "Octopus couldn't enable USB access."
        case .ready where !status.transportAvailable:
            return "No USB transport in this build yet, so Windows can't see Octopus."
        case .waitingForConnection where status.transportState == .error,
             .connected where status.transportState == .error:
            return status.transportFailure.isEmpty ? "The USB transport failed." : status.transportFailure
        default:
            return nil
        }
    }

    func toggle() {
        if isActive { disable() } else { enable() }
    }

    func enable() {
        DebugLog.log(.info, "app", "request", "Enable USB Access")
        let configuration = CoreConfiguration(
            scratch: Preferences.bool(Preferences.scratchFolder, default: false),
            selfTest: Preferences.bool(Preferences.selfTestOnEnable, default: true))
        run(title: "Enabling") { core in core.start(configuration) }
    }

    func disable() {
        DebugLog.log(.info, "app", "request", "Disable USB Access")
        run(title: "Disabling") { core in core.stop() }
    }

    func runSelfTest() {
        DebugLog.log(.info, "app", "request", "Run Self-Test")
        run(title: "Testing") { core in core.selfTest() }
    }

    /// Awaits the operation in progress, if any.
    func waitUntilIdle() async {
        await operation?.value
    }

    /// Re-reads the core's authoritative state and storage.
    func refresh() {
        status = core.status()
        state = status.state
        let seen = observedStages.union([ConnectionStage(service: state), ConnectionStage(transport: status.transportState)])
        if seen != observedStages { observedStages = seen }
        lastError = core.lastError()
        storage = core.storage()
        tree = storage == nil ? [] : core.tree()
        if !isBusy {
            for event in core.drainEvents() where event.kind == .log { append(event) }
        }
    }

    /// Up to the first 512 bytes of a file: text when it decodes as UTF-8, otherwise hex.
    func preview(of object: StorageObject) -> String {
        guard let data = core.read(object.id, length: 512) else { return "Unavailable while USB access is disabled." }
        if data.isEmpty { return "(empty)" }
        let more = UInt64(data.count) < object.size ? "\n\u{2026}" : ""
        if let text = String(data: data, encoding: .utf8) { return text + more }
        return Self.hexDump(data) + more
    }

    private func run(title: String, _ work: @escaping @Sendable (CoreService) -> [CoreEvent]) {
        guard !isBusy else { return }
        isBusy = true
        withAnimation(Motion.sheet) {
            sheet = ActivitySheetState(isPresented: true, title: title, lines: [])
        }
        let core = self.core
        operation = Task { [weak self] in
            let events = await core.perform(work)
            await self?.play(events)
            self?.finish()
        }
    }

    /// Replays the core's events. The core performs each operation
    /// synchronously and reports every step; the sheet only paces their
    /// display so each line can be read. Every line is a step the core ran.
    private func play(_ events: [CoreEvent]) async {
        var lineForStep: [UInt32: Int] = [:]
        for event in events {
            switch event.kind {
            case .state:
                state = event.state
            case .operationBegin:
                break
            case .stepBegin:
                let id = nextLineID()
                lineForStep[event.step] = id
                withAnimation(Motion.lineAppear) {
                    sheet.lines.append(SheetLine(id: id, text: event.text, status: .running))
                }
                await pause(pacing.stepDisplay)
            case .stepEnd:
                if let id = lineForStep[event.step], let index = sheet.lines.firstIndex(where: { $0.id == id }) {
                    withAnimation(Motion.lineFinish) {
                        sheet.lines[index].text = event.text
                        sheet.lines[index].status = event.failed ? .failed : .done
                    }
                    Haptics.light()
                }
                await pause(pacing.stepSettle)
            case .operationEnd:
                let id = nextLineID()
                withAnimation(Motion.lineAppear) {
                    sheet.lines.append(SheetLine(id: id, text: event.text,
                                                 status: event.failed ? .failedSummary : .summary))
                    sheet.title = event.failed ? "Failed" : "Done"
                }
                if event.failed { Haptics.failure() } else { Haptics.success() }
                await pause(pacing.foldDelay)
            case .log:
                append(event)
            }
        }
    }

    private func finish() {
        refresh()
        withAnimation(Motion.sheet) { sheet.isPresented = false }
        isBusy = false
    }

    private func pause(_ seconds: Double) async {
        guard seconds > 0 else { return }
        try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
    }

    private func nextLineID() -> Int {
        lineCounter += 1
        return lineCounter
    }

    private func append(_ event: CoreEvent) {
        log.append(LogEntry(id: event.sequence, date: event.date, text: event.text))
        if log.count > 500 { log.removeFirst(log.count - 500) }
    }

    private static func hexDump(_ data: Data) -> String {
        let bytes = [UInt8](data)
        return stride(from: 0, to: bytes.count, by: 8).map { offset in
            let row = bytes[offset..<min(offset + 8, bytes.count)].map { String(format: "%02X", $0) }
            return String(format: "%04lX  ", offset) + row.joined(separator: " ")
        }.joined(separator: "\n")
    }
}
