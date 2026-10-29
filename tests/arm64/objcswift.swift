// Stage 0 test (c), Swift half: generics, protocol witness dispatch, class allocation + ARC; calls the ObjC half.
@_silgen_name("x86c_now_ns") func nowNs() -> UInt64
@_silgen_name("x86c_objc_msgsend") func objcMsgSend(_ n: UInt32) -> UInt64
@_silgen_name("x86c_objc_alloc") func objcAlloc(_ n: UInt32) -> UInt64

protocol Shape { func area() -> Double }
struct Sq: Shape { var s: Double; func area() -> Double { s * s } }
struct Circ: Shape { var r: Double; func area() -> Double { 3.14159 * r * r } }
final class Node { var v: Int; var next: Node?; init(_ v: Int, _ n: Node?) { self.v = v; next = n } }

@inline(never) func sumGeneric<T: Numeric>(_ xs: [T]) -> T { var s: T = 0; for x in xs { s += x }; return s }
@inline(never) func existential(_ n: Int) -> Double {
    var shapes: [any Shape] = []
    for i in 0..<64 { if i & 1 == 0 { shapes.append(Sq(s: Double(i))) } else { shapes.append(Circ(r: Double(i))) } }
    var t = 0.0; for i in 0..<n { t += shapes[i & 63].area() }; return t
}
@inline(never) func arcList(_ n: Int) -> Int {
    var total = 0
    for _ in 0..<(n / 1000) { var head: Node? = nil; for i in 0..<1000 { head = Node(i, head) }
        var p = head; while let q = p { total &+= q.v; p = q.next } }
    return total
}
@inline(never) func dict(_ n: Int) -> Int {
    var d = [Int: Int](); for i in 0..<n { d[i &* 7919 % 100003, default: 0] += i }; return d.count
}

func run(_ name: String, _ f: () -> UInt64) {
    var best = UInt64.max, first: UInt64 = 0, r: UInt64 = 0
    for k in 0..<3 { let t0 = nowNs(); r = f(); let dt = nowNs() - t0; if k == 0 { first = dt }; best = min(best, dt) }
    print("\(name) best \(Double(best) / 1e6) ms  first \(Double(first) / 1e6) ms  result \(r)")
}
let ints = Array(0..<2_000_000), dbls = ints.map { Double($0) }
run("msgSend") { objcMsgSend(20_000_000) }
run("objc-alloc") { objcAlloc(2_000_000) }
run("generic-int") { UInt64(sumGeneric(ints)) }
run("generic-dbl") { UInt64(sumGeneric(dbls)) }
run("existential") { UInt64(existential(10_000_000)) }
run("arc-list") { UInt64(arcList(2_000_000)) }
run("dict") { UInt64(dict(1_000_000)) }
