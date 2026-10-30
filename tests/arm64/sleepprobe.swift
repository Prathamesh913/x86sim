// Speed 9: do Swift Concurrency / dispatch timers wait as long as asked under the A64 JIT? (Settings' search
// placeholder task spun the main thread at ~10k wakeups/s.) Prints the measured wait for each clock.
import Foundation
func now() -> Double { var t = timespec(); clock_gettime(CLOCK_MONOTONIC, &t); return Double(t.tv_sec) + Double(t.tv_nsec) / 1e9 }
func show(_ what: String, _ t0: Double) { print(String(format: "%-38@ %.3f s (want 0.300)", what as NSString, now() - t0)) }
let done = DispatchSemaphore(value: 0)
Task.detached {
	print("cancelled at start:", Task.isCancelled)
	var t = now(); do { try await Task.sleep(nanoseconds: 300_000_000) } catch { print("threw", error) }; show("Task.sleep(nanoseconds:)", t)
	t = now(); do { try await Task.sleep(for: .milliseconds(300)) } catch { print("threw", error) }; show("Task.sleep(for:) continuous", t)
	t = now(); do { try await Task.sleep(until: .now + .milliseconds(300), clock: .suspending) } catch { print("threw", error) }; show("Task.sleep(until:) suspending", t)
	t = now(); do { try await Task.sleep(until: .now + .milliseconds(300), clock: .continuous) } catch { print("threw", error) }; show("Task.sleep(until:) continuous", t)
	print("SuspendingClock.now", SuspendingClock.now, "ContinuousClock.now", ContinuousClock.now)
	done.signal()
}
done.wait()
var t = now()
DispatchQueue.global().asyncAfter(deadline: .now() + 0.3) { show("asyncAfter(deadline:)", t); done.signal() }
done.wait()
t = now()
DispatchQueue.global().asyncAfter(wallDeadline: .now() + 0.3) { show("asyncAfter(wallDeadline:)", t); done.signal() }
done.wait()
t = now(); Thread.sleep(forTimeInterval: 0.3); show("Thread.sleep", t)
t = now(); RunLoop.current.run(until: Date().addingTimeInterval(0.3)); show("RunLoop.run(until:)", t)
