// x86Code RAM 3 fastmem test guest (words in fastmemtest.cpp: clang --target=arm64-apple-macos -march=armv8.1-a -c; llvm-objdump -d)
// x0 = buffer (maybe tagged), x1 = second buffer untagged; results into x2..x28
    ldrb w2, [x0]
    ldrh w3, [x0, #2]
    ldr  w4, [x0, #4]
    ldr  x5, [x0, #8]
    ldp  x6, x7, [x0, #16]
    ldr  q0, [x0, #32]
    ldrsb x8, [x0, #48]
    ldrsw x9, [x0, #52]
    ldar x10, [x0]
    add  x11, x0, #64
    strb w2, [x11]
    strh w3, [x11, #2]
    str  w4, [x11, #4]
    str  x5, [x11, #8]
    stp  x6, x7, [x11, #16]
    str  q0, [x11, #32]
    add  x12, x0, #128
    stlr x5, [x12]
    ldxr x13, [x12]
    add  x13, x13, #1
    stxr w14, x13, [x12]
    ldr  x15, [x12]
    // LSE atomics have no fastmem fallback (a tagged address faults in either mode): untagged x1 only
    add  x28, x1, #16
    mov  x16, #7
    ldadd x16, x17, [x28]
    ldr  x18, [x28]
    mov  x19, x18
    mov  x20, #99
    cas  x19, x20, [x28]
    ldr  x21, [x28]
    add x24, x0, #16
    ldaxp x22, x23, [x24]
    add x25, x11, #16
    stlxp w24, x23, x22, [x25]
    ldp  x25, x26, [x11, #16]
    ldr  x27, [x1]
    str  x0, [x1, #8]
    svc  #0
