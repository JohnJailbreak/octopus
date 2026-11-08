import SwiftUI
import UIKit

/// Caz-style window: column width, 70 percent of the screen height (80 on
/// short screens), 16 pt continuous corners, a 57 pt header and a hairline.
struct WindowView: View {
    let kind: AppWindow
    let layout: Layout
    let close: () -> Void
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
            SettingsWindow()
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

    var body: some View {
        let status = service.status
        WindowList {
            Section {
                InfoRow(title: "Service", value: service.state.title)
                InfoRow(title: "USB transport",
                        value: status.transportAvailable ? status.transportName : "Not in this build",
                        tint: status.transportAvailable ? Theme.chevron : Theme.failure)
                InfoRow(title: "Host", value: service.state == .connected ? "Connected" : "Not connected")
                InfoRow(title: "Session", value: status.sessionOpen ? "Open (ID \(status.sessionID))" : "None")
            } header: {
                SectionHeader("Status")
            }
            Section {
                ForEach(ServiceState.allCases, id: \.self) { state in
                    StateRow(state: state, current: service.state == state,
                             supported: state.isSupported(withTransport: status.transportAvailable))
                }
            } header: {
                SectionHeader("States")
            } footer: {
                SectionFooter("Octopus only shows states it can observe. Waiting for connection and Connected need the live USB transport, which this build does not include.")
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
    let state: ServiceState
    let current: Bool
    let supported: Bool

    var body: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(current ? Theme.accent : Color.white.opacity(0.25))
                .frame(width: 8, height: 8)
            VStack(alignment: .leading, spacing: 2) {
                Text(state.title)
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
    @AppStorage(Preferences.haptics) private var haptics = true
    @AppStorage(Preferences.selfTestOnEnable) private var selfTest = true
    @AppStorage(Preferences.scratchFolder) private var scratch = false

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
            } header: {
                SectionHeader("About")
            }
        }
    }
}
