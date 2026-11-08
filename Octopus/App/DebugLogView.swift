//
//  DebugLogView.swift
//  Octopus
//
//  Settings > Debug Log: the shared debug log, newest at the bottom. Filter by severity, copy what is shown,
//  export the whole log with the share sheet, add a device report, or clear it.
//

import SwiftUI
import UIKit

struct DebugLogWindow: View {
    @State private var minimum: DebugLog.Level = .info
    @State private var entries: [DebugLog.Entry] = []
    @State private var stats = DebugLog.Stats()
    @State private var exportURL: URL? = nil
    @State private var exportProblem: String? = nil
    @State private var confirmClear = false
    @State private var notice: String? = nil

    private var visible: [DebugLog.Entry] { entries.filter { $0.level >= minimum } }

    var body: some View {
        VStack(spacing: 10) {
            Picker("Show", selection: $minimum) {
                Text("All").tag(DebugLog.Level.debug)
                Text("Info").tag(DebugLog.Level.info)
                Text("Warnings").tag(DebugLog.Level.warning)
                Text("Errors").tag(DebugLog.Level.error)
            }
            .pickerStyle(.segmented)
            .padding(.horizontal, 16)
            .padding(.top, 12)

            if let failure = DebugLog.lastFailure(in: entries) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Last failure")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(Theme.failure)
                    Text("\(failure.component) \(failure.event): \(failure.message)")
                        .font(.system(size: 12))
                        .foregroundStyle(Color.white.opacity(0.8))
                        .lineLimit(3)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 16)
                .accessibilityElement(children: .combine)
            }

            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 9) {
                        if visible.isEmpty {
                            Text(entries.isEmpty ? "The log is empty." : "Nothing at this level.")
                                .font(.system(size: 13))
                                .foregroundStyle(Color.white.opacity(0.5))
                                .padding(.top, 20)
                        }
                        ForEach(visible) { entry in
                            DebugLogRow(entry: entry)
                                .id(entry.id)
                        }
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 6)
                }
                .refreshable { reload() }
                .onAppear { scrollToNewest(proxy) }
                .onChange(of: minimum) { _, _ in scrollToNewest(proxy) }
                .onChange(of: entries) { _, _ in scrollToNewest(proxy) }
            }

            Text(notice ?? footer)
                .font(.system(size: 11))
                .foregroundStyle(Color.white.opacity(0.5))
                .lineLimit(1)
                .padding(.horizontal, 16)

            HStack(spacing: 0) {
                toolbarButton("Copy", symbol: "doc.on.doc", action: copy)
                if let exportURL {
                    ShareLink(item: exportURL) {
                        ToolbarLabel(title: "Export", symbol: "square.and.arrow.up")
                    }
                    .buttonStyle(PressDimStyle())
                    .accessibilityHint("Shares the whole log as a file")
                } else {
                    ToolbarLabel(title: "Export", symbol: "square.and.arrow.up").opacity(0.35)
                }
                toolbarButton("Report", symbol: "list.bullet.rectangle", action: report)
                toolbarButton("Refresh", symbol: "arrow.clockwise", action: reload)
                toolbarButton("Clear", symbol: "trash") { confirmClear = true }
            }
            .padding(.bottom, 10)
        }
        .onAppear(perform: reload)
        .confirmationDialog("Clear the debug log?", isPresented: $confirmClear, titleVisibility: .visible) {
            Button("Clear Log", role: .destructive) {
                DebugLog.clear()
                reload()
            }
        } message: {
            Text("Every saved line is removed. Export the log first if you still need it.")
        }
    }

    private var footer: String {
        let place = stats.fileOpen ? (stats.path as NSString).lastPathComponent : "memory only"
        let size = ByteCountFormatter.string(fromByteCount: Int64(stats.fileBytes), countStyle: .file)
        let lines = "\(place) · \(size) · \(visible.count) of \(entries.count) lines"
        return exportProblem.map { "\(lines)\nExport unavailable: \($0)" } ?? lines
    }

    private func toolbarButton(_ title: String, symbol: String, action: @escaping () -> Void) -> some View {
        Button {
            Haptics.light()
            action()
        } label: {
            ToolbarLabel(title: title, symbol: symbol)
        }
        .buttonStyle(PressDimStyle())
    }

    private func reload() {
        entries = DebugLog.entries()
        stats = DebugLog.stats()
        if let previous = exportURL { try? FileManager.default.removeItem(at: previous) }
        do {
            exportURL = try DebugLog.exportFile()
            exportProblem = nil
        } catch {
            exportURL = nil
            let problem = Diagnostics.redact(error.localizedDescription)
            if problem != exportProblem { // once per reason, not on every refresh
                DebugLog.log(.error, "log", "export_failed", "The log couldn't be saved for sharing: \(problem)")
            }
            exportProblem = problem
        }
    }

    private func copy() {
        UIPasteboard.general.string = visible.map(\.line).joined(separator: "\n")
        flash("Copied \(visible.count) lines")
    }

    private func report() {
        let values = DeviceReport.collect()
        reload()
        flash("Device report added: \(values) values")
    }

    private func flash(_ text: String) {
        notice = text
        Task {
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            if notice == text { notice = nil }
        }
    }

    private func scrollToNewest(_ proxy: ScrollViewProxy) {
        if let last = visible.last { proxy.scrollTo(last.id, anchor: .bottom) }
    }
}

private struct ToolbarLabel: View {
    let title: String
    let symbol: String

    var body: some View {
        VStack(spacing: 4) {
            Image(systemName: symbol)
                .font(.system(size: 17, weight: .medium))
            Text(title)
                .font(.system(size: 11))
        }
        .foregroundStyle(.white)
        .frame(maxWidth: .infinity, minHeight: 44)
        .contentShape(Rectangle())
    }
}

struct DebugLogRow: View {
    let entry: DebugLog.Entry

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 6) {
                Text(entry.level.title.uppercased())
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .foregroundStyle(color)
                Text(String(entry.time.dropFirst(11).prefix(12)))
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundStyle(Color.white.opacity(0.5))
                Text("\(entry.component) · \(entry.event)")
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundStyle(Color.white.opacity(0.65))
                    .lineLimit(1)
            }
            if !entry.message.isEmpty {
                Text(entry.message)
                    .font(.system(size: 12, design: .monospaced))
                    .foregroundStyle(Color.white.opacity(0.9))
                    .textSelection(.enabled)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .combine)
    }

    private var color: Color {
        switch entry.level {
        case .debug: return Color.white.opacity(0.45)
        case .info: return Color.white.opacity(0.75)
        case .warning: return Theme.accent
        case .error: return Theme.failure
        }
    }
}
