import SwiftUI

@main
struct OctopusApp: App {
    @StateObject private var service = ServiceController()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(service)
                .preferredColorScheme(.dark)
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
