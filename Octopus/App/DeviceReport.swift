//
//  DeviceReport.swift
//  Octopus
//
//  Debug Log > Report: a read-only look at how this iPhone presents itself over USB, written to the debug log.
//  It reads three system property lists found in the iOS 17 filesystem (docs/CHANGELOG.md, v0.3) with public
//  Foundation APIs and changes nothing: the USB device configurations, the PTP DeviceInfo dataset that ptpd
//  answers with, and ptpd's launch description. Their structure isn't documented, so the report flattens what
//  it finds into key = value lines, bounded, with anything that looks like a serial number redacted.
//

import Foundation

enum DeviceReport {
    static let sources: [(event: String, path: String)] = [
        ("usb_configuration", "/System/Library/AppleUSBDevice/USBDeviceConfiguration.plist"),
        ("ptp_device_info", "/System/Library/ApplePTP/PTPDeviceInfoDataset.plist"),
        ("ptpd_launch", "/System/Library/LaunchDaemons/com.apple.ptpd.plist"),
    ]
    static let maxLinesPerFile = 150
    private static let redacted = ["serial", "udid", "ecid", "imei", "meid", "uniquechipid"]

    /// Logs the report and returns how many values it logged.
    @discardableResult
    static func collect() -> Int {
        DebugLog.log(.info, "device", "report", "Device report for \(DebugLog.systemDescription)")
        var total = 0
        for source in sources {
            total += summarize(event: source.event, path: source.path)
        }
        DebugLog.log(.info, "device", "report_done", "Device report finished: \(total) values from \(sources.count) files")
        return total
    }

    static func summarize(event: String, path: String) -> Int {
        guard let data = FileManager.default.contents(atPath: path) else {
            DebugLog.log(.info, "device", event, "\(path): not present or not readable")
            return 0
        }
        guard let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) else {
            DebugLog.log(.warning, "device", event, "\(path): \(data.count) bytes, not a property list")
            return 0
        }
        var lines: [String] = []
        flatten(plist, path: "", depth: 0, into: &lines)
        DebugLog.log(.info, "device", event, "\(path): \(data.count) bytes, \(lines.count) values")
        for line in lines.prefix(maxLinesPerFile) {
            DebugLog.log(.info, "device", event, line)
        }
        if lines.count > maxLinesPerFile {
            DebugLog.log(.info, "device", event, "\(lines.count - maxLinesPerFile) more values not logged")
        }
        return min(lines.count, maxLinesPerFile)
    }

    /// key.path = value lines. Numbers show in decimal and hex, as USB classes and PTP codes read best in hex.
    static func flatten(_ value: Any, path: String, depth: Int, into lines: inout [String]) {
        guard lines.count < 1000 else { return }
        let lower = path.lowercased()
        if redacted.contains(where: { lower.contains($0) }) {
            lines.append("\(path) = <redacted>")
            return
        }
        if depth > 8 {
            lines.append("\(path) = <nested deeper than 8 levels>")
            return
        }
        switch value {
        case let dictionary as [String: Any]:
            if dictionary.isEmpty { lines.append("\(path) = {}") }
            for key in dictionary.keys.sorted() {
                flatten(dictionary[key] as Any, path: path.isEmpty ? key : "\(path).\(key)", depth: depth + 1, into: &lines)
            }
        case let array as [Any]:
            if !array.isEmpty, array.allSatisfy({ ($0 as? NSNumber).map { !isBoolean($0) } ?? false }) {
                let numbers = array.prefix(64).map { describe($0 as! NSNumber) }
                lines.append("\(path) = [\(numbers.joined(separator: ", "))\(array.count > 64 ? ", ..." : "")] (\(array.count))")
            } else {
                if array.isEmpty { lines.append("\(path) = []") }
                for (index, item) in array.enumerated() {
                    flatten(item, path: "\(path)[\(index)]", depth: depth + 1, into: &lines)
                }
            }
        case let number as NSNumber:
            // A plist boolean is a CFBoolean; an integer 1 must not read as "true".
            lines.append("\(path) = \(isBoolean(number) ? (number.boolValue ? "true" : "false") : describe(number))")
        case let string as String:
            lines.append("\(path) = \"\(string.prefix(120))\"")
        case let data as Data:
            lines.append("\(path) = <\(data.count) bytes>")
        case let date as Date:
            lines.append("\(path) = \(date)")
        default:
            lines.append("\(path) = <\(type(of: value))>")
        }
    }

    private static func isBoolean(_ number: NSNumber) -> Bool {
        CFGetTypeID(number as CFTypeRef) == CFBooleanGetTypeID()
    }

    private static func describe(_ number: NSNumber) -> String {
        let value = number.int64Value
        if Double(value) == number.doubleValue, value >= 0 {
            return value > 9 ? "\(value) (0x\(String(value, radix: 16, uppercase: true)))" : "\(value)"
        }
        return number.stringValue
    }
}
