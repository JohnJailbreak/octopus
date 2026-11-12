import Foundation
import Combine
import OctopusCore
import OctopusSpawn

/// What can be known about the companion without changing anything: whether this app carries a
/// package, which version, and which version is installed.
///
/// A plain enum rather than part of the installer, so it is not tied to the main actor and the
/// diagnostics can ask from wherever they run.
enum CompanionPackage {
    /// The package inside this app, if this build carries one.
    static var carriedPath: String? {
        Bundle.main.path(forResource: "OctopusNative", ofType: "deb")
    }

    /// This app's version, which a carried package always shares: both come from one CI run.
    static var appVersion: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? ""
    }

    /// The installed companion's version, from the package manager's own records. Read directly
    /// rather than through dpkg, which needs root; the status file is readable by the app.
    static func installedVersion() -> String? {
        let status = "/var/jb/Library/dpkg/status"
        guard let text = try? String(contentsOfFile: status, encoding: .utf8) else { return nil }
        return version(in: text)
    }

    /// The companion's version in the text of a dpkg status file, or nil if it is not listed.
    /// Separate from the file read so it can be checked against a known status file.
    static func version(in status: String) -> String? {
        // One stanza per package, separated by a blank line: "Package:", then among others "Status:"
        // and "Version:". The status matters: dpkg keeps a stanza, version and all, for a package
        // that was removed but left its configuration behind ("deinstall ok config-files"), and
        // reading the version alone reported a removed companion as installed, which hid the very
        // button needed to put it back.
        for stanza in status.components(separatedBy: "\n\n") {
            var isOurs = false
            var isInstalled = false
            var version: String?
            for line in stanza.split(separator: "\n") {
                if line == "Package: com.parv.octopus.native" { isOurs = true }
                if line == "Status: install ok installed" { isInstalled = true }
                if line.hasPrefix("Version: ") { version = String(line.dropFirst(9)) }
            }
            if isOurs, isInstalled, let version, !version.isEmpty { return version }
        }
        return nil
    }
}

/// Installs the companion that Octopus carries inside its own app bundle.
///
/// The companion is a rootless package loaded into Apple's ptpd, and until now it had to be found
/// and installed separately. That is where every version mismatch in this project came from: an
/// app from one build with a companion from another, which looks exactly like a bug in whatever
/// changed between them, and cost two debugging sessions before it was recognised.
///
/// So the package rides inside the app, built in the same CI run and carrying the same version, and
/// this installs it with the jailbreak's dpkg through the root helper. The pattern is Speaka's,
/// which carries its tweak the same way for the same reason.
@MainActor
final class CompanionInstaller: ObservableObject {
    static let shared = CompanionInstaller()

    enum State: Equatable {
        case unknown                                       // not looked yet
        case notCarried                                    // this build has no package inside it
        case matching(String)                              // installed, and this app's version
        case mismatched(installed: String, carried: String)
        case notInstalled(String)                          // carried, but nothing installed
        case installing
        case installed(String)
        case failed(String)
    }

    @Published private(set) var state: State = .unknown

    /// What the button should say, or nil when there is nothing to offer.
    var action: String? {
        switch state {
        case .notInstalled: return "Install Companion"
        case .mismatched: return "Update Companion"
        case .failed: return "Retry Companion"
        default: return nil
        }
    }

    /// The companion's version for Settings, beside the app's, so a mismatch is visible at a glance.
    var versionLabel: String {
        switch state {
        case .unknown: return "Checking"
        case .notCarried: return CompanionPackage.installedVersion() ?? "Not installed"
        case .matching(let version): return version
        case .mismatched(let installed, _): return "\(installed), app is \(CompanionPackage.appVersion)"
        case .notInstalled: return "Not installed"
        case .installing: return "Installing"
        case .installed(let version): return version
        case .failed: return "Install failed"
        }
    }

    /// Whether the label above describes a healthy pair, for its colour.
    var versionHealthy: Bool {
        switch state {
        case .matching, .installed, .unknown, .installing: return true
        case .notCarried: return CompanionPackage.installedVersion() != nil
        case .mismatched, .notInstalled, .failed: return false
        }
    }

    /// One line for the interface, in every state.
    var summary: String {
        switch state {
        case .unknown:
            return "Checking the companion."
        case .notCarried:
            return "This build carries no companion. Install the package from the same release as this app."
        case .matching(let version):
            return "Companion \(version) installed, matching this app."
        case .mismatched(let installed, let carried):
            return "Companion \(installed) is installed but this app is \(carried). They must match: a mismatched "
                + "pair behaves like a bug in whatever changed between them."
        case .notInstalled(let version):
            return "Companion \(version) is ready to install from inside this app."
        case .installing:
            return "Installing the companion."
        case .installed(let version):
            return "Companion \(version) installed and loading into Apple's responder."
        case .failed(let reason):
            return reason
        }
    }

    /// Reads what is installed and compares it with what this app carries.
    func refresh() {
        if state == .installing { return }
        guard CompanionPackage.carriedPath != nil else {
            state = .notCarried
            return
        }
        let carried = CompanionPackage.appVersion
        guard let installed = CompanionPackage.installedVersion() else {
            state = .notInstalled(carried)
            return
        }
        state = installed == carried ? .matching(installed) : .mismatched(installed: installed, carried: carried)
    }

    /// Installs the carried package through the root helper, which runs the jailbreak's dpkg.
    func install() {
        guard CompanionPackage.carriedPath != nil, state != .installing else { return }
        state = .installing
        let helper = Bundle.main.bundlePath + "/octopushelper"
        DispatchQueue.global(qos: .userInitiated).async {
            let outcome = RootHelper.run(helper, ["companion"])
            DispatchQueue.main.async { [weak self] in
                self?.finished(outcome)
            }
        }
    }

    /// The helper's exit codes for this command: 0 done, 3 not root, 4 nothing carried, 8 dpkg failed,
    /// 9 dpkg is absent, 10 the package could not be copied out of the app.
    private func finished(_ outcome: RootHelper.Outcome) {
        DebugLog.log(outcome.status == 0 ? .info : .error, "companion", "install",
                     "Helper exit \(outcome.status)\(outcome.errors.last.map { ": \($0)" } ?? "")")
        if outcome.status == 0 {
            state = .installed(CompanionPackage.appVersion)
            // A newly installed companion only loads into a ptpd that starts after it, so end the
            // current one rather than asking for a reconnect. If nothing is connected this does
            // nothing, and the next connection loads it.
            NativeIntegration.shared.restartResponder(reason: "loading the companion just installed")
            return
        }
        // The helper's own message says more than a number, so it is preferred when there is one.
        if let message = outcome.errors.last {
            state = .failed(message)
            return
        }
        switch outcome.status {
        case 3: state = .failed("Octopus is not running as root. Install it with TrollStore.")
        case 4: state = .failed("This build carries no companion package.")
        case 9: state = .failed("dpkg was not found. The companion needs a rootless jailbreak.")
        case 10: state = .failed("The package could not be copied out of the app.")
        default: state = .failed("The companion could not be installed (exit \(outcome.status)).")
        }
    }
}
