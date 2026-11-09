import Foundation
import XCTest
@testable import Octopus

final class NativeIntegrationTests: XCTestCase {
    private func capture(_ changes: [String: Any] = [:]) throws -> NativeCapture {
        var object: [String: Any] = [
            "schema": "octopus-native/1", "pid": 123, "uid": 501, "generation": 7,
            "generated_at": 100.0, "lease": "0123456789abcdef0123456789abcdef",
            "authorized": true, "expires_at_ms": 16000, "provider_eligible": true,
            "compatibility": "matched", "hooks": "Installed", "provider": "ABI matched",
            "lease_commands": 4, "lease_paired_responses": 3, "lease_provider_events": 0,
            "last_provider_event": "none"
        ]
        object.merge(changes) { _, new in new }
        return NativeCapture(data: try JSONSerialization.data(withJSONObject: object), problem: "",
                             expectedLease: "0123456789abcdef0123456789abcdef", observedPID: 123,
                             capturedAt: 101, capturedMonotonicMs: 2000, appUID: 501)
    }

    func testFreshNativeEvidenceIsDistinctFromSimulation() throws {
        let sample = try capture()
        XCTAssertNil(sample.validationProblem)
        XCTAssertEqual(sample.result(transactions: false).kind, .nativeObservation)
        XCTAssertEqual(sample.result(transactions: true).kind, .realUSB)
        XCTAssertEqual(sample.result(transactions: true).status, .pass)
        XCTAssertTrue(sample.result(transactions: true).detail.contains("still need Windows verification"))
    }

    func testStaleUnconsentedOrWrongProcessEvidenceCannotPass() throws {
        let cases: [[String: Any]] = [
            ["generated_at": 95.0], ["generated_at": 103.0], ["pid": 124], ["uid": 0],
            ["generation": 0], ["lease": "previous"], ["authorized": false],
            ["expires_at_ms": 2000], ["expires_at_ms": 17001], ["hooks": "ABI mismatch"],
            ["schema": "other/1"], ["lease_paired_responses": 5]
        ]
        for changes in cases {
            let sample = try capture(changes)
            XCTAssertNotNil(sample.validationProblem, "\(changes)")
            XCTAssertEqual(sample.result(transactions: true).status, .blocked)
            XCTAssertNotEqual(sample.result(transactions: true).kind, .realUSB)
        }
    }

    func testSuccessfulLoadWithoutUSBTransactionsStaysBlocked() throws {
        let sample = try capture(["lease_commands": 0, "lease_paired_responses": 0])
        XCTAssertEqual(sample.result(transactions: false).status, .pass)
        XCTAssertEqual(sample.result(transactions: true).status, .blocked)
        XCTAssertEqual(sample.result(transactions: true).kind, .nativeObservation)
    }

    func testMissingAndMalformedReportsAreBlocked() throws {
        var sample = try capture()
        sample.data = Data("{}".utf8)
        XCTAssertNotNil(sample.validationProblem)
        sample.data = nil; sample.problem = "Permission denied"
        XCTAssertEqual(sample.validationProblem, "Permission denied")
    }

    func testExportEnvironmentKeepsTheSameImmutableCapture() throws {
        var environment = DiagnosticsEnvironment.unknown
        environment.nativeCapture = try capture()
        let captured = Diagnostics.nativeCapture(environment)
        XCTAssertEqual(captured.data, environment.nativeCapture?.data)
        XCTAssertEqual(captured.result(transactions: true).status, .pass)
    }
}
