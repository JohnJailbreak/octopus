import SwiftUI
import XCTest
@testable import Octopus

/// Exercises the real C core through the Swift bridge and the controller's
/// state machine. Nothing here is mocked.
@MainActor
final class ServiceControllerTests: XCTestCase {
    func testBridgeLifecycleAgainstTheCore() async {
        let core = CoreService()
        core.setIdentity(name: "Test iPhone", serial: "TEST-0001", system: "17.0")
        XCTAssertEqual(core.state(), .disabled)
        let started = await core.perform { $0.start(CoreConfiguration(scratch: false, selfTest: true)) }
        XCTAssertEqual(core.state(), .ready)
        XCTAssertEqual(started.first?.kind, .operationBegin)
        XCTAssertTrue(started.contains { $0.kind == .stepEnd && $0.failed && $0.text == "No USB transport in this build" })
        XCTAssertTrue(started.contains { $0.kind == .stepEnd && $0.text.hasPrefix("Self-test passed") })
        XCTAssertTrue(started.contains { $0.kind == .operationEnd && !$0.failed })
        XCTAssertFalse(started.contains { $0.state == .waitingForConnection || $0.state == .connected })
        let tree = core.tree()
        XCTAssertEqual(tree.map(\.object.name),
                       ["README.txt", "Documents", "example.txt", "sample.bin", "Information", "version.txt"])
        XCTAssertEqual(tree.map(\.depth), [0, 0, 1, 1, 0, 1])
        let readme = tree[0].object
        XCTAssertTrue(readme.writeProtected)
        XCTAssertEqual(core.read(readme.id, length: 25).flatMap { String(data: $0, encoding: .utf8) },
                       "Octopus synthetic storage")
        let sample = tree[3].object
        XCTAssertEqual(sample.size, 4096)
        XCTAssertEqual(core.read(sample.id, length: 8).map { [UInt8]($0) }, [0, 1, 2, 3, 4, 5, 6, 7])
        XCTAssertEqual(core.storage()?.readOnly, true)
        XCTAssertEqual(core.status().operationCount, 20)
        _ = await core.perform { $0.stop() }
        XCTAssertEqual(core.state(), .disabled)
        XCTAssertNil(core.storage())
        XCTAssertNil(core.read(readme.id, length: 4))
    }

    func testRepeatedStartsAndStops() async {
        let core = CoreService()
        for _ in 0..<25 {
            _ = await core.perform { $0.start(CoreConfiguration(scratch: true, selfTest: false)) }
            XCTAssertEqual(core.state(), .ready)
            XCTAssertEqual(core.tree().count, 7)
            _ = await core.perform { $0.stop() }
            XCTAssertEqual(core.state(), .disabled)
        }
        let again = await core.perform { $0.stop() }
        XCTAssertTrue(again.isEmpty) // stopping a disabled service changes nothing
    }

    func testControllerStateTransitions() async {
        let controller = ServiceController(core: CoreService(), pacing: .immediate)
        XCTAssertEqual(controller.state, .disabled)
        XCTAssertEqual(controller.primaryTitle, "Enable USB Access")
        XCTAssertNil(controller.headerWarning)
        controller.enable()
        XCTAssertTrue(controller.isBusy)
        XCTAssertTrue(controller.sheet.isPresented)
        XCTAssertEqual(controller.sheet.title, "Enabling")
        await controller.waitUntilIdle()
        XCTAssertEqual(controller.state, .ready)
        XCTAssertTrue(controller.isActive)
        XCTAssertFalse(controller.isBusy)
        XCTAssertFalse(controller.sheet.isPresented)
        XCTAssertEqual(controller.sheet.title, "Done")
        XCTAssertEqual(controller.sheet.lines.last?.status, .summary)
        XCTAssertEqual(controller.primaryTitle, "Disable USB Access")
        XCTAssertNotNil(controller.headerWarning) // no USB transport is reported, not hidden
        XCTAssertEqual(controller.tree.count, 6)
        XCTAssertEqual(controller.storage?.name, "Octopus")
        controller.toggle()
        await controller.waitUntilIdle()
        XCTAssertEqual(controller.state, .disabled)
        XCTAssertNil(controller.storage)
        XCTAssertTrue(controller.tree.isEmpty)
        XCTAssertTrue(controller.log.contains { $0.text == "USB access disabled" })
    }

    func testSelfTestFromControllerLeavesServiceDisabled() async {
        let controller = ServiceController(core: CoreService(), pacing: .immediate)
        controller.runSelfTest()
        await controller.waitUntilIdle()
        XCTAssertEqual(controller.state, .disabled)
        XCTAssertTrue(controller.sheet.lines.contains { $0.text.hasPrefix("Self-test passed") })
        XCTAssertTrue(controller.sheet.lines.contains { $0.text == "Scratch: created, read, renamed and deleted" })
        XCTAssertFalse(controller.sheet.lines.contains { $0.status == .failed || $0.status == .failedSummary })
    }

    func testRequestsWhileBusyAreIgnored() async {
        let controller = ServiceController(core: CoreService(), pacing: .immediate)
        controller.enable()
        controller.enable()
        controller.disable()
        await controller.waitUntilIdle()
        XCTAssertEqual(controller.state, .ready)
        XCTAssertEqual(controller.core.status().starts, 1)
    }

    func testLayoutMatchesSpecificationGeometry() {
        let phone = Layout(size: CGSize(width: 390, height: 844),
                           insets: EdgeInsets(top: 47, leading: 0, bottom: 34, trailing: 0))
        XCTAssertFalse(phone.hasHomeButton)
        XCTAssertEqual(phone.rowHeight, 73)
        XCTAssertEqual(phone.columnWidth, 330)
        XCTAssertEqual(phone.menuTop, 306.7, accuracy: 0.1)
        XCTAssertEqual(phone.buttonTop, 697.1, accuracy: 0.1)
        XCTAssertEqual(phone.sheetTop, 254.4, accuracy: 0.1)
        XCTAssertEqual(phone.windowSize.height, 590.8, accuracy: 0.1)
        let homeButton = Layout(size: CGSize(width: 375, height: 667),
                                insets: EdgeInsets(top: 20, leading: 0, bottom: 0, trailing: 0))
        XCTAssertTrue(homeButton.hasHomeButton)
        XCTAssertEqual(homeButton.rowHeight, 65)
        XCTAssertEqual(homeButton.logBottomInset, 10)
        XCTAssertGreaterThanOrEqual(homeButton.gap, 12)
        let short = Layout(size: CGSize(width: 320, height: 568), insets: EdgeInsets())
        XCTAssertEqual(short.rowHeight, 52)
        XCTAssertEqual(short.menuSidePadding, 10)
        XCTAssertEqual(short.windowSize.height, 568 * 0.8, accuracy: 0.01)
    }
}
