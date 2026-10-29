// libarm64jit: the two native ends of the guest boundary (System V x86-64, which Darwin x86_64 also uses).
    .intel_syntax noprefix
    .text

// void a64_ncall(const void *fn, const u64 ints[16], const u64 fps[8], NRet *ret)
// ints[0..5] -> rdi..r9, ints[6..15] -> stack slots, fps[0..7] -> xmm0-7, al = 8 (for variadic natives).
    .globl a64_ncall
    .type a64_ncall, @function
a64_ncall:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    mov r11, rdi
    mov rbx, rsi
    mov r12, rcx
    movq xmm0, [rdx]
    movq xmm1, [rdx + 8]
    movq xmm2, [rdx + 16]
    movq xmm3, [rdx + 24]
    movq xmm4, [rdx + 32]
    movq xmm5, [rdx + 40]
    movq xmm6, [rdx + 48]
    movq xmm7, [rdx + 56]
    sub rsp, 80
    mov rax, [rbx + 48]
    mov [rsp], rax
    mov rax, [rbx + 56]
    mov [rsp + 8], rax
    mov rax, [rbx + 64]
    mov [rsp + 16], rax
    mov rax, [rbx + 72]
    mov [rsp + 24], rax
    mov rax, [rbx + 80]
    mov [rsp + 32], rax
    mov rax, [rbx + 88]
    mov [rsp + 40], rax
    mov rax, [rbx + 96]
    mov [rsp + 48], rax
    mov rax, [rbx + 104]
    mov [rsp + 56], rax
    mov rax, [rbx + 112]
    mov [rsp + 64], rax
    mov rax, [rbx + 120]
    mov [rsp + 72], rax
    mov rdi, [rbx]
    mov rsi, [rbx + 8]
    mov rdx, [rbx + 16]
    mov rcx, [rbx + 24]
    mov r8, [rbx + 32]
    mov r9, [rbx + 40]
    mov eax, 8
    call r11
    mov [r12], rax
    mov [r12 + 8], rdx
    movq [r12 + 16], xmm0
    movq [r12 + 24], xmm1
    lea rsp, [rbp - 16]
    pop r12
    pop rbx
    pop rbp
    ret
    .size a64_ncall, . - a64_ncall

// Target of the per-function trampolines "mov r10, <guest pc>; mov r11, a64_tramp_entry; jmp r11".
// Frame passed to a64_enter_guest: ints[8], fps[8], then the result (rax, rdx, xmm0, xmm1).
    .globl a64_tramp_entry
    .type a64_tramp_entry, @function
a64_tramp_entry:
    push rbp
    mov rbp, rsp
    sub rsp, 160
    mov [rsp], rdi
    mov [rsp + 8], rsi
    mov [rsp + 16], rdx
    mov [rsp + 24], rcx
    mov [rsp + 32], r8
    mov [rsp + 40], r9
    mov rax, [rbp + 16]
    mov [rsp + 48], rax
    mov rax, [rbp + 24]
    mov [rsp + 56], rax
    movq [rsp + 64], xmm0
    movq [rsp + 72], xmm1
    movq [rsp + 80], xmm2
    movq [rsp + 88], xmm3
    movq [rsp + 96], xmm4
    movq [rsp + 104], xmm5
    movq [rsp + 112], xmm6
    movq [rsp + 120], xmm7
    mov rdi, r10
    mov rsi, rsp
    call a64_enter_guest
    mov rax, [rsp + 128]
    mov rdx, [rsp + 136]
    movq xmm0, [rsp + 144]
    movq xmm1, [rsp + 152]
    leave
    ret
    .size a64_tramp_entry, . - a64_tramp_entry

    .section .note.GNU-stack,"",@progbits
