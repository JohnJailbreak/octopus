//
//  DiagnosticsView.swift
//  Octopus
//
//  Settings > Run Full Diagnostics: progress, a row per check with its status and kind, and Export Diagnostics,
//  which shares one ZIP with the results, the logs, build information and connectivity observations.
//

import SwiftUI

struct DiagnosticsWindow: View {
    @EnvironmentObject private var runner: DiagnosticsRunner
    @EnvironmentObject private var service: ServiceController
    @ObservedObject private var native = NativeIntegration.shared

    var body: some View {
        VStack(spacing: 0) {
            header
                .padding(.horizontal, 16)
                .padding(.top, 4)
                .padding(.bottom, 8)
            Rectangle().fill(Theme.hairline).frame(height: 1)
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 2) {
                    VStack(alignment: .leading, spacing: 8) {
                        Toggle("Observe native USB (experimental)", isOn: $native.observe)
                        Toggle("Expose synthetic storage (experimental)", isOn: $native.synthetic)
                            .disabled(!native.observe || (!native.synthetic && !providerPrerequisite))
                        Text(native.detail)
                            .font(.system(size: 12)).foregroundStyle(Theme.secondary)
                        Text("Requires the optional native companion. Keep Octopus open with USB Access enabled. Turning this off stops new exposure; reconnect USB to refresh Explorer.")
                            .font(.system(size: 12)).foregroundStyle(Theme.chevron)
                        if native.observe && !providerPrerequisite {
                            Text("First open the iPhone in Explorer in observer mode, then run diagnostics. Storage exposure requires successful native storage requests in that process.")
                                .font(.system(size: 12)).foregroundStyle(Theme.chevron)
                        }
                    }
                    .font(.system(size: 14)).padding(.vertical, 12)
                    ForEach(groups, id: \.self) { group in
                        Text(group.uppercased())
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(Theme.chevron)
                            .padding(.top, 14)
                            .padding(.bottom, 2)
                        ForEach(runner.results.filter { $0.group == group }) { result in
                            DiagnosticRow(result: result)
                        }
                    }
                    if let current = runner.current {
                        HStack(spacing: 8) {
                            ProgressView().controlSize(.small).tint(.white)
                            Text(current).font(.system(size: 14)).foregroundStyle(Theme.foreground)
                        }
                        .padding(.top, 12)
                    }
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 12)
            }
            Rectangle().fill(Theme.hairline).frame(height: 1)
            footer.padding(14)
        }
        .onAppear {
            if runner.results.isEmpty && !runner.running { runner.start(environment: DiagnosticsEnvironment(service: service)) }
        }
    }

    private var groups: [String] {
        var seen: [String] = []
        for result in runner.results where !seen.contains(result.group) { seen.append(result.group) }
        return seen
    }

    private var providerPrerequisite: Bool {
        // The status file this used to read is written into a folder ptpd's sandbox refuses, so on
        // the current device it never appeared and this toggle could never unlock. The companion
        // now also posts its state by notification, which is the direction proven to cross that
        // boundary; either source may satisfy the prerequisite, and both require the companion to
        // have reported itself eligible after seeing a real host ask for storage.
        if let capture = runner.environment?.nativeCapture,
           capture.expectedLease == native.token, capture.validationProblem == nil,
           capture.report?.providerEligible == true {
            return true
        }
        var runtime = oct_native_runtime()
        guard oct_native_runtime_read(&runtime) == OCT_OK else { return false }
        let eligible = runtime.flags & UInt8(OCT_NATIVE_RUNTIME_ELIGIBLE.rawValue) != 0
        let hooked = runtime.flags & UInt8(OCT_NATIVE_RUNTIME_HOOKED.rawValue) != 0
        return eligible && hooked
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 6) {
            ProgressView(value: Double(runner.results.count), total: Double(max(runner.planned, 1)))
                .tint(Theme.accent)
            Text(runner.running ? "Running check \(min(runner.results.count + 1, max(runner.planned, 1))) of \(max(runner.planned, 1))"
                                : runner.results.isEmpty ? "Starting…" : runner.summaryLine)
                .font(.system(size: 13, weight: .medium))
                .foregroundStyle(Theme.secondary)
            Text("Simulated checks talk to a simulated host through the real adapter and responder. "
                 + "External checks need a real connection; those that can't run say why.")
                .font(.system(size: 12))
                .foregroundStyle(Theme.chevron)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var footer: some View {
        VStack(spacing: 8) {
            if let problem = runner.exportProblem {
                Text(problem)
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.failure)
                    .lineLimit(8)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            HStack(spacing: 10) {
                Button {
                    Haptics.light()
                    runner.start(environment: DiagnosticsEnvironment(service: service))
                } label: {
                    Text("Run Again")
                        .frame(maxWidth: .infinity, minHeight: 44)
                        .foregroundStyle(busy ? Theme.chevron : .white)
                        .background(Capsule().fill(Color.white.opacity(0.12)))
                }
                .buttonStyle(PressDimStyle())
                .disabled(busy)
                if let archive = runner.archive, !busy {
                    ShareLink(item: archive) {
                        Text("Export Diagnostics")
                            .frame(maxWidth: .infinity, minHeight: 44)
                            .foregroundStyle(.black)
                            .background(Capsule().fill(Theme.accent))
                    }
                    .simultaneousGesture(TapGesture().onEnded { Haptics.light() })
                } else if runner.exportProblem != nil, !busy {
                    Button {
                        Haptics.light()
                        runner.retryExport()
                    } label: {
                        Text("Retry Export")
                            .frame(maxWidth: .infinity, minHeight: 44)
                            .foregroundStyle(.black)
                            .background(Capsule().fill(Theme.accent))
                    }
                    .buttonStyle(PressDimStyle())
                } else {
                    Text(runner.exporting ? "Exporting…" : "Export Diagnostics")
                        .frame(maxWidth: .infinity, minHeight: 44)
                        .foregroundStyle(Theme.chevron)
                        .background(Capsule().fill(Color.white.opacity(0.06)))
                }
            }
            .font(.system(size: 15, weight: .semibold))
            // Without an archive the results can still leave the iPhone: as text, which needs no file at all.
            if runner.exportProblem != nil, runner.archive == nil, !busy, !runner.results.isEmpty {
                ShareLink(item: runner.resultsText) {
                    Text("Share Results as Text")
                        .font(.system(size: 14, weight: .semibold))
                        .frame(maxWidth: .infinity, minHeight: 36)
                        .foregroundStyle(.white)
                        .background(Capsule().fill(Color.white.opacity(0.12)))
                }
            }
        }
    }

    private var busy: Bool { runner.running || runner.exporting }
}

private struct DiagnosticRow: View {
    let result: DiagnosticResult

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Text(result.status.rawValue)
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundStyle(color)
                .frame(width: 60, alignment: .leading)
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(result.title).font(.system(size: 15)).foregroundStyle(.white)
                    if result.kind != .local {
                        Text(result.kind.label)
                            .font(.system(size: 11, weight: .semibold))
                            .foregroundStyle(Theme.secondary)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(Capsule().fill(Color.white.opacity(0.12)))
                    }
                }
                Text(result.detail)
                    .font(.system(size: 12))
                    .foregroundStyle(Theme.foreground)
                    .lineLimit(4)
                    .textSelection(.enabled)
            }
        }
        .padding(.vertical, 6)
        .accessibilityElement(children: .combine)
    }

    private var color: Color {
        switch result.status {
        case .pass: return Color(.sRGB, red: 120 / 255, green: 220 / 255, blue: 140 / 255, opacity: 1)
        case .fail: return Theme.failure
        case .skipped: return Theme.chevron
        case .blocked: return Theme.accent
        }
    }
}
