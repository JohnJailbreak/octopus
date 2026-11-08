//
//  Updater.swift
//  Octopus
//
//  Checks GitHub for a newer Octopus and installs it with TrollStore. This is Speaka's updater (SpeakaUpdater.swift)
//  without the tweak: Caz's UpdateModel, which follows Dopamine's updater (DOUIManager, DOUpdateViewController,
//  DODownloadViewController).
//  Dopamine: Copyright (c) 2023-2024 Lars Fröder (opa334), MIT License (see LICENSE-Dopamine.txt).
//

import Combine
import Foundation
import OctopusSpawn
import UIKit

/// One GitHub release of Octopus: its title (the version), its notes (the changelog) and the app it carries.
struct OctopusRelease: Identifiable, Equatable {
    let id: Int
    let name: String
    let body: String
    let version: [Int]
    let tipaURL: URL?
}

final class Updater: ObservableObject {
    enum Stage: Equatable {
        case changelog
        case downloading
        case installing
        case installed
        case failed(String)
    }

    /// Settings > Check for Updates, and the line under it.
    enum ManualCheck: Equatable {
        case idle
        case checking
        case upToDate(String)   // the version that's installed
        case failed(String)     // why: "you're offline", GitHub's limit, ...
    }

    /// What a check ends in: an update to show, nothing newer, or why it couldn't.
    enum CheckResult: Equatable {
        case available
        case upToDate
        case failed(String)
    }

    static let apiURL = URL(string: "https://api.github.com/repos/m9z2w4d8vw-blip/octopus/releases")!
    static let releasesPage = URL(string: "https://github.com/m9z2w4d8vw-blip/octopus/releases")!

    /// Where a download is saved, tried in order: outside the app's own data folder, as in Speaka, so the root helper
    /// can read it. These are also the only folders the helper takes an update from (Octopus/RootHelper/main.c,
    /// k_update_dirs); Octopus/Tests/RepoTests keeps the two lists the same.
    static let downloadFolders = ["/var/mobile/Library/Caches/Octopus", "/var/tmp/Octopus"]
    static let downloadName = "Octopus-update.tipa"

    /// Releases newer than this app, newest first: the changelog.
    @Published private(set) var updates: [OctopusRelease] = []
    /// The newest release whose app is newer than this one, if any.
    @Published private(set) var available: OctopusRelease? = nil
    @Published var stage: Stage = .changelog
    @Published private(set) var manual: ManualCheck = .idle
    /// When Octopus last got an answer from GitHub.
    @Published private(set) var lastChecked: Date? = Preferences.lastUpdateCheck
    /// What the circle shows. It eases toward the real progress like Caz's (25% of the gap per frame).
    @Published private(set) var progress: Double = 0

    /// The download goes into memory (Octopus.tipa is a few MB) and is written out to `downloadFolders`, so nothing is
    /// written in the app's data folder, as in Speaka. No cache either.
    private static let session = URLSession(configuration: .ephemeral)

    private var target: Double = 0
    private var timer: Timer?
    private var task: URLSessionTask?
    private var checking = false
    private var checked = false
    private var waiting: [(CheckResult) -> Void] = []
    private var lastFetch: Date? = nil
    /// Settings > Update notifications, while Octopus is open: the check again every 2 minutes.
    private var watchTimer: Timer?

    var isAvailable: Bool { available != nil }
    var isBusy: Bool { stage == .downloading || stage == .installing }

    func watch(_ on: Bool) {
        watchTimer?.invalidate()
        watchTimer = nil
        guard on else { return }
        watchTimer = Timer.scheduledTimer(withTimeInterval: UpdateNotifications.interval, repeats: true) { [weak self] _ in
            self?.check(again: true)
        }
    }

    // MARK: - Checking

    /// Fetches the releases once per launch, or `again`. Drafts and pre-releases are ignored, as in Caz and Dopamine.
    /// Every caller of a check in flight gets its answer.
    func check(again: Bool = false, completion: ((CheckResult) -> Void)? = nil) {
        if let completion { waiting.append(completion) }
        guard !checking else { return }
        guard again || !checked else {
            finishCheck(isAvailable ? .available : .upToDate)
            return
        }
        checking = true
        Self.fetchReleases { [weak self] outcome in
            DispatchQueue.main.async {
                guard let self else { return }
                self.checking = false
                self.lastFetch = Date()
                switch outcome {
                case .failure(let failure):
                    self.finishCheck(.failed(failure.reason))   // offline or rate limited: the next check tries again
                case .success(let releases):
                    self.checked = true
                    self.lastChecked = Preferences.lastUpdateCheck
                    self.choose(from: releases, app: Self.versionParts(AppInfo.shortVersion))
                    self.finishCheck(self.isAvailable ? .available : .upToDate)
                }
            }
        }
    }

    /// When Octopus comes to the front: a fresh check, so the Update Available a notification promised is there at
    /// once, unless one answered in the last 20 s (switching apps back and forth).
    func checkOnActivation() {
        refreshLastChecked()
        if let lastFetch, Date().timeIntervalSince(lastFetch) < 20 { return }
        check(again: true)
    }

    /// Settings > Check for Updates: always a fresh check; its outcome is the line under the button.
    func checkNow(completion: ((CheckResult) -> Void)? = nil) {
        guard manual != .checking else {
            if let completion { waiting.append(completion) }
            return
        }
        manual = .checking
        check(again: true) { [weak self] result in
            guard let self else { return }
            switch result {
            case .available: self.manual = .idle
            case .upToDate: self.manual = .upToDate(AppInfo.shortVersion)
            case .failed(let reason): self.manual = .failed(reason)
            }
            completion?(result)
        }
    }

    func refreshLastChecked() {
        let when = Preferences.lastUpdateCheck
        if when != lastChecked { lastChecked = when }
    }

    private func finishCheck(_ result: CheckResult) {
        let callers = waiting
        waiting = []
        callers.forEach { $0(result) }
    }

    func choose(from releases: [OctopusRelease], app: [Int]) {
        let chosen = Self.select(from: releases, app: app)
        if chosen.available != available { available = chosen.available }
        if chosen.updates != updates { updates = chosen.updates }
    }

    /// The newest release that carries the app, if it's newer than `app`, and every release newer than `app`, newest
    /// first, for the changelog.
    static func select(from releases: [OctopusRelease], app: [Int]) -> (available: OctopusRelease?, updates: [OctopusRelease]) {
        let newest = releases.sorted { compare($0.version, $1.version) > 0 }
        var available: OctopusRelease? = nil
        if let release = newest.first(where: { $0.tipaURL != nil }), compare(release.version, app) > 0 {
            available = release
        }
        return (available, newest.filter { compare($0.version, app) > 0 })
    }

    // MARK: - The release list, from GitHub

    /// Every check goes through here: the app's, its background one and Settings > Check for Updates. It asks with the
    /// last answer's ETag: an unchanged list comes back as 304, which doesn't count against GitHub's 60 unauthenticated
    /// requests an hour, and the answer kept in memory is used. A failure says why, for the line under Check for Updates.
    private static let api: URLSession = {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        return URLSession(configuration: configuration)
    }()
    private static let answerLock = NSLock()
    private static var lastAnswer: (etag: String, body: Data)? = nil

    static func fetchReleases(completion: @escaping (Result<[OctopusRelease], FetchFailure>) -> Void) {
        var request = URLRequest(url: apiURL, timeoutInterval: 20)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        answerLock.lock(); let previous = lastAnswer; answerLock.unlock()
        if let previous { request.setValue(previous.etag, forHTTPHeaderField: "If-None-Match") }
        api.dataTask(with: request) { data, response, error in
            if let error = error as? URLError {
                completion(.failure(FetchFailure(Self.describe(error))))
                return
            }
            guard let http = response as? HTTPURLResponse else {
                completion(.failure(FetchFailure(error?.localizedDescription ?? "GitHub didn't answer")))
                return
            }
            var body = data
            switch http.statusCode {
            case 200:
                if let data, let etag = http.value(forHTTPHeaderField: "ETag") {
                    Self.answerLock.lock(); Self.lastAnswer = (etag, data); Self.answerLock.unlock()
                }
            case 304 where previous != nil:
                body = previous?.body
            default:
                completion(.failure(FetchFailure(Self.describe(http))))
                return
            }
            guard let body, let releases = Self.parse(body) else {
                completion(.failure(FetchFailure("GitHub's answer couldn't be read")))
                return
            }
            Preferences.lastUpdateCheck = Date()
            completion(.success(releases))
        }.resume()
    }

    struct FetchFailure: Error {
        let reason: String
        init(_ reason: String) { self.reason = reason }
    }

    static func describe(_ error: URLError) -> String {
        switch error.code {
        case .notConnectedToInternet, .networkConnectionLost, .dataNotAllowed, .internationalRoamingOff:
            return "you're offline"
        case .timedOut:
            return "GitHub didn't answer"
        case .cannotFindHost, .cannotConnectToHost, .dnsLookupFailed:
            return "GitHub couldn't be reached"
        default:
            return error.localizedDescription
        }
    }

    static func describe(_ response: HTTPURLResponse) -> String {
        if response.statusCode == 403 || response.statusCode == 429,
           response.value(forHTTPHeaderField: "X-RateLimit-Remaining") == "0" {
            if let reset = response.value(forHTTPHeaderField: "X-RateLimit-Reset").flatMap(Double.init) {
                let time = DateFormatter.localizedString(from: Date(timeIntervalSince1970: reset), dateStyle: .none,
                                                         timeStyle: .short)
                return "GitHub's hourly limit is used up until \(time)"
            }
            return "GitHub's hourly limit is used up"
        }
        return "GitHub answered \(response.statusCode)"
    }

    /// An error as text, with its domain and code (and the POSIX error under it, if any), so a report pins it down.
    static func describe(_ error: Error) -> String {
        let ns = error as NSError
        var text = "\(ns.localizedDescription) (\(ns.domain) \(ns.code)"
        if let under = ns.userInfo[NSUnderlyingErrorKey] as? NSError {
            text += ", \(under.domain) \(under.code)"
        }
        return text + ")"
    }

    private struct APIRelease: Decodable {
        struct Asset: Decodable {
            let name: String
            let browserDownloadUrl: String
        }
        let id: Int
        let tagName: String
        let name: String?
        let body: String?
        let draft: Bool
        let prerelease: Bool
        let assets: [Asset]
    }

    static func parse(_ data: Data) -> [OctopusRelease]? {
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        guard let list = try? decoder.decode([APIRelease].self, from: data) else { return nil }
        return list.compactMap { release -> OctopusRelease? in
            if release.draft || release.prerelease { return nil }
            let version = versionParts(release.tagName)
            if version.isEmpty { return nil }
            let title = release.name.flatMap { $0.isEmpty ? nil : $0 } ?? release.tagName
            return OctopusRelease(id: release.id, name: title, body: release.body ?? "", version: version,
                                  tipaURL: link(in: release.assets, ending: ".tipa") ?? link(in: release.assets, ending: ".ipa"))
        }
    }

    private static func link(in assets: [APIRelease.Asset], ending suffix: String) -> URL? {
        guard let asset = assets.first(where: { $0.name.lowercased().hasSuffix(suffix) }) else { return nil }
        return URL(string: asset.browserDownloadUrl)
    }

    /// "v0.2.57" -> [0, 2, 57]
    static func versionParts(_ text: String) -> [Int] {
        text.split(whereSeparator: { !$0.isASCII || !$0.isNumber }).compactMap { Int($0) }
    }

    static func compare(_ a: [Int], _ b: [Int]) -> Int {
        for index in 0..<max(a.count, b.count) {
            let x = index < a.count ? a[index] : 0
            let y = index < b.count ? b[index] : 0
            if x != y { return x < y ? -1 : 1 }
        }
        return 0
    }

    // MARK: - Downloading and installing

    /// Downloads the newer app and hands it to the root helper, which installs it with TrollStore. If that fails,
    /// TrollStore is asked to install it from its link instead, as in Caz.
    func startDownload() {
        guard let url = available?.tipaURL, !isBusy else { return }
        stage = .downloading
        progress = 0
        target = 0
        startTimer()
        let dataTask = Self.session.dataTask(with: url) { [weak self] data, response, error in
            var file: URL? = nil
            var problem: String? = nil
            if let error {
                problem = "Downloading the update: \(Self.describe(error))"
            } else if let http = response as? HTTPURLResponse, http.statusCode != 200 {
                problem = "Downloading the update: GitHub answered with error \(http.statusCode)."
            } else if let data, !data.isEmpty {
                let saving = Self.save(data, as: Self.downloadName)
                file = saving.file
                if file == nil { problem = "Saving the update: \(saving.problem)" }
            } else {
                problem = "Downloading the update: GitHub sent an empty file."
            }
            let saved = file
            let reason = problem
            DispatchQueue.main.async {
                guard let self else { return }
                guard let saved else {
                    self.fail(reason ?? "The download didn't finish.")
                    return
                }
                self.install(saved)
            }
        }
        task = dataTask
        dataTask.resume()
    }

    /// Writes a download into the first of `downloadFolders` that takes it. The problem names every folder tried.
    static func save(_ data: Data, as name: String) -> (file: URL?, problem: String) {
        var problems: [String] = []
        for folder in downloadFolders {
            let directory = URL(fileURLWithPath: folder, isDirectory: true)
            let file = directory.appendingPathComponent(name)
            do {
                try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
                try? FileManager.default.removeItem(at: file)
                try data.write(to: file)
                return (file, "")
            } catch {
                problems.append("\(folder): \(describe(error))")
            }
        }
        return (nil, problems.joined(separator: "; "))
    }

    private func install(_ file: URL) {
        target = 1
        stage = .installing
        let helper = Bundle.main.bundlePath + "/octopushelper"
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let outcome = RootHelper.run(helper, ["install-update", "--tipa", file.path])
            DispatchQueue.main.async {
                self?.installed(outcome)
            }
        }
    }

    /// The helper's exit codes: 0 done, 2 usage, 3 not root, 4 the download is missing, 5 TrollStore's installer
    /// wasn't found, 6 it couldn't start, 7 it failed; -1 the helper didn't start, -2 it was killed. Installing usually
    /// closes Octopus before the answer arrives.
    private func installed(_ outcome: RootHelper.Outcome) {
        if outcome.status == 0 {
            timer?.invalidate()
            progress = 1
            stage = .installed
            return
        }
        let reason = outcome.errors.last ?? "The update helper stopped with code \(outcome.status)"
        fail("\(Self.sentence(reason)) TrollStore will install it from its link instead.")
        openInTrollStore()
    }

    /// A reason as a sentence: with a full stop, unless it already ends like one.
    static func sentence(_ text: String) -> String {
        guard let last = text.last else { return text }
        return ".?!".contains(last) ? text : text + "."
    }

    /// 60 times a second: read the real progress and move the circle a quarter of the way there.
    private func startTimer() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { [weak self] tick in
            guard let self else {
                tick.invalidate()
                return
            }
            if self.progress >= 0.99 {
                self.progress = 1
                tick.invalidate()
                return
            }
            if let task = self.task, task.countOfBytesExpectedToReceive > 0 {
                let real = Double(task.countOfBytesReceived) / Double(task.countOfBytesExpectedToReceive)
                self.target = max(self.target, min(1, real))
            }
            if self.progress < self.target {
                self.progress += (self.target - self.progress) * 0.25
            }
        }
    }

    private func fail(_ message: String) {
        timer?.invalidate()
        stage = .failed(message)
    }

    /// TrollStore installs from a link with apple-magnifier://install?url=, the same hand-off Caz, Speaka and Dopamine
    /// use. Without TrollStore, the releases page opens instead.
    func openInTrollStore() {
        guard let url = available?.tipaURL,
              let link = URL(string: "apple-magnifier://install?url=" + url.absoluteString) else {
            UIApplication.shared.open(Self.releasesPage)
            return
        }
        UIApplication.shared.open(link, options: [:]) { opened in
            if !opened { UIApplication.shared.open(Updater.releasesPage) }
        }
    }

    /// Back to the changelog when the window closes, unless something is still running.
    func resetIfIdle() {
        if !isBusy { stage = .changelog }
    }
}

/// Octopus's root helper (Octopus/RootHelper/main.c), started as root through TrollStore's persona spawn.
enum RootHelper {
    struct Outcome {
        let status: Int32
        let lines: [String]

        /// The helper's ERR lines: why it failed, for the update window.
        var errors: [String] {
            lines.filter { $0.hasPrefix("ERR\t") }.map { String($0.dropFirst(4)) }
        }
    }

    static func run(_ path: String, _ arguments: [String]) -> Outcome {
        let strings = ([path] + arguments).map { strdup($0) }
        defer { strings.forEach { free($0) } }
        var argv: [UnsafePointer<CChar>?] = strings.map { $0.map { UnsafePointer($0) } }
        argv.append(nil)
        var output: UnsafeMutablePointer<CChar>? = nil
        let status = oct_spawn(path, argv, 1, &output)
        let text = output.map { String(cString: $0) } ?? ""
        free(output)
        return Outcome(status: status, lines: text.split(separator: "\n").map { String($0) })
    }
}
