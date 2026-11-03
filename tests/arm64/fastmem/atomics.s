// x86Code: LSE atomics / CAS / CASP on x0 (maybe top-byte tagged): words as kAtomics in fastmemtest.cpp
// (clang --target=arm64-apple-macos -march=armv8.1-a -c; llvm-objdump -d)
    add x9, x0, #192
    mov x16, #7
    ldadd x16, x17, [x9]
    swp x16, x18, [x9]
    ldsmax x16, x19, [x9]
    ldclral x16, x20, [x9]
    mov x21, xzr          // memory is 0 here: the CAS succeeds (stores 7)
    cas x21, x16, [x9]
    add x9, x0, #224
    ldr x2, [x9]
    ldr x3, [x9, #8]
    mov x4, #5
    mov x5, #6
    casp x2, x3, x4, x5, [x9]
    svc #0
