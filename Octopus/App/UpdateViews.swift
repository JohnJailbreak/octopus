//
//  UpdateViews.swift
//  Octopus
//
//  The update flow, Speaka's and Caz's, which is drawn like Dopamine's: both screens sit in the centred window frame
//  with no card behind them, only the dimmed backdrop (DOUpdateViewController, DODownloadViewController,
//  DOUpdateCircleView, DOModalTransitionPush).
//  Dopamine: Copyright (c) 2023-2024 Lars Fröder (opa334), MIT License (see LICENSE-Dopamine.txt).
//

import SwiftUI
import UIKit

/// A borderless menu-style button (DOActionMenuButton without a chevron): "Update Available" on the main screen and
/// "Update" under the changelog.
/// The companion's install, confirmed in the slot its button occupied. Shaped like that button so
/// nothing shifts when one replaces the other, but inert: the work is done.
struct CompanionConfirmation: View {
    let title: String

    var body: some View {
        HStack(spacing: 10) {
            // A plain tick, the same weight and size as the button's own box icon, so the slot keeps
            // its shape: mockup 1. A filled disc would introduce a second colour the home screen
            // otherwise avoids.
            Image(systemName: "checkmark")
                .font(.system(size: 19, weight: .medium))
            Text(title)
                .font(.system(size: 17))
        }
        .foregroundStyle(.white)
        .padding(.horizontal, 12)
        .frame(maxHeight: .infinity)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isStaticText)
    }
}

struct UpdateLinkButton: View {
    let title: String
    let symbol: String
    let action: () -> Void

    var body: some View {
        Button {
            Haptics.light()
            action()
        } label: {
            HStack(spacing: 10) {
                Image(systemName: symbol)
                    .font(.system(size: 19, weight: .medium))
                Text(title)
                    .font(.system(size: 17))
            }
            .foregroundStyle(.white)
            .padding(.horizontal, 12)
            .frame(maxHeight: .infinity)
            .contentShape(Rectangle())
        }
        .buttonStyle(PressDimStyle())
    }
}

/// The changelog, then the download, pushed sideways by a full screen width like DOModalTransitionPush.
struct UpdateFlowView: View {
    @ObservedObject var updater: Updater
    let screenWidth: CGFloat

    var body: some View {
        ZStack {
            if updater.stage == .changelog {
                ChangelogView(updater: updater) {
                    updater.startDownload()
                }
                .transition(.asymmetric(insertion: .offset(x: -screenWidth), removal: .offset(x: -screenWidth)))
            } else {
                DownloadView(updater: updater)
                    .transition(.asymmetric(insertion: .offset(x: screenWidth), removal: .offset(x: screenWidth)))
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .animation(Motion.window, value: updater.stage == .changelog)
    }
}

/// DOUpdateViewController: a 24 pt title 20 pt from the top, the release notes centred at 70% under a fade mask
/// (0, 1%, 50%, 87%), and a 30 pt Update button 20 pt from the bottom.
struct ChangelogView: View {
    @ObservedObject var updater: Updater
    let onUpdate: () -> Void

    var body: some View {
        ZStack(alignment: .bottom) {
            VStack(spacing: 0) {
                Text("Changelog")
                    .font(.system(size: 24, weight: .medium))
                    .foregroundStyle(.white)
                    .padding(.top, 20)
                    .accessibilityAddTraits(.isHeader)
                ScrollView {
                    ChangelogText.make(updater.updates)
                        .foregroundStyle(.white)
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: .infinity)
                        .padding(.horizontal, 5)
                        .padding(.vertical, 8)
                }
                .scrollIndicators(.hidden)
                .opacity(0.7)
                .mask {
                    LinearGradient(stops: [
                        .init(color: .clear, location: 0),
                        .init(color: .black, location: 0.01),
                        .init(color: .black, location: 0.5),
                        .init(color: .clear, location: 0.87),
                    ], startPoint: .top, endPoint: .bottom)
                }
                .padding(.top, 20)
            }
            if updater.isAvailable {
                UpdateLinkButton(title: "Update", symbol: "arrow.down", action: onUpdate)
                    .frame(height: 30)
                    .padding(.bottom, 20)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

enum ChangelogText {
    /// Each release: its name in bold 18 pt, a blank line, its notes at 16 pt, then three blank lines.
    static func make(_ releases: [OctopusRelease]) -> Text {
        guard !releases.isEmpty else {
            return Text("Changelog Unavailable").font(.system(size: 18))
        }
        var text = Text("")
        for release in releases {
            text = text + Text(release.name + "\n").font(.system(size: 18, weight: .bold))
            text = text + Text("\n")
            text = text + Text(markdown(release.body)).font(.system(size: 16))
            text = text + Text("\n\n\n")
        }
        return text + Text(String(repeating: "\n", count: 10))
    }

    /// Inline markdown only, keeping line breaks, like Dopamine's parsing options.
    static func markdown(_ source: String) -> AttributedString {
        let options = AttributedString.MarkdownParsingOptions(allowsExtendedAttributes: true,
                                                              interpretedSyntax: .inlineOnlyPreservingWhitespace)
        return (try? AttributedString(markdown: source, options: options)) ?? AttributedString(source)
    }
}

/// DODownloadViewController: a centred stack, 80% wide with 10 pt spacing: a 24 pt title, an 18 pt subtitle at 50%,
/// a 20 pt gap and the 150 pt circle, which pulses to 70% while installing.
struct DownloadView: View {
    @ObservedObject var updater: Updater
    @State private var dimmed = false

    var body: some View {
        GeometryReader { geo in
            VStack(spacing: 10) {
                Text(title)
                    .font(.system(size: 24, weight: .medium))
                    .foregroundStyle(.white)
                    .multilineTextAlignment(.center)
                Text(subtitle)
                    .font(.system(size: 18))
                    .foregroundStyle(Color.white.opacity(0.5))
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
                Color.clear
                    .frame(height: 20)
                UpdateCircle(progress: updater.progress)
                    .frame(width: 150, height: 150)
                    .opacity(dimmed ? 0.7 : 1)
                    .opacity(isFailed ? 0 : 1)
            }
            .frame(width: geo.size.width * 0.8)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .onChange(of: updater.stage) { _, stage in
            if stage == .installing {
                withAnimation(.easeInOut(duration: 0.5).repeatForever(autoreverses: true)) { dimmed = true }
            } else {
                withAnimation(.easeOut(duration: 0.2)) { dimmed = false }
            }
        }
    }

    private var isFailed: Bool {
        if case .failed = updater.stage { return true }
        return false
    }

    private var title: String {
        switch updater.stage {
        case .installing: return "Installing update..."
        case .installed: return "Update installed"
        case .failed: return "Update failed"
        case .changelog, .downloading: return "Downloading update..."
        }
    }

    private var subtitle: String {
        switch updater.stage {
        case .installing: return "Octopus will close when it's done"
        case .installed: return "Close Octopus and open it again"
        case .failed(let message): return message
        case .changelog, .downloading: return "Please wait while files finish downloading"
        }
    }
}

/// DOUpdateCircleView: a 10 pt track at 10% white, a white arc with round ends from the top, and the percentage in
/// 29 pt medium.
struct UpdateCircle: View {
    let progress: Double

    var body: some View {
        ZStack {
            Circle()
                .stroke(Color.white.opacity(0.1), lineWidth: 10)
                .padding(5)
            Circle()
                .trim(from: 0, to: max(0, min(1, progress)))
                .stroke(Color.white, style: StrokeStyle(lineWidth: 10, lineCap: .round))
                .rotationEffect(.degrees(-90))
                .padding(5)
            Text("\(Int(progress * 100))%")
                .font(.system(size: 29, weight: .medium))
                .foregroundStyle(.white)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Update progress")
        .accessibilityValue("\(Int(progress * 100)) percent")
    }
}
