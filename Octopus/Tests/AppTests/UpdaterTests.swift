import SwiftUI
import XCTest
@testable import Octopus

/// The updater's decisions, from GitHub's own JSON: which releases count, which one is offered, how the changelog is
/// ordered and how failures read. The network and TrollStore aren't involved.
final class UpdaterTests: XCTestCase {
    private func release(_ version: String, app: Bool = true) -> OctopusRelease {
        OctopusRelease(id: version.hashValue, name: version, body: "- Notes for \(version)",
                       version: Updater.versionParts(version),
                       tipaURL: app ? URL(string: "https://github.com/m9z2w4d8vw-blip/octopus/releases/download/v\(version)/Octopus.tipa") : nil)
    }

    func testVersionParts() {
        XCTAssertEqual(Updater.versionParts("v0.2.57"), [0, 2, 57])
        XCTAssertEqual(Updater.versionParts("0.2"), [0, 2])
        XCTAssertEqual(Updater.versionParts("Octopus 1.10.3"), [1, 10, 3])
        XCTAssertEqual(Updater.versionParts("nightly"), [])
    }

    func testVersionsCompareNumerically() {
        XCTAssertEqual(Updater.compare([0, 2], [0, 2, 0]), 0)
        XCTAssertEqual(Updater.compare([0, 10], [0, 9, 99]), 1)
        XCTAssertEqual(Updater.compare([0, 2, 9], [0, 2, 10]), -1)
        XCTAssertEqual(Updater.compare([0, 2, 1], [0, 1, 300]), 1)   // a new series beats any run of the old one
    }

    func testParseKeepsPublishedReleasesAndFindsTheApp() throws {
        let json = """
        [
          {"id": 4, "tag_name": "v0.2.12", "name": "0.2.12", "body": "- Draft", "draft": true, "prerelease": false,
           "assets": [{"name": "Octopus.tipa", "browser_download_url": "https://example.com/draft.tipa"}]},
          {"id": 3, "tag_name": "v0.2.11", "name": "0.2.11", "body": "- Beta", "draft": false, "prerelease": true,
           "assets": [{"name": "Octopus.tipa", "browser_download_url": "https://example.com/beta.tipa"}]},
          {"id": 2, "tag_name": "v0.2.10", "name": "0.2.10", "body": "- Slate", "draft": false, "prerelease": false,
           "assets": [{"name": "SHA256SUMS", "browser_download_url": "https://example.com/SHA256SUMS"},
                      {"name": "Octopus.ipa", "browser_download_url": "https://example.com/Octopus.ipa"},
                      {"name": "Octopus.tipa", "browser_download_url": "https://example.com/Octopus.tipa"}]},
          {"id": 1, "tag_name": "v0.2.9", "name": "", "body": null, "draft": false, "prerelease": false,
           "assets": [{"name": "Octopus.ipa", "browser_download_url": "https://example.com/old.ipa"}]}
        ]
        """
        let releases = try XCTUnwrap(Updater.parse(Data(json.utf8)))
        XCTAssertEqual(releases.map(\.id), [2, 1])
        XCTAssertEqual(releases[0].version, [0, 2, 10])
        XCTAssertEqual(releases[0].tipaURL?.absoluteString, "https://example.com/Octopus.tipa")   // .tipa before .ipa
        XCTAssertEqual(releases[1].name, "v0.2.9")   // an empty title falls back to the tag
        XCTAssertEqual(releases[1].body, "")
        XCTAssertEqual(releases[1].tipaURL?.absoluteString, "https://example.com/old.ipa")
        XCTAssertNil(Updater.parse(Data("{\"message\": \"Not Found\"}".utf8)))
    }

    func testOnlyNewerReleasesAreOfferedNewestFirst() {
        let releases = [release("0.2.9"), release("0.2.10"), release("0.2.3")]
        let behind = Updater.select(from: releases, app: [0, 2, 5])
        XCTAssertEqual(behind.available?.name, "0.2.10")
        XCTAssertEqual(behind.updates.map(\.name), ["0.2.10", "0.2.9"])
        let current = Updater.select(from: releases, app: [0, 2, 10])
        XCTAssertNil(current.available)
        XCTAssertTrue(current.updates.isEmpty)
    }

    func testAReleaseWithoutTheAppIsInTheChangelogButNotOffered() {
        let chosen = Updater.select(from: [release("0.2.11", app: false), release("0.2.10")], app: [0, 2, 9])
        XCTAssertEqual(chosen.available?.name, "0.2.10")
        XCTAssertEqual(chosen.updates.map(\.name), ["0.2.11", "0.2.10"])
    }

    func testFailureReasons() throws {
        let url = Updater.apiURL
        let limited = try XCTUnwrap(HTTPURLResponse(url: url, statusCode: 403, httpVersion: nil,
                                                    headerFields: ["X-RateLimit-Remaining": "0"]))
        XCTAssertEqual(Updater.describe(limited), "GitHub's hourly limit is used up")
        let reset = try XCTUnwrap(HTTPURLResponse(url: url, statusCode: 403, httpVersion: nil,
                                                  headerFields: ["X-RateLimit-Remaining": "0",
                                                                 "X-RateLimit-Reset": "1767225600"]))
        XCTAssertTrue(Updater.describe(reset).hasPrefix("GitHub's hourly limit is used up until "))
        let missing = try XCTUnwrap(HTTPURLResponse(url: url, statusCode: 404, httpVersion: nil, headerFields: nil))
        XCTAssertEqual(Updater.describe(missing), "GitHub answered 404")
        XCTAssertEqual(Updater.describe(URLError(.notConnectedToInternet)), "you're offline")
        XCTAssertEqual(Updater.describe(URLError(.timedOut)), "GitHub didn't answer")
    }

    func testHelperErrorsReadAsSentences() {
        let outcome = RootHelper.Outcome(status: 5, lines: ["LOG\tInstalling the update with TrollStore",
                                                            "ERR\tTrollStore's installer wasn't found. Is TrollStore installed?"])
        XCTAssertEqual(outcome.errors, ["TrollStore's installer wasn't found. Is TrollStore installed?"])
        XCTAssertEqual(Updater.sentence("Is TrollStore installed?"), "Is TrollStore installed?")
        XCTAssertEqual(Updater.sentence("The update helper stopped with code 3"), "The update helper stopped with code 3.")
    }

    func testAMissingHelperFailsCleanly() {
        // A simulator build carries no helper, so starting it fails with a reason instead of crashing.
        let outcome = RootHelper.run(Bundle.main.bundlePath + "/octopushelper", ["version"])
        XCTAssertEqual(outcome.status, -1)
        XCTAssertFalse(outcome.errors.isEmpty)
    }

    func testUpdateAvailableSitsBetweenTheMenuAndTheButton() {
        let layouts = [
            Layout(size: CGSize(width: 390, height: 844), insets: EdgeInsets(top: 47, leading: 0, bottom: 34, trailing: 0)),
            Layout(size: CGSize(width: 375, height: 667), insets: EdgeInsets(top: 20, leading: 0, bottom: 0, trailing: 0)),
            Layout(size: CGSize(width: 320, height: 568), insets: EdgeInsets()),
        ]
        for layout in layouts {
            XCTAssertEqual(layout.updateLinkTop + layout.updateLinkHeight,
                           layout.buttonTop - (layout.hasHomeButton ? 10 : 20), accuracy: 0.01)
            XCTAssertGreaterThanOrEqual(layout.updateLinkTop, layout.menuTop + layout.menuHeight)
        }
        let phone = layouts[0]
        XCTAssertEqual(phone.updateLinkTop, 647.1, accuracy: 0.1)
    }
}
