import SwiftUI
import UIKit

enum AppWindow: String, CaseIterable, Identifiable {
    case usb, filesystem, activity, settings

    var id: String { rawValue }

    var title: String {
        switch self {
        case .usb: return "USB Connection"
        case .filesystem: return "Filesystem"
        case .activity: return "Activity"
        case .settings: return "Settings"
        }
    }

    var symbol: String {
        switch self {
        case .usb: return "cable.connector"
        case .filesystem: return "folder"
        case .activity: return "list.bullet.rectangle"
        case .settings: return "gearshape"
        }
    }
}

struct MainScreen: View {
    let layout: Layout
    @EnvironmentObject private var service: ServiceController
    @State private var window: AppWindow? = nil
    @Namespace private var panel

    private var windowOpen: Bool { window != nil }
    private var sheetOpen: Bool { service.sheet.isPresented }
    private var panelShape: RoundedRectangle { RoundedRectangle(cornerRadius: 14, style: .continuous) }

    var body: some View {
        ZStack(alignment: .topLeading) {
            Color.black
            Backdrop()
                .opacity(windowOpen ? 0.4 : 1)
                .animation(Motion.dim, value: windowOpen)
            mainLayer
                .opacity(windowOpen ? 0 : 1)
                .scaleEffect(windowOpen ? 0.9 : 1)
                .allowsHitTesting(!windowOpen)
            if let window {
                Color.clear
                    .contentShape(Rectangle())
                    .onTapGesture(perform: closeWindow)
                    .accessibilityLabel("Close window")
                WindowView(kind: window, layout: layout, close: closeWindow)
                    .position(x: layout.size.width / 2, y: layout.size.height / 2)
                    .transition(.scale(scale: 0.7).combined(with: .opacity))
            }
        }
        .frame(width: layout.size.width, height: layout.size.height)
    }

    private var mainLayer: some View {
        ZStack(alignment: .topLeading) {
            HeaderView(warning: service.headerWarning)
                .frame(width: layout.columnWidth - 10, height: layout.headerHeight, alignment: .topLeading)
                .position(x: layout.size.width / 2, y: layout.columnTop + layout.headerHeight / 2)
                .offset(y: sheetOpen ? -25 : 0)
            MenuPanel(layout: layout, enabled: !service.isBusy, open: openWindow)
                .frame(width: layout.columnWidth, height: layout.menuHeight)
                .scaleEffect(sheetOpen ? 0.6 : 1)
                .offset(y: sheetOpen ? -75 : 0)
                .opacity(sheetOpen ? 0 : 1)
                .allowsHitTesting(!sheetOpen)
                .position(x: layout.size.width / 2, y: layout.menuTop + layout.menuHeight / 2)
            if sheetOpen {
                ActivitySheetView(sheet: service.sheet, layout: layout)
                    .frame(width: layout.size.width, height: layout.sheetHeight)
                    .background(panelShape.fill(Theme.menu).matchedGeometryEffect(id: "panel", in: panel))
                    .position(x: layout.size.width / 2, y: layout.sheetTop + layout.sheetHeight / 2)
            } else {
                Button(action: primaryAction) {
                    PrimaryButtonLabel(active: service.isActive)
                        .frame(width: layout.columnWidth, height: layout.buttonHeight)
                        .background(panelShape.fill(Theme.menu).matchedGeometryEffect(id: "panel", in: panel))
                        .contentShape(panelShape)
                }
                .buttonStyle(PressDimStyle())
                .disabled(service.isBusy)
                .opacity(service.isBusy ? 0.7 : 1)
                .position(x: layout.size.width / 2, y: layout.buttonTop + layout.buttonHeight / 2)
            }
        }
        .frame(width: layout.size.width, height: layout.size.height)
    }

    private func primaryAction() {
        Haptics.light()
        service.toggle()
    }

    private func openWindow(_ kind: AppWindow) {
        service.refresh()
        withAnimation(Motion.window) { window = kind }
    }

    private func closeWindow() {
        withAnimation(Motion.window) { window = nil }
    }
}

struct HeaderView: View {
    let warning: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("Octopus")
                .font(.custom(Wordmark.font, fixedSize: Wordmark.size))
                .tracking(Wordmark.tracking)
                .foregroundStyle(.white)
                .lineLimit(1)
                .fixedSize()
                .alignmentGuide(.top) { dimensions in dimensions[.firstTextBaseline] - Wordmark.baseline }
                .frame(height: 40, alignment: .topLeading)
                .accessibilityAddTraits(.isHeader)
            Text("iOS \(UIDevice.current.systemVersion)")
                .font(.system(size: 14, weight: .medium))
                .foregroundStyle(.white)
                .lineLimit(1)
            Text(warning ?? "by Parv")
                .font(.system(size: 14))
                .foregroundStyle(warning == nil ? Theme.secondary : Theme.failure)
                .lineLimit(2)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

struct MenuPanel: View {
    let layout: Layout
    let enabled: Bool
    let open: (AppWindow) -> Void

    var body: some View {
        VStack(spacing: 0) {
            ForEach(AppWindow.allCases) { kind in
                MenuRow(kind: kind, height: layout.rowHeight, showsSeparator: kind != .settings) {
                    open(kind)
                }
            }
        }
        .padding(.horizontal, layout.menuSidePadding)
        .padding(.vertical, 5)
        .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Theme.menu))
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.45)
    }
}

struct MenuRow: View {
    let kind: AppWindow
    let height: CGFloat
    let showsSeparator: Bool
    let action: () -> Void

    var body: some View {
        Button {
            Haptics.light()
            action()
        } label: {
            HStack(spacing: 10) {
                Image(systemName: kind.symbol)
                    .font(.system(size: 19, weight: .medium))
                Text(kind.title)
                    .font(.system(size: 17))
                    .lineLimit(1)
                    .minimumScaleFactor(0.6)
                Spacer(minLength: 10)
                Image(systemName: "chevron.right")
                    .font(.system(size: 16))
                    .foregroundStyle(Theme.chevron)
            }
            .foregroundStyle(.white)
            .padding(.leading, 12)
            .padding(.trailing, 10)
            .frame(height: height)
            .contentShape(Rectangle())
        }
        .buttonStyle(PressDimStyle())
        .overlay(alignment: .bottom) {
            if showsSeparator {
                Capsule()
                    .fill(Theme.separator)
                    .frame(height: 1)
                    .padding(.horizontal, 10)
            }
        }
    }
}

struct PrimaryButtonLabel: View {
    let active: Bool

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: active ? "eject" : "externaldrive.connected.to.line.below")
                .font(.system(size: 19, weight: .medium))
            Text(active ? "Disable USB Access" : "Enable USB Access")
                .font(.system(size: 17))
                .lineLimit(1)
                .minimumScaleFactor(0.7)
        }
        .foregroundStyle(.white)
        .padding(.horizontal, 12)
    }
}
