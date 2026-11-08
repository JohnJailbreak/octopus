//
//  UpdateNotifications.swift
//  Octopus
//
//  Settings > Update notifications: a notification when a new release is ready. Speaka's, without its launchd checker,
//  which comes with Speaka's tweak; Octopus is only an app.
//

import BackgroundTasks
import Foundation
import UserNotifications

/// Two ways a release is found, one rule: each release is announced once, whichever finds it first
/// (Preferences.notifiedVersion).
/// - Octopus's background check, when iOS chooses to run it: often hours apart, and never once Octopus was swiped away
///   or while Background App Refresh is off.
/// - While Octopus is open: every 2 minutes, and each time it comes to the front (Updater).
/// A release is announced only while iOS allows Octopus's notifications, and counts as announced only once iOS has
/// accepted the request, so a release found while they're off is announced when they're back on. Tapping the
/// notification opens the update (OctopusNotificationDelegate).
enum UpdateNotifications {
    /// Listed in Info.plist under BGTaskSchedulerPermittedIdentifiers (project.yml).
    static let taskID = "octopus.update-check"
    /// While Octopus is open (Updater.watch). Each check is conditional, so an unchanged list costs nothing against
    /// GitHub's hourly limit.
    static let interval: TimeInterval = 120
    /// What Octopus asks iOS for in the background. iOS takes it as "not before", and decides.
    static let backgroundInterval: TimeInterval = 15 * 60

    /// Turning it on asks iOS for permission to notify first; if that's refused, it stays off. `completion` gets
    /// whether it ended up on, on the main thread.
    static func setEnabled(_ on: Bool, completion: @escaping @MainActor (Bool) -> Void) {
        guard on else {
            Preferences.updateNotificationsEnabled = false
            BGTaskScheduler.shared.cancel(taskRequestWithIdentifier: taskID)
            Task { @MainActor in completion(false) }
            return
        }
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { granted, _ in
            Task { @MainActor in
                Preferences.updateNotificationsEnabled = granted
                if granted { UpdateNotifications.schedule() }
                completion(granted)
            }
        }
    }

    /// iOS shows Octopus's notifications: authorized, provisional or ephemeral.
    static func shows(_ status: UNAuthorizationStatus) -> Bool {
        switch status {
        case .authorized, .provisional, .ephemeral: return true
        default: return false
        }
    }

    /// When Octopus comes to the front: notifications turned off in iOS Settings turn the switch off too.
    static func syncPermission() {
        guard Preferences.updateNotificationsEnabled else { return }
        UNUserNotificationCenter.current().getNotificationSettings { settings in
            let allowed = UpdateNotifications.shows(settings.authorizationStatus)
            DispatchQueue.main.async {
                if !allowed { Preferences.updateNotificationsEnabled = false }
            }
        }
    }

    /// Asks iOS to wake Octopus in the background in 15 minutes, or when it allows. If Background App Refresh is off
    /// for Octopus, or Low Power Mode is on, iOS refuses and only the checks while Octopus is open remain.
    static func schedule() {
        let request = BGAppRefreshTaskRequest(identifier: taskID)
        request.earliestBeginDate = Date(timeIntervalSinceNow: backgroundInterval)
        do {
            try BGTaskScheduler.shared.submit(request)
        } catch {
            // Refused for now; the next time Octopus goes to the background asks again.
        }
    }

    /// The background wake-up: the next one is asked for first, so one failed check doesn't end the chain.
    static func backgroundCheck() async {
        guard Preferences.updateNotificationsEnabled else { return }
        schedule()
        await check()
    }

    /// One check: the newest release that carries the app, if it's newer than this app and wasn't announced, and only
    /// while iOS allows Octopus's notifications.
    static func check() async {
        let releases: [OctopusRelease]? = await withCheckedContinuation { continuation in
            Updater.fetchReleases { result in continuation.resume(returning: try? result.get()) }
        }
        guard let releases else { return }
        let app = Updater.versionParts(AppInfo.shortVersion)
        guard let newest = Updater.select(from: releases, app: app).available,
              Preferences.notifiedVersion != newest.name else { return }
        let settings = await UNUserNotificationCenter.current().notificationSettings()
        guard shows(settings.authorizationStatus) else { return }   // kept for when they're allowed
        let content = UNMutableNotificationContent()
        content.title = "Octopus \(newest.name) is ready"
        content.body = "Tap to update."
        content.sound = .default
        let note = UNNotificationRequest(identifier: "update-\(newest.name)", content: content, trigger: nil)
        do {
            try await UNUserNotificationCenter.current().add(note)
            Preferences.notifiedVersion = newest.name   // only once iOS has accepted it
        } catch {
            // Not now: the next check tries again.
        }
    }
}

/// Octopus's notifications show as banners while it's open, and tapping an update notification opens the update
/// window. Installed at launch (OctopusApp), so a tap that launches Octopus isn't missed.
final class OctopusNotificationDelegate: NSObject, UNUserNotificationCenterDelegate {
    static let shared = OctopusNotificationDelegate()
    static let openUpdate = Notification.Name("octopus.open-update")
    /// A tap that came before the main screen was up (it launched Octopus); the main screen takes it when it appears.
    static var openRequested = false

    func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification,
                                withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void) {
        completionHandler([.banner, .list, .sound])
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse,
                                withCompletionHandler completionHandler: @escaping () -> Void) {
        if response.notification.request.identifier.hasPrefix("update-") {
            DispatchQueue.main.async {
                OctopusNotificationDelegate.openRequested = true
                NotificationCenter.default.post(name: OctopusNotificationDelegate.openUpdate, object: nil)
            }
        }
        completionHandler()
    }
}
