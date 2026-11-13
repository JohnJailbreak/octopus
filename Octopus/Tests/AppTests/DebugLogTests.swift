import OctopusCore
import XCTest
@testable import Octopus

/// The Swift side of the debug log, against the real C logger: the line format, persistence across a relaunch,
/// levels, clearing and exporting; the device report's flattening; and the connection stages the USB Connection
/// window shows.
final class DebugLogTests: XCTestCase {
    private var folder: URL!

    override func setUp() {
        super.setUp()
        folder = FileManager.default.temporaryDirectory.appendingPathComponent("octopus-log-\(UUID().uuidString)")
        XCTAssertNotNil(DebugLog.start(in: [folder], detailed: false))
    }

    override func tearDown() {
        DebugLog.start() // back to the app's own log
        try? FileManager.default.removeItem(at: folder)
        super.tearDown()
    }

    func testParsesTheCoreLineFormat() throws {
        let line = "2026-11-08T19:51:02.123Z INFO  link       transaction op=0x1007 GetObjectHandles tid=4 rc=0x2001 OK"
        let entry = try XCTUnwrap(DebugLog.parse(line, id: 7))
        XCTAssertEqual(entry.id, 7)
        XCTAssertEqual(entry.time, "2026-11-08T19:51:02.123Z")
        XCTAssertEqual(entry.level, .info)
        XCTAssertEqual(entry.component, "link")
        XCTAssertEqual(entry.event, "transaction")
        XCTAssertEqual(entry.message, "op=0x1007 GetObjectHandles tid=4 rc=0x2001 OK")
        XCTAssertEqual(entry.line, line)
        let bare = try XCTUnwrap(DebugLog.parse("2026-11-08T19:51:02.123Z WARN  app        phase", id: 1))
        XCTAssertEqual(bare.message, "")
        XCTAssertNil(DebugLog.parse("not a log line", id: 0))
        XCTAssertNil(DebugLog.parse("2026-11-08T19:51:02.123Z LOUD  app        phase x", id: 0))
    }

    func testWritesPersistsAcrossLaunchesAndFilters() {
        DebugLog.log(.debug, "test", "hidden", "below the level")
        DebugLog.log(.warning, "test", "warned", "something to look at")
        DebugLog.log(.error, "test", "failed", "the reason")
        let entries = DebugLog.entries()
        XCTAssertTrue(entries.contains { $0.event == "warned" && $0.level == .warning })
        XCTAssertFalse(entries.contains { $0.event == "hidden" })
        XCTAssertEqual(DebugLog.lastFailure(in: entries)?.event, "failed")
        XCTAssertEqual(entries.filter { $0.level >= .error }.map(\.event), ["failed"])

        DebugLog.start(in: [folder], detailed: true) // the next launch, with detailed logging
        DebugLog.log(.debug, "test", "detailed", "kept with detailed logging")
        let again = DebugLog.entries()
        XCTAssertTrue(again.contains { $0.event == "warned" })
        XCTAssertTrue(again.contains { $0.event == "detailed" })
        XCTAssertEqual(again.filter { $0.event == "launch" }.count, 2)
        XCTAssertEqual(DebugLog.files().map(\.lastPathComponent), ["debug.log"])
        XCTAssertTrue(DebugLog.stats().fileOpen)
    }

    func testClearAndExport() throws {
        DebugLog.log(.info, "test", "before", "to be cleared")
        DebugLog.clear()
        let entries = DebugLog.entries()
        XCTAssertFalse(entries.contains { $0.event == "before" })
        XCTAssertTrue(entries.contains { $0.event == "cleared" })
        DebugLog.log(.info, "test", "after", "exported")
        let url = try DebugLog.exportFile()
        defer { try? FileManager.default.removeItem(at: url) }
        let text = try String(contentsOf: url, encoding: .utf8)
        XCTAssertTrue(text.hasPrefix("# Octopus "))
        XCTAssertTrue(text.contains("test       after exported"))
        XCTAssertTrue(url.lastPathComponent.hasPrefix("debug-"))
    }

    func testTheSelfTestRehearsalIsNeverTheLastFailure() {
        let rehearsal = DebugLog.Entry(id: 1, time: "2026-11-08T19:51:02.123Z", level: .warning, component: "selftest",
                                       event: "timeout", message: "deliberate", line: "")
        let real = DebugLog.Entry(id: 0, time: "2026-11-08T19:51:01.000Z", level: .error, component: "link",
                                  event: "send_failed", message: "real", line: "")
        XCTAssertNil(DebugLog.lastFailure(in: [rehearsal]))
        XCTAssertEqual(DebugLog.lastFailure(in: [real, rehearsal])?.event, "send_failed")
    }

    func testDeviceReportFlattensAndRedacts() {
        let plist: [String: Any] = [
            "Configurations": [["Interfaces": [["Class": 6, "SubClass": 1, "Name": "PTP"]]]],
            "SerialNumber": "ABC123",
            "Enabled": true,
            "One": 1,
            "Codes": [0x1001, 0x1002],
        ]
        var lines: [String] = []
        DeviceReport.flatten(plist, path: "", depth: 0, into: &lines)
        XCTAssertTrue(lines.contains("Configurations[0].Interfaces[0].Class = 6"))
        XCTAssertTrue(lines.contains("Configurations[0].Interfaces[0].Name = \"PTP\""))
        XCTAssertTrue(lines.contains("SerialNumber = <redacted>"))
        XCTAssertTrue(lines.contains("Enabled = true"))
        XCTAssertTrue(lines.contains("One = 1"))          // an integer 1 is not a boolean
        XCTAssertTrue(lines.contains("Codes = [4097 (0x1001), 4098 (0x1002)] (2)"))
        XCTAssertFalse(lines.joined().contains("ABC123"))
    }

    func testConnectionStagesFollowTheCore() {
        XCTAssertEqual(ConnectionStage(service: .disabled), .serviceDisabled)
        XCTAssertEqual(ConnectionStage(service: .starting), .serviceInitializing)
        XCTAssertEqual(ConnectionStage(service: .ready), .responderReady)
        XCTAssertEqual(ConnectionStage(transport: TransportState(OCT_TRANSPORT_UNAVAILABLE)), .transportUnavailable)
        XCTAssertEqual(ConnectionStage(transport: TransportState(OCT_TRANSPORT_SESSION_ACTIVE)), .sessionActive)
        XCTAssertEqual(ConnectionStage(transport: TransportState(OCT_TRANSPORT_DISCONNECTED)), .disconnected)
        XCTAssertFalse(ConnectionStage.transportUnavailable.needsTransport)
        XCTAssertTrue(ConnectionStage.waiting.needsTransport)
        XCTAssertEqual(ConnectionStage.service.count + ConnectionStage.transport.count, ConnectionStage.allCases.count)
    }
}
