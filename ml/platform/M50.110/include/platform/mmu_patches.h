#ifndef __PLATFORM_MMU_PATCHES_H__
#define __PLATFORM_MMU_PATCHES_H__

#include "patch.h"

#if CONFIG_FW_VERSION == 110 // ensure our hard-coded patch addresses are not broken
                             // by a FW upgrade

// Data patches this early on Digic 8 don't seem to work.
// It looks like ROM1 isn't properly mapped or initialised
// at that time, so accesses fail.
struct patch early_data_patches[] =
{

};

struct patch normal_data_patches[] =
{

};


struct function_hook_patch early_code_patches[] =
{

};

#if defined(CONFIG_M50_EDMAC_HOOK) && !defined(M50_SPY_BUILD)
/* ---------------------------------------------------------------------
 * RAW discovery hook (M50 1.1.0): records every EDMAC "set address" call.
 *
 * Hooked: 0xE054A9AE, Canon's EDMAC driver set-address function
 * (ELD/Edmac/Edmac.c), the whole function is:
 *     ldr   r2, [pc, #0x220]        ; r2 = 0xE0DD7BA0 (port table)
 *     ldr.w r0, [r2, r0, lsl #3]    ; r0 = MMIO base of port r0
 *     str.w r1, [r0, #0xa0]         ; ram_addr = r1
 *     bx    lr
 * The 8-byte hook overwrites the first two instructions and half of the
 * third, so the tail below re-implements the whole function and returns.
 * patch_mmu.c checks orig_content before patching and refuses on mismatch.
 *
 * Results are shown in Debug > M50 RAW probe > "Port log" (raw_probe.c):
 * which ports get addresses, and how many different addresses port 3 uses
 * (2-3 = Canon double/triple-buffers the RAW frame).
 * ------------------------------------------------------------------- */
/* returns the address Canon should really program (normally addr) */
extern uint32_t m50_edmac_hook(uint32_t port, uint32_t addr);
extern volatile uint32_t m50_crx_native_pitch;

/* Keep the original six-argument ABI for the encoder's register loader. */
void __attribute__((naked,noinline)) m50_crx_load_encoder(void *config, uint32_t planes,
        uint32_t levels, uint32_t width, uint32_t bits, uint32_t registers)
{
    asm(
        "push.w {r2-r8, lr}\n"
        "mov r4, r0\n"
        "mov r7, r1\n"
        "ldr pc, =0xe02d964f\n"
        ".ltorg\n"
    );
}

extern void m50_crx_quant_load(void *config, uint32_t planes, uint32_t levels,
        uint32_t width, uint32_t bits, uint32_t registers);

/* Canon's geometry setter shares the ROM page with the set-address hook.
 * Override xb only for Mem1Path's LV call, before the driver programs port 3. */
void __attribute__((noreturn,noinline,naked,aligned(4))) hook_m50_raw_pitch(void)
{
    asm(
        "push {r2, r3}\n"
        "cmp r0, #3\n"
        "bne 1f\n"
        "ldr r2, =0xe080d417\n"
        "cmp lr, r2\n"
        "bne 1f\n"
        "ldr r2, =m50_crx_native_pitch\n"
        "ldr r2, [r2]\n"
        "cbz r2, 1f\n"
        "str r2, [r1, #36]\n"
        "1: pop {r2, r3}\n"
        "push.w {r4-r11, lr}\n"
        "mov r5, r0\n"
        "ldr r0, =0xe0dd7ba0\n"
        "ldr pc, =0xe054a9c3\n"
        ".ltorg\n"
    );
}

void __attribute__((noreturn,noinline,naked,aligned(4))) hook_m50_set_addr(void)
{
    asm("push { r0-r12, lr }\n"); // 14 regs: keeps sp 8-byte aligned for the C calls

    uint32_t a0, a1;
    asm __volatile__ (
        "mov %0, r0\n"
        "mov %1, r1\n" : "=&r"(a0), "=&r"(a1)
    );
    uint32_t new_addr = m50_edmac_hook(a0, a1);
    /* overwrite the saved r1 (at sp+4 after the push) with the result */
    asm __volatile__ ("str %0, [sp, #4]\n" : : "r"(new_addr) : "memory");

    asm("pop { r0-r12, lr }\n"
        "ldr r2, =0xe0dd7ba0\n"
        "ldr.w r0, [r2, r0, lsl #3]\n"
        "str.w r1, [r0, #0xa0]\n"
        "bx lr\n");
}
#endif /* CONFIG_M50_EDMAC_HOOK */

#ifdef CONFIG_M50_CRAW_SPY
/* ---------------------------------------------------------------------
 * CRAW encoder spy: hook DryosDebugMsg (0xE0577FD0). Its first 8 bytes
 *     push   {r0, r1, r2, r3}
 *     push.w {r4-r11, lr}
 *     mov    sl, r1
 * are position-independent, so the hook replays them and jumps back
 * to 0xE0577FD8. m50_dbgmsg_spy() gets r0-r3 + caller stack args.
 * ------------------------------------------------------------------- */
extern void m50_dbgmsg_spy(uint32_t *regs);

void __attribute__((noreturn,noinline,naked,aligned(4))) hook_m50_dbgmsg(void)
{
    asm(
        "push   {r0-r3}\n"           /* regs[0..3]; caller stack args follow at sp+16 */
        "push   {r4-r12, lr}\n"      /* 40 bytes: sp stays 8-byte aligned */
        "add    r0, sp, #40\n"
        "ldr    r12, =m50_dbgmsg_spy\n"
        "blx    r12\n"
        "pop    {r4-r12, lr}\n"
        "pop    {r0-r3}\n"
        /* replay the 8 displaced bytes, then continue in ROM */
        "push   {r0, r1, r2, r3}\n"
        "push.w {r4-r11, lr}\n"
        "mov    r10, r1\n"
        "ldr    pc, =0xe0577fd9\n"
        ".ltorg\n"
    );
}
#endif

struct function_hook_patch normal_code_patches[] =
{
#ifdef CONFIG_M50_CRAW_SPY
    {
        .patch_addr = 0xe0577fd0, // DryosDebugMsg
        .orig_content = {0x0f, 0xb4, 0x2d, 0xe9, 0xf0, 0x4f, 0x8a, 0x46},
        .target_function_addr = (uint32_t)hook_m50_dbgmsg,
        .description = "M50 CRAW spy: DryosDebugMsg"
    },
#endif
#if defined(CONFIG_M50_EDMAC_HOOK) && !defined(M50_SPY_BUILD)
    {
        .patch_addr = 0xe054a9ae, // EDMAC set address (port, addr)
        .orig_content = {0x88, 0x4a, 0x52, 0xf8, 0x30, 0x00, 0xc0, 0xf8}, // checked before patching
        .target_function_addr = (uint32_t)hook_m50_set_addr,
        .description = "M50 RAW: log EDMAC set-address"
    },
    {
        .patch_addr = 0xe054a9ba, // EDMAC set geometry (port, descriptor)
        .orig_content = {0x2d, 0xe9, 0xf0, 0x4f, 0x05, 0x46, 0x83, 0x48},
        .target_function_addr = (uint32_t)hook_m50_raw_pitch,
        .description = "M50 RAW: 12-bit port 3 pitch"
    },
    {
        .patch_addr = 0xe02d9646,
        .orig_content = {0x2d, 0xe9, 0xfc, 0x41, 0x04, 0x46, 0x0f, 0x46},
        .target_function_addr = (uint32_t)m50_crx_quant_load,
        .description = "M50 C-RAW quantization test"
    },
#endif
};

#endif // M50 FW_VERSION 110

#endif // __PLATFORM_MMU_PATCHES_H__
