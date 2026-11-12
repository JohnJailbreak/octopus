import SwiftUI
import UserNotifications

@main
struct OctopusApp: App {
    @StateObject private var service = ServiceController()
    @StateObject private var updater = Updater()
    @StateObject private var diagnostics = DiagnosticsRunner()

    init() {
        // The debug log first, so everything after it is recorded (DebugLog, Octopus/Core/oct_log.c).
        DebugLog.start()
        // Banners while Octopus is open, and a tap on an update notification opens the update window.
        UNUserNotificationCenter.current().delegate = OctopusNotificationDelegate.shared
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(service)
                .environmentObject(updater)
                .environmentObject(diagnostics)
                .preferredColorScheme(.dark)
        }
        // Settings > Update notifications: iOS wakes Octopus in the background when it chooses (UpdateNotifications).
        .backgroundTask(.appRefresh(UpdateNotifications.taskID)) {
            await UpdateNotifications.backgroundCheck()
        }
    }
}

/// Measures the whole screen. The specification places every element in
/// full-screen points, so the main screen lays itself out in that space and
/// applies the safe-area rules itself.
struct RootView: View {
    @Environment(\.scenePhase) private var scenePhase
    var body: some View {
        GeometryReader { proxy in
            MainScreen(layout: Layout(size: proxy.size, insets: proxy.safeAreaInsets))
        }
        .ignoresSafeArea()
        .onChange(of: scenePhase) { _, phase in
            NativeIntegration.shared.setForeground(phase == .active)
        }
    }
}
