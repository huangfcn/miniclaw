.text

#if defined(__aarch64__)
.align 4

// ----------------------------------------------------------------------------
// void asm_taskmain(void);
//
// Setup: x19 = bridge function address, x20 = FibTCB*
// ----------------------------------------------------------------------------
.global asm_taskmain
.global _asm_taskmain
asm_taskmain:
_asm_taskmain:
    mov x0, x20     // FibTCB* to first argument
    br x19          // jump to bridge function

// ----------------------------------------------------------------------------
// void swap_context(FibCTX *from, FibCTX *to);
// x0 = from, x1 = to
// ----------------------------------------------------------------------------
.global swap_context
.global _swap_context
swap_context:
_swap_context:
    // Save context
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    str x29, [x0, #80]      // fp
    
    mov x9, sp
    str x9, [x0, #88]       // sp
    str lr, [x0, #96]       // lr
    
    stp d8, d9,   [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]

    // Restore context
    ldp x19, x20, [x1, #0]
    ldp x21, x22, [x1, #16]
    ldp x23, x24, [x1, #32]
    ldp x25, x26, [x1, #48]
    ldp x27, x28, [x1, #64]
    ldr x29, [x1, #80]      // fp
    
    ldr x9, [x1, #88]       // sp
    mov sp, x9
    ldr lr, [x1, #96]       // lr
    
    ldp d8, d9,   [x1, #104]
    ldp d10, d11, [x1, #120]
    ldp d12, d13, [x1, #136]
    ldp d14, d15, [x1, #152]

    ret

#else // x86_64

#ifdef _WIN32
// Windows x64
// ----------------------------------------------------------------------------
.align 32
.global swap_context
.global _swap_context
swap_context:
_swap_context:
    // 1. SAVE CURRENT CONTEXT (rcx = from, rdx = to)
    movq %r12, 0x00(%rcx)
    movq %r13, 0x08(%rcx)
    movq %r14, 0x10(%rcx)
    movq %r15, 0x18(%rcx)
    movq %rbx, 0x30(%rcx)
    movq %rbp, 0x38(%rcx)
    movq %rdi, 0x40(%rcx)
    movq %rsi, 0x48(%rcx)

    // Save RIP and RSP
    movq (%rsp), %rax
    movq %rax, 0x20(%rcx)
    leaq 8(%rsp), %rax
    movq %rax, 0x28(%rcx)

    // Windows TEB
    movq %gs:(0x00), %rax    // SEH Chain
    movq %rax, 0x50(%rcx)
    movq %gs:(0x08), %rax    // Stack Base
    movq %rax, 0x58(%rcx)
    movq %gs:(0x10), %rax    // Stack Limit
    movq %rax, 0x60(%rcx)

    // Save XMM6-XMM15
    movdqu %xmm6,  0x70(%rcx)
    movdqu %xmm7,  0x80(%rcx)
    movdqu %xmm8,  0x90(%rcx)
    movdqu %xmm9,  0xA0(%rcx)
    movdqu %xmm10, 0xB0(%rcx)
    movdqu %xmm11, 0xC0(%rcx)
    movdqu %xmm12, 0xD0(%rcx)
    movdqu %xmm13, 0xE0(%rcx)
    movdqu %xmm14, 0xF0(%rcx)
    movdqu %xmm15, 0x100(%rcx)

    // 2. RESTORE NEW CONTEXT
    movq 0x00(%rdx), %r12
    movq 0x08(%rdx), %r13
    movq 0x10(%rdx), %r14
    movq 0x18(%rdx), %r15
    movq 0x30(%rdx), %rbx
    movq 0x38(%rdx), %rbp
    movq 0x40(%rdx), %rdi
    movq 0x48(%rdx), %rsi

    // Restore Windows TEB
    movq 0x50(%rdx), %rax
    movq %rax, %gs:(0x00)
    movq 0x58(%rdx), %rax
    movq %rax, %gs:(0x08)
    movq 0x60(%rdx), %rax
    movq %rax, %gs:(0x10)

    // Restore XMM6-XMM15
    movdqu 0x70(%rdx),  %xmm6
    movdqu 0x80(%rdx),  %xmm7
    movdqu 0x90(%rdx),  %xmm8
    movdqu 0xA0(%rdx),  %xmm9
    movdqu 0xB0(%rdx),  %xmm10
    movdqu 0xC0(%rdx),  %xmm11
    movdqu 0xD0(%rdx),  %xmm12
    movdqu 0xE0(%rdx),  %xmm13
    movdqu 0xF0(%rdx),  %xmm14
    movdqu 0x100(%rdx), %xmm15

    // Switch Stack and Return
    movq 0x28(%rdx), %rsp
    pushq 0x20(%rdx)
    ret

.global asm_taskmain
.global _asm_taskmain
asm_taskmain:
_asm_taskmain:
    movq %r13, %rcx          // 1st arg: FibTCB*
    subq $72, %rsp           // Shadow space
    call *%r12               // bridge(FibTCB*)
    addq $72, %rsp
    ret

#else // System V AMD64 (Linux/macOS)
// ----------------------------------------------------------------------------
.align 32
.global asm_taskmain
.global _asm_taskmain
asm_taskmain:
_asm_taskmain:
    movq %r13, %rdi
    jmpq *%r12

.align 32
.global swap_context
.global _swap_context
swap_context:
_swap_context:
    popq %rax
    movq %r12, 0x00(%rdi)
    movq %r13, 0x08(%rdi)
    movq %r14, 0x10(%rdi)
    movq %r15, 0x18(%rdi)
    movq %rax, 0x20(%rdi)
    movq %rsp, 0x28(%rdi)
    movq %rbx, 0x30(%rdi)
    movq %rbp, 0x38(%rdi)

    movq 0x00(%rsi), %r12
    movq 0x08(%rsi), %r13
    movq 0x10(%rsi), %r14
    movq 0x18(%rsi), %r15
    movq 0x20(%rsi), %rax
    movq 0x28(%rsi), %rsp
    movq 0x30(%rsi), %rbx
    movq 0x38(%rsi), %rbp
    jmpq *%rax

#endif // _WIN32
#endif // __aarch64__

// Avoid executable stack on Linux
#if defined(__linux__) && defined(__GNUC__)
.section .note.GNU-stack,"",@progbits
#endif
