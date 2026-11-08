import OctopusCore
import XCTest
@testable import Octopus

extension DiagnosticsEnvironment {
    static let sample = DiagnosticsEnvironment(serviceState: "Disabled", transportState: "Transport unavailable",
                                               transportFailure: "", transportTransactions: 0,
                                               settings: ["haptics": "default"])
}

@MainActor
final class DiagnosticsTests: XCTestCase {
    private let containerPath = "/var/mobile/Containers/Data/Application/0A1B2C3D-4E5F-6071-8293-A4B5C6D7E8F9/Library/Logs"

    /// The log in a folder of this test's own, whatever earlier tests left it in.
    private func startLog() {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("diagnostics-tests", isDirectory: true)
        DebugLog.start(in: [folder], detailed: false)
    }

    func testTheCoreChecksPassAndSayWhichAreSimulated() {
        let core = CoreDiagnostics()
        XCTAssertEqual(core.count, 17)
        for index in 0..<core.count {
            let result = core.run(index)
            XCTAssertEqual(result.status, .pass, "\(result.id): \(result.detail)")
            XCTAssertEqual(result.kind, result.id.hasPrefix("adapter.") ? .simulated : .local, result.id)
            XCTAssertGreaterThan(result.checks, 0, result.id)
        }
    }

    func testAFullRunReportsEveryCheckAndClaimsNoConnection() async {
        startLog()
        let runner = DiagnosticsRunner()
        await runner.run(environment: .sample, includeNetwork: false)
        XCTAssertFalse(runner.running)
        XCTAssertGreaterThan(runner.planned, 20)
        XCTAssertEqual(runner.results.count, runner.planned)
        XCTAssertEqual(Set(runner.results.map(\.id)).count, runner.results.count)
        let failed = runner.results.filter { $0.status == .fail }
        XCTAssertTrue(failed.isEmpty, failed.map { "\($0.id): \($0.detail)" }.joined(separator: "\n"))
        let external = runner.results.filter { $0.kind == .external }
        XCTAssertEqual(external.map(\.id), ["external.usb_transport", "external.host_requests", "external.windows"])
        XCTAssertTrue(external.allSatisfy { $0.status == .blocked }, "nothing external is reachable in the simulator")
        XCTAssertEqual(runner.results.first { $0.id == "adapter.mtp_enumeration" }?.status, .pass)
        XCTAssertEqual(runner.results.first { $0.id == "app.debug_log" }?.status, .pass)
        XCTAssertEqual(runner.results.first { $0.id == "updates.helper_root" }?.status, .skipped)
    }

    func testTheArchiveHoldsResultsLogsBuildAndConnectivity() throws {
        startLog()
        DebugLog.log(.warning, "test", "warning", "a warning for failures.txt")
        let results = [DiagnosticResult(id: "app.debug_log", group: "Application", title: "Persistent debug log",
                                        status: .fail, kind: .local, checks: 1, durationMs: 1.5,
                                        detail: "Refused: \(containerPath)")]
        let url = try Diagnostics.makeArchive(results: results, environment: .sample, started: Date(), finished: Date())
        defer { try? FileManager.default.removeItem(at: url) }
        let entries = try zipEntries(url)
        for name in ["README.txt", "results.json", "results.txt", "failures.txt", "build.json", "settings.json",
                     "connectivity.txt"] {
            XCTAssertNotNil(entries[name], name)
        }
        XCTAssertTrue(entries.keys.contains { $0.hasPrefix("logs/octopus-debug") })
        let report = try JSONSerialization.jsonObject(with: try XCTUnwrap(entries["results.json"])) as? [String: Any]
        XCTAssertEqual(report?["schema"] as? String, "octopus-diagnostics/1")
        XCTAssertEqual((report?["summary"] as? [String: Int])?["FAIL"], 1)
        let first = (report?["results"] as? [[String: Any]])?.first
        XCTAssertEqual(first?["status"] as? String, "FAIL")
        XCTAssertEqual(first?["kind"] as? String, "local")
        XCTAssertEqual(first?["duration_ms"] as? Double, 1.5)
        let detail = first?["detail"] as? String ?? ""
        XCTAssertTrue(detail.contains("/Application/<app-container>/Library/Logs"), detail)
        XCTAssertFalse(detail.contains("0A1B2C3D"))
        let failures = String(decoding: entries["failures.txt"] ?? Data(), as: UTF8.self)
        XCTAssertTrue(failures.contains("[FAIL] app.debug_log"))
        XCTAssertTrue(failures.contains("a warning for failures.txt"))
        let build = try JSONSerialization.jsonObject(with: try XCTUnwrap(entries["build.json"])) as? [String: Any]
        XCTAssertEqual(build?["core"] as? String, OCT_CORE_VERSION)
    }

    func testRedactionKeepsPathsButNotContainerIDs() {
        XCTAssertEqual(Diagnostics.redact("\(containerPath): Opening the log"),
                       "/var/mobile/Containers/Data/Application/<app-container>/Library/Logs: Opening the log")
        XCTAssertEqual(Diagnostics.redact("/var/containers/Bundle/Application/0A1B2C3D-4E5F-6071-8293-A4B5C6D7E8F9/Octopus.app"),
                       "/var/containers/Bundle/Application/<app-container>/Octopus.app")
        XCTAssertEqual(Diagnostics.redact("/var/mobile/Library/Caches/Octopus"), "/var/mobile/Library/Caches/Octopus")
    }

    func testStatusesMapFromTheCore() {
        XCTAssertEqual(DiagnosticStatus(OCT_DIAG_PASS), .pass)
        XCTAssertEqual(DiagnosticStatus(OCT_DIAG_FAIL), .fail)
        XCTAssertEqual(DiagnosticStatus(OCT_DIAG_SKIPPED), .skipped)
        XCTAssertEqual(DiagnosticStatus(OCT_DIAG_BLOCKED), .blocked)
        XCTAssertEqual(DiagnosticKind(OCT_DIAG_SIMULATED), .simulated)
        XCTAssertEqual(DiagnosticKind(OCT_DIAG_EXTERNAL), .external)
    }

    /// The entries of a stored ZIP, read from its central directory.
    private func zipEntries(_ url: URL) throws -> [String: Data] {
        let bytes = [UInt8](try Data(contentsOf: url))
        func u16(_ i: Int) -> Int { Int(bytes[i]) | Int(bytes[i + 1]) << 8 }
        func u32(_ i: Int) -> Int { u16(i) | u16(i + 2) << 16 }
        let end = bytes.count - 22
        XCTAssertEqual(u32(end), 0x06054b50)
        var offset = u32(end + 16)
        var entries: [String: Data] = [:]
        for _ in 0..<u16(end + 10) {
            XCTAssertEqual(u32(offset), 0x02014b50)
            let size = u32(offset + 20), nameLength = u16(offset + 28)
            let extra = u16(offset + 30), comment = u16(offset + 32), local = u32(offset + 42)
            let name = String(decoding: bytes[(offset + 46)..<(offset + 46 + nameLength)], as: UTF8.self)
            let start = local + 30 + u16(local + 26) + u16(local + 28)
            entries[name] = Data(bytes[start..<(start + size)])
            offset += 46 + nameLength + extra + comment
        }
        return entries
    }
}
