#ifndef __FIB_CONTEXT_H__
#define __FIB_CONTEXT_H__

#include <stdint.h>

typedef struct FibCTX {
#if defined(__x86_64__) || defined(_M_X64)
    // Standard non-volatile registers (x86_64)
    uint64_t    reg_r12;    // 0x00
    uint64_t    reg_r13;    // 0x08
    uint64_t    reg_r14;    // 0x10
    uint64_t    reg_r15;    // 0x18
    uint64_t    reg_rip;    // 0x20
    uint64_t    reg_rsp;    // 0x28
    uint64_t    reg_rbx;    // 0x30
    uint64_t    reg_rbp;    // 0x38

#ifdef _WIN32
    uint64_t    reg_rdi;    // 0x40
    uint64_t    reg_rsi;    // 0x48
    uint64_t    reg_seh;    // 0x50
    uint64_t    stack_base; // 0x58
    uint64_t    stack_limit;// 0x60
    uint64_t    padding;    // 0x68 (align to 16 bytes)
    uint64_t    xmm[10][2]; // 0x70 (XMM6-XMM15)
#endif

#elif defined(__aarch64__)
    // ARM64 registers (x19-x28, fp, sp, lr, d8-d15)
    uint64_t    x19, x20, x21, x22, x23, x24, x25, x26, x27, x28; // 0x00 - 0x48 (80B)
    uint64_t    fp, sp, lr;                                     // 0x50 - 0x60 (24B)
    uint64_t    d8, d9, d10, d11, d12, d13, d14, d15;           // 0x68 - 0xA0 (64B)
#endif
} FibCTX;

#ifdef __cplusplus
extern "C" {
#endif

void swap_context(FibCTX* from, FibCTX* to);
void asm_taskmain();

#ifdef __cplusplus
}
#endif

#endif // __FIB_CONTEXT_H__
