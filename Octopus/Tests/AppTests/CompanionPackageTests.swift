import XCTest
@testable import Octopus

/// The companion's installed version, read from the text of a dpkg status file.
///
/// It decides whether Octopus offers to install its companion, so getting it wrong in either
/// direction is visible: reporting a version that is not installed hides the button needed to put
/// the companion back, and missing one that is installed offers an install that is not needed.
final class CompanionPackageTests: XCTestCase {
    private let installed = """
        Package: com.opa334.dopamine
        Status: install ok installed
        Version: 2.1

        Package: com.parv.octopus.native
        Status: install ok installed
        Architecture: iphoneos-arm64
        Version: 0.4.52

        Package: ellekit
        Status: install ok installed
        Version: 1.2

        """

    func testFindsTheInstalledCompanionAmongOtherPackages() {
        XCTAssertEqual(CompanionPackage.version(in: installed), "0.4.52")
    }

    /// dpkg keeps a stanza, version and all, for a package removed with its configuration left
    /// behind. Reading the version alone reported a removed companion as installed.
    func testARemovedCompanionIsNotInstalled() {
        let removed = installed.replacingOccurrences(
            of: "Package: com.parv.octopus.native\nStatus: install ok installed",
            with: "Package: com.parv.octopus.native\nStatus: deinstall ok config-files")
        XCTAssertNotEqual(removed, installed)   // the substitution really happened
        XCTAssertNil(CompanionPackage.version(in: removed))
    }

    func testAbsentLookalikeAndEmpty() {
        XCTAssertNil(CompanionPackage.version(in: "Package: ellekit\nStatus: install ok installed\nVersion: 1.2\n"))
        // A different package whose name starts the same way is not the companion.
        XCTAssertNil(CompanionPackage.version(
            in: "Package: com.parv.octopus.native-extra\nStatus: install ok installed\nVersion: 9.9\n"))
        XCTAssertNil(CompanionPackage.version(in: ""))
    }

    /// The order of fields within a stanza is dpkg's business, not ours.
    func testFieldOrderDoesNotMatter() {
        let reordered = "Version: 0.4.60\nStatus: install ok installed\nPackage: com.parv.octopus.native\n"
        XCTAssertEqual(CompanionPackage.version(in: reordered), "0.4.60")
    }
}
