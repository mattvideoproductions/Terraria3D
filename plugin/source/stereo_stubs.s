@ ARM-mode entry stubs for the stage-2 stereo hooks (see stereo_layers.cpp).
@ The game jumps here through 8-byte `ldr pc, [pc, #-4]` veneers written over
@ the original function entries, so LR still holds the game caller's return
@ address; we pass it to C as the 2nd argument.

    .arm
    .section .text.t3d_stubs, "ax", %progbits
    .align 2

@ SpriteBatch::NextLayer (0x1D8BF8, a 4-instruction leaf that only touches r1).
@ Preserve every scratch register so callers see leaf-like behaviour.
    .global t3d_stub_layer_inc
    .type   t3d_stub_layer_inc, %function
t3d_stub_layer_inc:
    push    {r0-r3, r12, lr}
    mov     r1, lr
    bl      t3d_layer_inc               @ (batch, callerLr)
    pop     {r0-r3, r12, pc}

@ SpriteBatch::End (0x10FAFC).
    .global t3d_stub_end
    .type   t3d_stub_end, %function
t3d_stub_end:
    push    {r4, lr}
    mov     r1, lr
    mov     r2, r4                      @ caller's r4 (End wrapper restores it)
    bl      t3d_end                     @ (batch, callerLr, callerR4)
    pop     {r4, pc}

@ The original End: replay the two instructions our veneer overwrote, then
@ continue inside the game's function. Its epilogue returns to our caller.
    .global t3d_end_original
    .type   t3d_end_original, %function
t3d_end_original:
    push    {r4-r11, lr}                @ e92d4ff0
    mov     r5, r0                      @ e1a05000
    ldr     pc, =0x0010FB04
    .ltorg

@ MenuBackground 0x21C0BC, at 0x21C160 (after the island picture, before the
@ logo): stamp the logo's sprites, then replay `ldr r0,[r4,#0x88]; cmp r0,#0`
@ and continue at 0x21C168 (`beq`), flags intact.
    .global t3d_stub_menu_logo
    .type   t3d_stub_menu_logo, %function
t3d_stub_menu_logo:
    push    {r0-r3, r12, lr}
    bl      t3d_menu_logo_mark
    pop     {r0-r3, r12, lr}
    ldr     r0, [r4, #0x88]             @ e5940088
    cmp     r0, #0                      @ e3500000
    ldr     pc, =0x0021C168
    .ltorg

@ SpriteBatch vertex writer 0x1D90C4 (the single CPU vertex path, every
@ sprite). The veneer lands in t3d_stub_sprite_write: unless a background
@ layer is being submitted (t3d_fill_armed) it falls straight into the
@ original; otherwise it branches to the C t3d_sprite_write with the game's
@ arguments untouched (r0-r3, s0, stack, LR = game caller).
    .global t3d_stub_sprite_write
    .type   t3d_stub_sprite_write, %function
t3d_stub_sprite_write:
    ldr     ip, =t3d_fill_armed
    ldrb    ip, [ip]
    cmp     ip, #0
    bne     t3d_sprite_write

@ The original writer: replay the two overwritten instructions and continue at
@ 0x1D90CC. Its epilogue returns to whoever called it.
    .global t3d_sprite_write_original
    .type   t3d_sprite_write_original, %function
t3d_sprite_write_original:
    push    {r4-r10, lr}                @ e92d47f0
    mov     r6, r0                      @ e1a06000
    ldr     pc, =0x001D90CC
    .ltorg
