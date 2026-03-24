// =============================================================================
/*
 * CDTV_IR_keyboard — CDTV Wireless Keyboard IR Transmitter
 * Copyright (c) 2026 Korinel
 * SPDX-License-Identifier: MIT
 *
 * Sends Amiga keyboard scancodes to a CDTV via the 40-bit IR keyboard
 * protocol. Each scancode is converted to the keycode the CDTV expects
 * and transmitted as a key-down frame over a 40 kHz IR carrier.
 *
 * ── Protocol overview ───────────────────────────────────────────────────────
 *
 * Each frame is 40 bits, transmitted most significant bit first:
 *
 *   [4-bit header] [8-bit qualifier bitmask] [8-bit keycode] [20-bit check]
 *
 *   Header:    Always 0100 on the wire (routes to the keyboard handler)
 *   Qualifier: One bit per qualifier key (Shift L & R, Alt L & R, Amiga L & R, Ctrl, CapsLock)
 *   Keycode:   Encoded key identifier; 0x00 = no key
 *   Check:     Bitwise complement of the preceding 20 data bits
 *
 * ── Keycode encoding ───────────────────────────────────────────────────────
 *
 *   keycode = bit_reverse(amiga_scancode + 1)
 *
 *   +1 ensures scancode 0x00 maps to 0x80 rather than 0x00, which the CDTV
 *   treats as "no key". bit_reverse() compensates for the CDTV's bit
 *   accumulation order.
 *
 *   Bit 0 of the keycode is the key-up flag: 0 = pressed, 1 = released.
 *
 * ── Carrier and timings ──────────────────────────────────────────────────────
 *
 *   Carrier:      40 kHz, 33% duty cycle
 *   Header mark:  9,000 µs
 *   Header space: 4,500 µs
 *   Bit '1' mark: 1,200 µs + 400 µs space
 *   Bit '0' mark:   400 µs + 400 µs space
 *
 * ── Platform ─────────────────────────────────────────────────────────────────
 *
 *   Raspberry Pi Pico. PWM peripheral generates the IR carrier;
 *   busy_wait_us_32() provides the mark and space timing.
 *
 * ── See also ─────────────────────────────────────────────────────────────────
 *
 *   README.md — full protocol documentation including qualifier bitmask
 *               layout, check bit calculation, and frame examples.
 */
// =============================================================================
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"
#include <stdint.h>
#include <stdio.h>

// =============================================================================
// GPIO
// =============================================================================
#define LED_IR 16           /* IR LED output pin */

// =============================================================================
// Carrier settings
// =============================================================================
static const uint32_t ir_frequency_hz       = 40000; /* 40 kHz — not 38 kHz */
static const uint8_t  ir_duty_cycle_percent = 33;

// =============================================================================
// Frame timings (microseconds)
// =============================================================================
static const uint32_t ir_hdr_mark_us   = 9000;
static const uint32_t ir_hdr_space_us  = 4500;
static const uint32_t ir_zero_mark_us  =  400;
static const uint32_t ir_zero_space_us =  400;
static const uint32_t ir_one_mark_us   = 1200;
static const uint32_t ir_one_space_us  =  400;

static const size_t num_data_bits = 20; /* 4 header + 8 qualifier + 8 keycode */
static const uint32_t data_mask   = 0xFFFFF; /* 20-bit mask for check complement */

// =============================================================================
// Key sequencing
// =============================================================================
static const uint8_t qualifier       = 0x00;  /* no qualifier held */
static const uint8_t ir_kb_header    = 0x04;  /* 0100 MSB-first = keyboard handler */
static const uint32_t inter_frame_ms = 3000;   /* gap between pressed keys */

/*
 * Key sequence to transmit — Amiga raw scancodes.
 * See README.md or the Amiga Hardware Reference Manual for the full table.
 */
static const uint8_t key_sequence[] = {
    0x20,   /* a */
    0x37,   /* m */
    0x17,   /* i */
    0x24,   /* g */
    0x20,   /* a */
};

static const size_t key_count = sizeof(key_sequence) / sizeof(key_sequence[0]);

// =============================================================================
// PWM state
// =============================================================================
static uint32_t pwm_slice_num = 0;
static uint32_t pwm_channel   = 0;
static uint16_t pwm_level     = 0;  /* compare value for carrier-on level */

// =============================================================================
// Keycode encoding
// =============================================================================

/*
 * bit_reverse — reverse all 8 bits of a byte.
 *
 * The CDTV accumulates incoming IR bits in a way that reverses their order.
 * Reversing here means the CDTV decodes exactly the value we intended.
 */
static uint8_t bit_reverse(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

/*
 * scancode_to_keycode — convert an Amiga scancode (0x00–0x7F) to the
 * 8-bit keycode used in the CDTV IR frame.
 *
 * The +1 offset prevents scancode 0x00 from mapping to index 0x00, which
 * the CDTV uses as a "no key" sentinel. bit_reverse() compensates for
 * the CDTV's bit accumulation order.
 *
 * Bit 0 is left clear (key-down); the caller sets it for key-up.
 */
static uint8_t scancode_to_keycode(uint8_t scancode)
{
    return bit_reverse((uint8_t)((scancode + 1) & 0xFF));
}

// =============================================================================
// PWM / carrier initialisation
// =============================================================================
static void init_pwm_for_ir(void)
{
    gpio_set_function(LED_IR, GPIO_FUNC_PWM);
    pwm_slice_num = pwm_gpio_to_slice_num(LED_IR);
    pwm_channel   = pwm_gpio_to_channel(LED_IR);

    uint32_t sys_clk_freq = clock_get_hz(clk_sys);
    uint16_t pwm_wrap = (uint16_t)((sys_clk_freq / ir_frequency_hz) - 1);
    pwm_level = (uint16_t)((pwm_wrap * ir_duty_cycle_percent) / 100);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, pwm_wrap);
    pwm_init(pwm_slice_num, &config, true);
    pwm_set_chan_level(pwm_slice_num, pwm_channel, 0); /* carrier off at start */
}

// =============================================================================
// IR transmission
// =============================================================================

/*
 * ir_emit — transmit one mark/space pair.
 *
 * Enables the PWM carrier for mark_us, then disables it for space_us.
 * All timing is produced by busy-wait; do not call from an interrupt.
 */
static inline void ir_emit(uint32_t mark_us, uint32_t space_us)
{
    pwm_set_chan_level(pwm_slice_num, pwm_channel, pwm_level); /* carrier on  */
    busy_wait_us_32(mark_us);
    pwm_set_chan_level(pwm_slice_num, pwm_channel, 0);          /* carrier off */
    busy_wait_us_32(space_us);
}

/*
 * transmit_cdtv_frame — send one complete 40-bit CDTV IR keyboard frame.
 *
 * Frame layout (MSB first):
 *   20 data bits  — [header nibble 0x4][qualifier 8 bits][keycode 8 bits]
 *   20 check bits — bitwise complement of the data bits
 *
 * The CDTV verifies the check bits before accepting the frame; a mismatch
 * causes the frame to be silently discarded.
 */
static void transmit_cdtv_frame(uint32_t key_data)
{
    uint32_t check_bits = (~key_data) & data_mask;

    /* 9 ms leader mark + 4.5 ms leader space */
    ir_emit(ir_hdr_mark_us, ir_hdr_space_us);

    /* 20 data bits, MSB first */
    for (size_t i = num_data_bits; i-- > 0; ) {
        if (key_data & (1u << (unsigned)i))
            ir_emit(ir_one_mark_us, ir_one_space_us);
        else
            ir_emit(ir_zero_mark_us, ir_zero_space_us);
    }

    /* 20 check bits, MSB first */
    for (size_t i = num_data_bits; i-- > 0; ) {
        if (check_bits & (1u << (unsigned)i))
            ir_emit(ir_one_mark_us, ir_one_space_us);
        else
            ir_emit(ir_zero_mark_us, ir_zero_space_us);
    }
}

/*
 * transmit_key_down — build and send a key-down frame for one Amiga scancode.
 *
 * Assembles the 20-bit data word:
 *   bits [19:16] — header nibble 0x4 (keyboard frame identifier)
 *   bits [15: 8] — qualifier bitmask (held Shift/Alt/Amiga/Ctrl/CapsLock)
 *   bits [ 7: 0] — keycode, bit 0 clear (key-down)
 */
static void transmit_key_down(uint8_t scancode, uint8_t qualifier)
{
    uint8_t keycode = scancode_to_keycode(scancode) & 0xFE; /* bit 0 = 0: down */

    uint32_t key_data =
        ((uint32_t)ir_kb_header << 16) |
        ((uint32_t)qualifier   <<  8) |
        (uint32_t)keycode;

    transmit_cdtv_frame(key_data);
}

// =============================================================================
// Main
// =============================================================================
int main(void)
{
    stdio_init_all();
    init_pwm_for_ir();

    printf("CDTV IR keyboard transmitter\n");
    printf("GPIO: %d  Carrier: %lu Hz  Duty: %d%%\n",
           LED_IR, (unsigned long)ir_frequency_hz, ir_duty_cycle_percent);
    printf("Keys to send: %u\n\n", (unsigned)key_count);

    for (size_t i = 0; i < key_count; i++) {

        uint8_t scancode  = key_sequence[i];
        uint8_t keycode = scancode_to_keycode(scancode) & 0xFE;

        printf("[%u/%u] scancode=0x%02X  keycode=0x%02X\n",
               (unsigned)(i + 1), (unsigned)key_count,
               scancode, keycode);

        transmit_key_down(scancode, qualifier);
        sleep_ms(inter_frame_ms);
    }

    printf("\nSequence complete.\n");
}
// =============================================================================
// END OF FILE
// =============================================================================