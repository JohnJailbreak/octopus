//
//  DebugLog.swift
//  Octopus
//
//  The debug log the app and the C core share (Octopus/Core/oct_log.c): debug.log, which persists
//  across launches and rotates at 512 KB, keeping three older files. The app adds its own lines (launch,
//  requests, updates, the device report); the core logs the service, the self-test and every transaction
//  that passes through the transport adapter. Nothing logged contains file names, file contents or
//  credentials.
//

import Foundation
import OctopusCore

enum DebugLog {
    enum Level: Int, CaseIterable, Comparable, Sendable {
        case debug = 0, info, warning, error

        init?(token: Substring) {
            switch token {
            case "DEBUG": self = .debug
            case "INFO": self = .info
            case "WARN": self = .warning
            case "ERROR": self = .error
            default: return nil
            }
        }

        var raw: oct_log_level {
            switch self {
            case .debug: return OCT_LOG_DEBUG
            case .info: return OCT_LOG_INFO
            case .warning: return OCT_LOG_WARN
            case .error: return OCT_LOG_ERROR
            }
        }

        var title: String {
            switch self {
            case .debug: return "Debug"
            case .info: return "Info"
            case .warning: return "Warning"
            case .error: return "Error"
            }
        }

        static func < (a: Level, b: Level) -> Bool { a.rawValue < b.rawValue }
    }

    struct Entry: Identifiable, Equatable, Sendable {
        let id: Int
        let time: String       // 2026-11-08T19:51:02.123Z
        let level: Level
        let component: String
        let event: String
        let message: String
        let line: String
    }

    struct Stats: Equatable, Sendable {
        var fileOpen = false
        var path = ""
        var fileBytes: UInt64 = 0
        var lines: UInt64 = 0
        var rotations: UInt64 = 0
        var writeErrors: UInt64 = 0
        var lastError = ""
        var reopens: UInt64 = 0
    }

    static let fileName = "debug.log"
    static let maxBytes = 512 * 1024
    static let keep: UInt32 = 3
    /// The most lines the Debug Log window shows; the export always has everything.
    static let displayLimit = 3000

    /// Where the log may live, tried in order: the folder that worked last time, the app's own Library/Logs, then
    /// the folders the updater uses, which an unsandboxed TrollStore app can write even when its own data folder
    /// refuses (on iOS 17, /var/mobile/Library/Caches/Octopus takes it while Library/Logs can't be created).
    static var candidateFolders: [URL] {
        var folders: [URL] = []
        if let remembered = UserDefaults.standard.string(forKey: Preferences.debugLogFolder), !remembered.isEmpty {
            folders.append(URL(fileURLWithPath: remembered, isDirectory: true))
        }
        if let library = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask).first {
            folders.append(library.appendingPathComponent("Logs", isDirectory: true))
        }
        folders += Updater.downloadFolders.map { URL(fileURLWithPath: $0, isDirectory: true) }
        folders.append(URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true))
        var seen = Set<String>()
        return folders.filter { seen.insert($0.standardizedFileURL.path).inserted }
    }

    /// Opens the log in the first folder that takes it; with none, the log keeps its recent lines in memory.
    /// Without `folders`, the candidates are tried and the folder that takes the log is remembered for next time.
    @discardableResult
    static func start(in folders: [URL]? = nil, detailed: Bool = Preferences.detailedLoggingEnabled) -> String? {
        let level = detailed ? OCT_LOG_DEBUG : OCT_LOG_INFO
        let remember = folders == nil
        var opened: String? = nil
        var refusals: [String] = []
        for folder in folders ?? candidateFolders {
            var creation = ""
            do {
                try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            } catch {
                creation = " (creating the folder: \((error as NSError).localizedDescription))"
            }
            let file = folder.appendingPathComponent(fileName).path
            let status = file.withCString { path -> oct_status in
                var config = oct_log_config()
                config.path = path
                config.max_bytes = maxBytes
                config.keep = keep
                config.level = level
                return oct_log_open(&config)
            }
            if status == OCT_OK {
                opened = file
                if remember { UserDefaults.standard.set(folder.path, forKey: Preferences.debugLogFolder) }
                break
            }
            refusals.append("\(folder.path): \(stats().lastError)\(creation)")
        }
        if opened == nil {
            var config = oct_log_config()
            config.level = level
            oct_log_open(&config)
        }
        log(.info, "app", "launch", "Octopus \(AppInfo.version) on \(systemDescription), log level \(detailed ? "debug" : "info")")
        // A refused folder is expected when a later one takes the log: worth a line, not a warning.
        for refusal in refusals {
            log(opened == nil ? .warning : .info, "log", opened == nil ? "folder_refused" : "folder_unavailable", refusal)
        }
        if let opened, !refusals.isEmpty {
            log(.info, "log", "folder", "The log is at \(opened)")
        }
        if opened == nil {
            log(.error, "log", "memory_only", "No folder took the log file; only recent lines are kept, in memory")
        }
        return opened
    }

    static func log(_ level: Level, _ component: String, _ event: String, _ message: String) {
        oct_log_message(level.raw, component, event, message)
    }

    static func setDetailed(_ on: Bool) {
        oct_log_set_level(on ? OCT_LOG_DEBUG : OCT_LOG_INFO)
        log(.info, "log", "level", on ? "Detailed logging on: every transaction is logged" : "Detailed logging off")
    }

    /// Writes the log to storage; recreates the file if something removed it.
    static func flush() {
        oct_log_flush()
    }

    static func clear() {
        oct_log_clear()
        log(.info, "log", "cleared", "The debug log was cleared")
    }

    static func stats() -> Stats {
        var raw = oct_log_stats()
        oct_log_get_stats(&raw)
        return Stats(fileOpen: raw.file_open, path: text(raw.path), fileBytes: raw.file_bytes, lines: raw.lines,
                     rotations: raw.rotations, writeErrors: raw.write_errors, lastError: text(raw.last_error),
                     reopens: raw.reopens)
    }

    /// The log's files, oldest first: debug.3.log ... debug.log, those that exist.
    static func files() -> [URL] {
        var urls: [URL] = []
        for index in stride(from: Int(keep), through: 0, by: -1) {
            var buffer = [CChar](repeating: 0, count: 1100)
            if oct_log_file_path(UInt32(index), &buffer, buffer.count) {
                let path = String(cString: buffer)
                if FileManager.default.fileExists(atPath: path) { urls.append(URL(fileURLWithPath: path)) }
            }
        }
        return urls
    }

    /// Everything in the log, oldest first: the files, or the in-memory lines when there is no file.
    static func allText() -> String {
        let urls = files()
        if urls.isEmpty {
            var buffer = [CChar](repeating: 0, count: 200 * 520)
            oct_log_recent(&buffer, buffer.count)
            return String(cString: buffer)
        }
        return urls.compactMap { try? String(contentsOf: $0, encoding: .utf8) }.joined()
    }

    /// The newest `limit` entries, oldest first.
    static func entries(limit: Int = displayLimit) -> [Entry] {
        let lines = allText().split(separator: "\n", omittingEmptySubsequences: true).suffix(limit)
        return lines.enumerated().compactMap { offset, line in parse(line, id: offset) }
    }

    static func parse(_ line: String, id: Int) -> Entry? {
        parse(Substring(line), id: id)
    }

    static func parse(_ line: Substring, id: Int) -> Entry? {
        let parts = line.split(separator: " ", maxSplits: 4, omittingEmptySubsequences: true)
        guard parts.count >= 4, parts[0].count == 24, parts[0].hasSuffix("Z"), let level = Level(token: parts[1]) else {
            return nil
        }
        return Entry(id: id, time: String(parts[0]), level: level, component: String(parts[2]), event: String(parts[3]),
                     message: parts.count > 4 ? String(parts[4]) : "", line: String(line))
    }

    /// The most recent warning or error that isn't the self-test's rehearsal.
    static func lastFailure(in entries: [Entry]) -> Entry? {
        entries.last { $0.level >= .warning && $0.component != "selftest" }
    }

    /// One file with every line, for the share sheet: debug-20261108-195102.log, in the first of
    /// Diagnostics.exportFolders that takes it. When none does, the error gives each folder's reason.
    static func exportFile(in folders: [String] = Diagnostics.exportFolders) throws -> URL {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        let name = "debug-\(formatter.string(from: Date())).log"
        let header = "# Octopus \(AppInfo.version), \(systemDescription). Exported \(Date()).\n"
        let data = Data((header + allText()).utf8)
        var reasons: [String] = []
        for folder in folders {
            var reason = [CChar](repeating: 0, count: 1024)
            let capacity = reason.count
            guard oct_zip_prepare_directory(folder, &reason, capacity) == OCT_OK else {
                reasons.append(Diagnostics.string(reason))
                continue
            }
            let url = URL(fileURLWithPath: folder, isDirectory: true).appendingPathComponent(name)
            do {
                try data.write(to: url, options: .atomic)
                return url
            } catch {
                reasons.append("Writing \(url.path): \((error as NSError).localizedDescription)")
            }
        }
        throw Diagnostics.ArchiveError(reasons.isEmpty ? "No folder was given." : reasons.joined(separator: "; "))
    }

    static var systemDescription: String {
        var system = utsname()
        uname(&system)
        let machine = withUnsafeBytes(of: system.machine) { raw in
            String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
        let version = ProcessInfo.processInfo.operatingSystemVersion
        return "iOS \(version.majorVersion).\(version.minorVersion).\(version.patchVersion) (\(machine))"
    }

    private static func text<T>(_ tuple: T) -> String {
        withUnsafeBytes(of: tuple) { raw in String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self) }
    }
}
