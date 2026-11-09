import SwiftUI
import UIKit

/// Caz-style window: column width, 70 percent of the screen height (80 on
/// short screens), 16 pt continuous corners, a 57 pt header and a hairline.
struct WindowView: View {
    let kind: AppWindow
    let layout: Layout
    let close: () -> Void
    /// Settings > Check for Updates found one: the update window, as Update Available opens it.
    let showUpdate: () -> Void
    /// Settings > Debug Log.
    let showDebugLog: () -> Void
    /// Settings > Run Full Diagnostics.
    let showDiagnostics: () -> Void
    @State private var selection: StorageObject? = nil

    private var shape: RoundedRectangle { RoundedRectangle(cornerRadius: 16, style: .continuous) }

    var body: some View {
        VStack(spacing: 0) {
            header
            Rectangle()
                .fill(Theme.hairline)
                .frame(height: 1)
                .padding(.horizontal, 20)
            content
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .frame(width: layout.windowSize.width, height: layout.windowSize.height)
        .background(shape.fill(Theme.window))
        .clipShape(shape)
    }

    private var header: some View {
        ZStack {
            Text(selection?.name ?? kind.title)
                .font(.system(size: 17, weight: .medium))
                .foregroundStyle(.white)
                .lineLimit(1)
                .padding(.horizontal, 50)
                .accessibilityAddTraits(.isHeader)
            if selection != nil {
                HStack {
                    Button {
                        Haptics.light()
                        withAnimation(Motion.window) { selection = nil }
                    } label: {
                        Image(systemName: "chevron.left")
                            .font(.system(size: 16, weight: .semibold))
                            .foregroundStyle(Theme.foreground)
                            .frame(width: 44, height: 44)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(PressDimStyle())
                    .accessibilityLabel("Back")
                    Spacer()
                }
                .padding(.leading, 8)
            }
        }
        .frame(height: 57)
    }

    @ViewBuilder private var content: some View {
        switch kind {
        case .usb:
            ConnectionWindow()
        case .filesystem:
            if let selection {
                ObjectWindow(object: selection)
            } else {
                FilesystemWindow { object in
                    withAnimation(Motion.window) { selection = object }
                }
            }
        case .activity:
            ActivityWindow(close: close)
        case .settings:
            SettingsWindow(showUpdate: showUpdate, showDebugLog: showDebugLog, showDiagnostics: showDiagnostics)
        case .debugLog:
            DebugLogWindow()
        case .diagnostics:
            DiagnosticsWindow()
        case .update:
            EmptyView()   // the main screen shows the update flow itself, without a window card
        }
    }
}

/// Inset-grouped list on the window fill: no row backgrounds or separators,
/// rows at least 48 pt, switches in the accent colour.
struct WindowList<Content: View>: View {
    @ViewBuilder let content: Content

    var body: some View {
        List {
            content
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .environment(\.defaultMinListRowHeight, 48)
        .tint(Theme.accent)
    }
}

extension View {
    func windowRow() -> some View {
        listRowBackground(Color.clear)
            .listRowSeparator(.hidden)
    }
}

struct SectionHeader: View {
    let text: String

    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text)
            .font(.system(size: 12, weight: .medium))
            .foregroundStyle(Color.white.opacity(0.6))
            .textCase(nil)
    }
}

struct SectionFooter: View {
    let text: String

    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text)
            .font(.system(size: 12))
            .foregroundStyle(Color.white.opacity(0.5))
    }
}

struct InfoRow: View {
    let title: String
    let value: String
    var tint: Color = Theme.chevron

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 12) {
            Text(title)
                .foregroundStyle(.white)
            Spacer(minLength: 12)
            Text(value)
                .foregroundStyle(tint)
                .multilineTextAlignment(.trailing)
        }
        .font(.system(size: 15))
        .windowRow()
    }
}

// MARK: - USB Connection

struct ConnectionWindow: View {
    @EnvironmentObject private var service: ServiceController

    private func stageRow(_ stage: ConnectionStage, current: Bool) -> some View {
        StateRow(title: stage.title, current: current, seen: service.observedStages.contains(stage),
                 supported: !stage.needsTransport || service.status.transportAvailable)
    }

    var body: some View {
        let status = service.status
        WindowList {
            Section {
                InfoRow(title: "Service", value: service.state.title)
                InfoRow(title: "Portable USB adapter",
                        value: status.transportAvailable ? status.transportName : "Not in this build",
                        tint: status.transportAvailable ? Theme.chevron : Theme.failure)
                InfoRow(title: "Transport", value: ConnectionStage(transport: status.transportState).title,
                        tint: status.transportState == .error ? Theme.failure : Theme.chevron)
                InfoRow(title: "Host (portable adapter)",
                        value: status.transportState == .hostConnected || status.transportState == .sessionActive
                            ? "Connected" : "Not observed")
                InfoRow(title: "Session", value: status.sessionOpen ? "Open (ID \(status.sessionID))" : "None")
            } header: {
                SectionHeader("Status")
            }
            if !status.transportFailure.isEmpty {
                Section {
                    Text(status.transportFailure)
                        .font(.system(size: 13))
                        .foregroundStyle(Theme.failure)
                        .windowRow()
                } header: {
                    SectionHeader("Last transport failure")
                }
            }
            Section {
                ForEach(ConnectionStage.service, id: \.self) { stage in
                    stageRow(stage, current: ConnectionStage(service: service.state) == stage)
                }
            } header: {
                SectionHeader("Service")
            }
            Section {
                ForEach(ConnectionStage.transport, id: \.self) { stage in
                    stageRow(stage, current: ConnectionStage(transport: status.transportState) == stage)
                }
            } header: {
                SectionHeader("USB transport")
            } footer: {
                SectionFooter("Octopus marks only the states it has observed: Current, or Seen since launch. The transport states need a USB driver. This build has the tested transport adapter but no driver yet, so the transport stays unavailable.")
            }
            Section {
                InfoRow(title: "Protocol", value: "PTP 1.0, MTP 1.0 objects")
                InfoRow(title: "Operations", value: "\(status.operationCount)")
                InfoRow(title: "Requests served", value: "\(status.transactions)")
                InfoRow(title: "Storage ID",
                        value: status.storageID == 0 ? "None" : String(format: "0x%08X", status.storageID))
            } header: {
                SectionHeader("Responder")
            }
        }
    }
}

struct StateRow: View {
    let title: String
    let current: Bool
    let seen: Bool
    let supported: Bool

    var body: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(current ? Theme.accent : Color.white.opacity(0.25))
                .frame(width: 8, height: 8)
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.system(size: 15))
                    .foregroundStyle(.white)
                if !supported {
                    Text("Needs the USB transport")
                        .font(.system(size: 12))
                        .foregroundStyle(Color.white.opacity(0.5))
                }
            }
            Spacer(minLength: 0)
            if current {
                Text("Current")
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(Theme.accent)
            } else if seen {
                Text("Seen")
                    .font(.system(size: 12))
                    .foregroundStyle(Color.white.opacity(0.5))
            }
        }
        .opacity(supported ? 1 : 0.55)
        .windowRow()
        .accessibilityElement(children: .combine)
    }
}

// MARK: - Filesystem

struct FilesystemWindow: View {
    let select: (StorageObject) -> Void
    @EnvironmentObject private var service: ServiceController

    var body: some View {
        if let storage = service.storage {
            WindowList {
                Section {
                    InfoRow(title: "Name", value: storage.name)
                    InfoRow(title: "Capacity", value: storage.capacity.byteString)
                    InfoRow(title: "Free", value: storage.free.byteString)
                    InfoRow(title: "Access", value: storage.readOnly ? "Read-only" : "Read-write")
                    InfoRow(title: "Objects", value: "\(storage.objectCount)")
                } header: {
                    SectionHeader("Storage")
                }
                Section {
                    ForEach(service.tree) { entry in
                        TreeRow(entry: entry) { select(entry.object) }
                    }
                } header: {
                    SectionHeader("Objects")
                } footer: {
                    SectionFooter("Synthetic objects generated in memory by the file-object provider. Nothing from the phone's own filesystem is exposed.")
                }
            }
        } else {
            VStack(spacing: 12) {
                Image(systemName: "externaldrive")
                    .font(.system(size: 28, weight: .light))
                Text("Enable USB Access to load the Octopus storage.")
                    .font(.system(size: 15))
                    .multilineTextAlignment(.center)
            }
            .foregroundStyle(Color.white.opacity(0.6))
            .padding(30)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }
}

struct TreeRow: View {
    let entry: StorageEntry
    let action: () -> Void

    var body: some View {
        Button {
            Haptics.light()
            action()
        } label: {
            HStack(spacing: 12) {
                Image(systemName: entry.object.isDirectory ? "folder" : "doc")
                    .font(.system(size: 15, weight: .medium))
                    .foregroundStyle(Theme.foreground)
                    .frame(width: 22)
                VStack(alignment: .leading, spacing: 2) {
                    Text(entry.object.name)
                        .font(.system(size: 16))
                        .foregroundStyle(.white)
                        .lineLimit(1)
                        .minimumScaleFactor(0.85)
                    Text(entry.object.caption)
                        .font(.system(size: 12))
                        .foregroundStyle(Color.white.opacity(0.55))
                }
                Spacer(minLength: 0)
                Image(systemName: "chevron.right")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(Theme.chevron)
            }
            .padding(.leading, CGFloat(entry.depth) * 18)
            .contentShape(Rectangle())
        }
        .buttonStyle(PressDimStyle())
        .listRowInsets(EdgeInsets(top: 6, leading: 16, bottom: 6, trailing: 16))
        .windowRow()
    }
}

struct ObjectWindow: View {
    let object: StorageObject
    @EnvironmentObject private var service: ServiceController

    var body: some View {
        WindowList {
            Section {
                InfoRow(title: "Handle", value: String(format: "0x%08X", object.id))
                InfoRow(title: "Parent",
                        value: object.parent == 0 ? "Storage root" : String(format: "0x%08X", object.parent))
                InfoRow(title: "Format", value: object.formatName)
                InfoRow(title: "Protection", value: object.writeProtected ? "Read-only" : "None")
                if !object.isDirectory {
                    InfoRow(title: "Size", value: "\(object.size) bytes")
                }
                InfoRow(title: "Modified", value: object.modified.formatted(date: .abbreviated, time: .shortened))
            } header: {
                SectionHeader("Object")
            }
            if !object.isDirectory {
                Section {
                    Text(service.preview(of: object))
                        .font(.system(size: 12, design: .monospaced))
                        .foregroundStyle(Color.white.opacity(0.85))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .windowRow()
                } header: {
                    SectionHeader("Contents")
                } footer: {
                    SectionFooter("Read through the provider, up to the first 512 bytes.")
                }
            }
        }
    }
}

// MARK: - Activity

struct ActivityWindow: View {
    let close: () -> Void
    @EnvironmentObject private var service: ServiceController

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    LazyVStack(alignment: .leading, spacing: 4) {
                        if service.log.isEmpty {
                            Text("No activity yet. Enable USB Access or run the self-test.")
                                .foregroundStyle(Color.white.opacity(0.5))
                        }
                        ForEach(service.log) { entry in
                            Text(entry.line)
                                .foregroundStyle(entry.isFailure ? Theme.failure : Color.white.opacity(0.85))
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                    .font(.system(size: 12, design: .monospaced))
                    .textSelection(.enabled)
                    .padding(.top, 16)
                    Button(action: runSelfTest) {
                        HStack(spacing: 10) {
                            Image(systemName: "stethoscope")
                                .font(.system(size: 19, weight: .medium))
                            Text("Run Self-Test")
                                .font(.system(size: 17))
                        }
                        .foregroundStyle(.white)
                        .frame(maxWidth: .infinity)
                        .frame(height: 50)
                        .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Theme.menu))
                    }
                    .buttonStyle(PressDimStyle())
                    .disabled(service.isBusy)
                    .opacity(service.isBusy ? 0.7 : 1)
                    .padding(.top, 20)
                    Text("Replays a recorded PTP/MTP session through the protocol responder and checks every reply.")
                        .font(.system(size: 12))
                        .foregroundStyle(Color.white.opacity(0.5))
                        .padding(.top, 8)
                        .padding(.bottom, 20)
                        .id("bottom")
                }
                .padding(.horizontal, 20)
            }
            .onAppear { proxy.scrollTo("bottom", anchor: .bottom) }
        }
    }

    private func runSelfTest() {
        Haptics.light()
        close()
        service.runSelfTest()
    }
}

// MARK: - Settings

struct SettingsWindow: View {
    let showUpdate: () -> Void
    let showDebugLog: () -> Void
    let showDiagnostics: () -> Void
    @AppStorage(Preferences.detailedLogging) private var detailedLogging = false
    @EnvironmentObject private var updater: Updater
    @AppStorage(Preferences.haptics) private var haptics = true
    @AppStorage(Preferences.selfTestOnEnable) private var selfTest = true
    @AppStorage(Preferences.scratchFolder) private var scratch = false
    @AppStorage(Preferences.updateNotifications) private var updateNotifications = false
    @State private var notificationsOn = Preferences.updateNotificationsEnabled
    @State private var notificationsRefused = false

    private var checking: Bool { updater.manual == .checking }

    var body: some View {
        WindowList {
            Section {
                Toggle(isOn: $haptics) { Text("Haptics") }
                    .font(.system(size: 15))
                    .windowRow()
            } header: {
                SectionHeader("Feedback")
            }
            Section {
                Toggle(isOn: $selfTest) { Text("Self-test while enabling") }
                    .font(.system(size: 15))
                    .windowRow()
                Toggle(isOn: $scratch) { Text("Writable Scratch folder") }
                    .font(.system(size: 15))
                    .windowRow()
            } header: {
                SectionHeader("Service")
            } footer: {
                SectionFooter("Applies the next time USB access is enabled. Scratch is an empty in-memory folder and the only place a host may create, rename or delete objects.")
            }
            Section {
                Toggle(isOn: $notificationsOn) { Text("Update notifications") }
                    .font(.system(size: 15))
                    .windowRow()
                Button(action: checkForUpdates) {
                    HStack {
                        Text(checking ? "Checking for Updates..." : "Check for Updates")
                            .foregroundStyle(.white)
                        Spacer()
                        if checking {
                            ProgressView()
                                .tint(Theme.chevron)
                        } else {
                            Image(systemName: "arrow.down.circle")
                                .font(.system(size: 15, weight: .medium))
                                .foregroundStyle(Theme.chevron)
                        }
                    }
                    .font(.system(size: 15))
                    .contentShape(Rectangle())
                }
                .buttonStyle(PressDimStyle())
                .disabled(checking || updater.isBusy)
                .windowRow()
            } header: {
                SectionHeader("Updates")
            } footer: {
                updatesFooter
            }
            Section {
                Button {
                    Haptics.light()
                    showDiagnostics()
                } label: {
                    HStack {
                        Text("Run Full Diagnostics")
                            .foregroundStyle(.white)
                        Spacer()
                        Image(systemName: "chevron.right")
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundStyle(Theme.chevron)
                    }
                    .font(.system(size: 15))
                    .contentShape(Rectangle())
                }
                .buttonStyle(PressDimStyle())
                .windowRow()
                Toggle(isOn: $detailedLogging) { Text("Detailed logging") }
                    .font(.system(size: 15))
                    .windowRow()
                Button {
                    Haptics.light()
                    showDebugLog()
                } label: {
                    HStack {
                        Text("Debug Log")
                            .foregroundStyle(.white)
                        Spacer()
                        Image(systemName: "chevron.right")
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundStyle(Theme.chevron)
                    }
                    .font(.system(size: 15))
                    .contentShape(Rectangle())
                }
                .buttonStyle(PressDimStyle())
                .windowRow()
            } header: {
                SectionHeader("Diagnostics")
            } footer: {
                SectionFooter("Detailed logging records every request a computer makes. The log never holds file names or contents.")
            }
            Section {
                InfoRow(title: "Version", value: AppInfo.version)
                InfoRow(title: "Core", value: CoreService.version)
                InfoRow(title: "System", value: "iOS \(UIDevice.current.systemVersion)")
                Link(destination: AppInfo.repository) {
                    HStack {
                        Text("Source code")
                            .foregroundStyle(.white)
                        Spacer()
                        Image(systemName: "chevron.right")
                            .font(.system(size: 14, weight: .semibold))
                            .foregroundStyle(Theme.chevron)
                    }
                    .font(.system(size: 15))
                }
                .windowRow()
                DisclosureGroup {
                    Text(Self.dopamineLicense)
                        .font(.system(size: 12))
                        .foregroundStyle(Color.white.opacity(0.6))
                        .windowRow()
                } label: {
                    Text("Dopamine's license")
                        .font(.system(size: 15))
                        .foregroundStyle(.white)
                }
                .windowRow()
            } header: {
                SectionHeader("About")
            } footer: {
                SectionFooter("The update screens follow Dopamine's, by Lars Fröder (opa334) and its contributors, under the MIT License.")
            }
        }
        .onAppear { updater.refreshLastChecked() }
        .onChange(of: notificationsOn) { _, on in
            guard on != updateNotifications else { return }
            // Turning it on asks iOS for permission first; refused, it stays off.
            UpdateNotifications.setEnabled(on) { enabled in
                notificationsRefused = on && !enabled
                if notificationsOn != enabled { notificationsOn = enabled }
            }
        }
        .onChange(of: detailedLogging) { _, on in
            DebugLog.setDetailed(on)
        }
        .onChange(of: updateNotifications) { _, on in
            if notificationsOn != on { notificationsOn = on }   // turned off in iOS Settings meanwhile
        }
    }

    private func checkForUpdates() {
        Haptics.light()
        updater.checkNow { result in
            if case .available = result { showUpdate() }
        }
    }

    /// Under the Updates section: why notifications are off, what the last check found, or when Octopus last checked.
    @ViewBuilder private var updatesFooter: some View {
        if notificationsRefused {
            Text("Allow notifications for Octopus in Settings > Notifications, then try again.")
                .font(.system(size: 12))
                .foregroundStyle(Theme.failure)
        } else if let line = checkLine {
            Text(line.text)
                .font(.system(size: 12))
                .foregroundStyle(line.failed ? Theme.failure : Color.white.opacity(0.5))
        }
    }

    private var checkLine: (text: String, failed: Bool)? {
        switch updater.manual {
        case .upToDate(let version):
            return ("Octopus \(version) is up to date.", false)
        case .failed(let reason):
            return ("Couldn't check for updates: \(reason).", true)
        case .idle, .checking:
            guard let when = updater.lastChecked else { return nil }
            let today = Calendar.current.isDateInToday(when)
            let time = DateFormatter.localizedString(from: when, dateStyle: today ? .none : .short, timeStyle: .short)
            return (today ? "Last checked at \(time)." : "Last checked \(time).", false)
        }
    }

    /// Dopamine's MIT License, shipped in the app (Octopus/Resources/LICENSE-Dopamine.txt).
    static let dopamineLicense: String = {
        if let path = Bundle.main.path(forResource: "LICENSE-Dopamine", ofType: "txt"),
           let text = try? String(contentsOfFile: path, encoding: .utf8) {
            return text.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        return "MIT License\n\nCopyright (c) 2023-2024 Lars Fröder (opa334)"
    }()
}
