// x86Code: does Swift concurrency run? (detached task, cooperative pool, MainActor hop, continuations resumed
// from GCD, Task.sleep, async let). Prints a timestamp per step; each step should take well under 1 s.
import Foundation
setvbuf(stdout, nil, _IOLBF, 0)
func log(_ s: String) { print(String(format: "%.3f ", Date().timeIntervalSince1970.truncatingRemainder(dividingBy: 1000)) + s) }
log("start")
DispatchQueue.global().async { log("gcd global ran") }
DispatchQueue.main.async { log("gcd main ran") }
Task.detached {
    log("detached task ran")
    await MainActor.run { log("MainActor.run ran") }
    for i in 0..<3 {
        let v: Int = await withCheckedContinuation { c in DispatchQueue.global().asyncAfter(deadline: .now() + 0.2) { c.resume(returning: i) } }
        log("continuation \(v) resumed (0.2 s timer)")
    }
    for i in 0..<3 { try? await Task.sleep(nanoseconds: 200_000_000); log("Task.sleep \(i) done") }
    async let a = Task.detached(priority: .userInitiated) { 1 }.value
    async let b = Task.detached(priority: .utility) { 2 }.value
    log("async let sum \(await a + b)")
    await withTaskGroup(of: Int.self) { g in for i in 0..<8 { g.addTask { i } }; var s = 0; for await x in g { s += x }; log("task group sum \(s)") }
    log("detached done")
}
Task { @MainActor in log("main task ran") }
RunLoop.main.run(until: Date() + Double(ProcessInfo.processInfo.environment["TASKPROBE_SECS"] ?? "8")!)
log("end")
