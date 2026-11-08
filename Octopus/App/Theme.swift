import SwiftUI
import UIKit

/// Colours from the Caz UI specification (build 1.0.15).
enum Theme {
    /// Menu panels, the primary button and the activity sheet: #3F3F3F at
    /// 114/255. The specification's samples show a flat fill with no blur.
    static let menu = Color(.sRGB, red: 63 / 255, green: 63 / 255, blue: 63 / 255, opacity: 114 / 255)
    /// Windows: #4C4C4C at 153/255.
    static let window = Color(.sRGB, red: 76 / 255, green: 76 / 255, blue: 76 / 255, opacity: 153 / 255)
    static let secondary = Color.white.opacity(0.8)
    static let foreground = Color.white.opacity(0.75)
    static let chevron = Color.white.opacity(0.6)
    static let separator = Color(white: 0.6).opacity(0.3)
    static let hairline = Color.white.opacity(0.1)
    static let accent = Color(.sRGB, red: 1, green: 178 / 255, blue: 30 / 255, opacity: 1)
    static let failure = Color(.sRGB, red: 1, green: 158 / 255, blue: 143 / 255, opacity: 1)
}

/// The wordmark is live text in the bundled Fonarto Regular, sized so its
/// capitals match Caz's 30.5 pt. At this size "Caz" measures the specified
/// 76.7 pt of ink, which is how the size was derived from the font file.
enum Wordmark {
    static let font = "Fonarto-Regular"
    static let size: CGFloat = 43.57
    static let tracking: CGFloat = -0.283 // -0.65 percent of the em
    static let baseline: CGFloat = 24.74  // below the top of the 40 pt title slot
}

enum Motion {
    static let sheet = Animation.spring(response: 0.75, dampingFraction: 0.9)
    static let window = Animation.spring(response: 0.6, dampingFraction: 0.9)
    static let dim = Animation.easeInOut(duration: 0.3)
    static let press = Animation.easeOut(duration: 0.15)
    static let lineAppear = Animation.easeOut(duration: 0.3)
    static let lineFinish = Animation.easeOut(duration: 0.2)
}

/// Main-screen geometry in full-screen points, following the specification.
/// On an iPhone 13 (390 x 844) this places the menu at y 306.7 and the button
/// at y 697.1, matching the reference screenshots.
struct Layout {
    let size: CGSize
    let insets: EdgeInsets

    var hasHomeButton: Bool { insets.bottom == 0 }
    var isShort: Bool { size.height < 618 }
    var columnWidth: CGFloat { max(0, size.width - 60) }
    var columnHeight: CGFloat { size.height * (hasHomeButton ? 0.78 : 0.73) }
    var columnTop: CGFloat { size.height / 2 + max(15, insets.top - 20) - columnHeight / 2 }
    var headerHeight: CGFloat { 77.4 }
    var rowHeight: CGFloat { isShort ? 52 : (hasHomeButton ? 65 : 73) }
    var menuSidePadding: CGFloat { isShort ? 10 : 20 }
    var menuHeight: CGFloat { rowHeight * 4 + 10 }
    var buttonHeight: CGFloat { 60 }
    var gap: CGFloat { max(12, (columnHeight - headerHeight - menuHeight - buttonHeight) / 2) }
    var menuTop: CGFloat { columnTop + headerHeight + gap }
    var buttonTop: CGFloat { menuTop + menuHeight + gap }
    /// Caz's Update Available: 30 pt tall, 20 pt above the button (10 on home-button phones).
    var updateLinkHeight: CGFloat { 30 }
    var updateLinkTop: CGFloat { buttonTop - (hasHomeButton ? 10 : 20) - updateLinkHeight }
    var sheetTop: CGFloat { size.height * 0.26 + 35 }
    var sheetHeight: CGFloat { size.height - sheetTop + 100 }
    var logBottomInset: CGFloat { hasHomeButton ? 10 : 40 }
    var windowSize: CGSize { CGSize(width: columnWidth, height: size.height * (isShort ? 0.8 : 0.7)) }
}

/// Pressed controls dim to 45 percent with a 0.15 s ease-out.
struct PressDimStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .opacity(configuration.isPressed ? 0.45 : 1)
            .animation(Motion.press, value: configuration.isPressed)
    }
}

enum Preferences {
    static let haptics = "haptics"
    static let selfTestOnEnable = "selfTestOnEnable"
    static let scratchFolder = "scratchFolder"
    /// Settings > Update notifications.
    static let updateNotifications = "updateNotifications"
    private static let notifiedVersionKey = "notifiedVersion"
    private static let lastUpdateCheckKey = "lastUpdateCheck"

    static func bool(_ key: String, default fallback: Bool) -> Bool {
        UserDefaults.standard.object(forKey: key) as? Bool ?? fallback
    }

    static var updateNotificationsEnabled: Bool {
        get { bool(updateNotifications, default: false) }
        set { UserDefaults.standard.set(newValue, forKey: updateNotifications) }
    }

    /// The last release a notification announced, so each is announced once.
    static var notifiedVersion: String? {
        get { UserDefaults.standard.string(forKey: notifiedVersionKey) }
        set { UserDefaults.standard.set(newValue, forKey: notifiedVersionKey) }
    }

    /// When Octopus last got an answer from GitHub about its releases.
    static var lastUpdateCheck: Date? {
        get { UserDefaults.standard.object(forKey: lastUpdateCheckKey) as? Date }
        set { UserDefaults.standard.set(newValue, forKey: lastUpdateCheckKey) }
    }
}

@MainActor
enum Haptics {
    private static var enabled: Bool { Preferences.bool(Preferences.haptics, default: true) }

    static func light() {
        guard enabled else { return }
        UIImpactFeedbackGenerator(style: .light).impactOccurred()
    }

    static func success() {
        guard enabled else { return }
        UINotificationFeedbackGenerator().notificationOccurred(.success)
    }

    static func failure() {
        guard enabled else { return }
        UINotificationFeedbackGenerator().notificationOccurred(.error)
    }
}

enum AppInfo {
    /// The release's version, "0.2.42", which the updater compares with GitHub's releases.
    static var shortVersion: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0"
    }

    static var version: String {
        let info = Bundle.main.infoDictionary ?? [:]
        let short = info["CFBundleShortVersionString"] as? String ?? "0.1"
        let build = info["CFBundleVersion"] as? String ?? "1"
        return "\(short) (\(build))"
    }

    static let repository = URL(string: "https://github.com/m9z2w4d8vw-blip/octopus")!
}

extension UInt64 {
    var byteString: String {
        ByteCountFormatter.string(fromByteCount: Int64(clamping: self), countStyle: .file)
    }
}
