// x86Sim test apps: shared result log. Every check prints "X86TEST <app> <check> PASS|FAIL <detail>" to stderr
// (launchd_sim's app log) and appends it to Documents/x86test-results.txt (tests/apps/run.sh reads that file).
import Foundation

enum X86Test {
    static let app = Bundle.main.bundleIdentifier?.components(separatedBy: ".").last ?? "app"
    static var file: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("x86test-results.txt")
    }
    static func reset() { try? FileManager.default.removeItem(at: file); log("start", true, ProcessInfo.processInfo.operatingSystemVersionString) }
    static func log(_ check: String, _ ok: Bool, _ detail: String = "") {
        let line = "X86TEST \(app) \(check) \(ok ? "PASS" : "FAIL") \(detail)\n"
        FileHandle.standardError.write(line.data(using: .utf8)!)
        NSLog("%@", line)
        if let h = try? FileHandle(forWritingTo: file) { h.seekToEndOfFile(); h.write(line.data(using: .utf8)!); try? h.close() }
        else { try? line.data(using: .utf8)!.write(to: file) }
    }
    static func done() { log("done", true) }
}
