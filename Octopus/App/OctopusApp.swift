import SwiftUI
import UserNotifications

@main
struct OctopusApp: App {
    @StateObject private var service = ServiceController()
    @StateObject private var updater = Updater()

    init() {
        // Banners while Octopus is open, and a tap on an update notification opens the update window.
        UNUserNotificationCenter.current().delegate = OctopusNotificationDelegate.shared
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(service)
                .environmentObject(updater)
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
    var body: some View {
        GeometryReader { proxy in
            MainScreen(layout: Layout(size: proxy.size, insets: proxy.safeAreaInsets))
        }
        .ignoresSafeArea()
    }
}
