// input/dualshock.cpp
// DualShock 2 controller implementation for PS2 emulator
#include "dualshock.h"
#include <android/log.h>
#include <cstring>

#define TAG "PS2-PAD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static DualShock g_pad_singleton;

// Android keycodes (from android_keycodes.h)
enum {
    AKEYCODE_DPAD_UP    = 19,
    AKEYCODE_DPAD_DOWN  = 20,
    AKEYCODE_DPAD_LEFT  = 21,
    AKEYCODE_DPAD_RIGHT = 22,
    AKEYCODE_BUTTON_A   = 96,
    AKEYCODE_BUTTON_B   = 97,
    AKEYCODE_BUTTON_X   = 99,
    AKEYCODE_BUTTON_Y   = 100,
    AKEYCODE_BUTTON_L1  = 102,
    AKEYCODE_BUTTON_R1  = 103,
    AKEYCODE_BUTTON_L2  = 104,
    AKEYCODE_BUTTON_R2  = 105,
    AKEYCODE_BUTTON_THUMBL = 106,
    AKEYCODE_BUTTON_THUMBR = 107,
    AKEYCODE_BUTTON_START = 108,
    AKEYCODE_BUTTON_SELECT = 109,
};

// Android axis IDs
enum {
    AXIS_X  = 0, AXIS_Y = 1, AXIS_Z = 11, AXIS_RZ = 14,
    AXIS_LX = 0, AXIS_LY = 1, AXIS_RX = 11, AXIS_RY = 14,
};

// PS2 button mask bits for DualShock 2 response
enum PS2_Button : uint16_t {
    PS2_CROSS     = (1 << 14),
    PS2_CIRCLE    = (1 << 13),
    PS2_SQUARE    = (1 << 15),
    PS2_TRIANGLE  = (1 << 12),
    PS2_L1        = (1 << 10),
    PS2_R1        = (1 << 11),
    PS2_L2        = (1 << 8),
    PS2_R2        = (1 << 9),
    PS2_START     = (1 << 3),
    PS2_SELECT    = (1 << 0),
    PS2_L3        = (1 << 1),
    PS2_R3        = (1 << 2),
    PS2_DPAD_UP   = (1 << 4),
    PS2_DPAD_RIGHT= (1 << 5),
    PS2_DPAD_DOWN = (1 << 6),
    PS2_DPAD_LEFT = (1 << 7),
};

DualShock::DualShock() { reset(); }

void DualShock::reset() {
    memset(&m_pad, 0, sizeof(m_pad));
    memset(m_sio2_buf, 0, sizeof(m_sio2_buf));
    memset(m_padmcu_regs, 0, sizeof(m_padmcu_regs));
    m_sio2_pos = 0;
    m_sio2_len = 0;
    m_polling = false;
    m_pad.connected = true;
    m_pad.left_x = m_pad.left_y = 0;
    m_pad.right_x = m_pad.right_y = 0;
}

void DualShock::poll() {
    // Android input events are delivered via on_key_down/up and on_axis_motion
    // This is called each frame to update the internal state
}

void DualShock::on_key_down(int keycode) {
    switch (keycode) {
    case AKEYCODE_BUTTON_B:      m_pad.cross = true; m_pad.pressure_cross = 255; break;
    case AKEYCODE_BUTTON_A:      m_pad.circle = true; m_pad.pressure_circle = 255; break;
    case AKEYCODE_BUTTON_Y:      m_pad.square = true; m_pad.pressure_square = 255; break;
    case AKEYCODE_BUTTON_X:      m_pad.triangle = true; m_pad.pressure_triangle = 255; break;
    case AKEYCODE_BUTTON_L1:     m_pad.l1 = true; m_pad.pressure_l1 = 255; break;
    case AKEYCODE_BUTTON_R1:     m_pad.r1 = true; m_pad.pressure_r1 = 255; break;
    case AKEYCODE_BUTTON_L2:     m_pad.l2 = true; m_pad.pressure_l2 = 255; break;
    case AKEYCODE_BUTTON_R2:     m_pad.r2 = true; m_pad.pressure_r2 = 255; break;
    case AKEYCODE_BUTTON_START:  m_pad.start = true; break;
    case AKEYCODE_BUTTON_SELECT: m_pad.select = true; break;
    case AKEYCODE_BUTTON_THUMBL: m_pad.l3 = true; break;
    case AKEYCODE_BUTTON_THUMBR: m_pad.r3 = true; break;
    case AKEYCODE_DPAD_UP:       m_pad.dpad_up = true; break;
    case AKEYCODE_DPAD_DOWN:     m_pad.dpad_down = true; break;
    case AKEYCODE_DPAD_LEFT:     m_pad.dpad_left = true; break;
    case AKEYCODE_DPAD_RIGHT:    m_pad.dpad_right = true; break;
    }
}

void DualShock::on_key_up(int keycode) {
    switch (keycode) {
    case AKEYCODE_BUTTON_B:      m_pad.cross = false; m_pad.pressure_cross = 0; break;
    case AKEYCODE_BUTTON_A:      m_pad.circle = false; m_pad.pressure_circle = 0; break;
    case AKEYCODE_BUTTON_Y:      m_pad.square = false; m_pad.pressure_square = 0; break;
    case AKEYCODE_BUTTON_X:      m_pad.triangle = false; m_pad.pressure_triangle = 0; break;
    case AKEYCODE_BUTTON_L1:     m_pad.l1 = false; m_pad.pressure_l1 = 0; break;
    case AKEYCODE_BUTTON_R1:     m_pad.r1 = false; m_pad.pressure_r1 = 0; break;
    case AKEYCODE_BUTTON_L2:     m_pad.l2 = false; m_pad.pressure_l2 = 0; break;
    case AKEYCODE_BUTTON_R2:     m_pad.r2 = false; m_pad.pressure_r2 = 0; break;
    case AKEYCODE_BUTTON_START:  m_pad.start = false; break;
    case AKEYCODE_BUTTON_SELECT: m_pad.select = false; break;
    case AKEYCODE_BUTTON_THUMBL: m_pad.l3 = false; break;
    case AKEYCODE_BUTTON_THUMBR: m_pad.r3 = false; break;
    case AKEYCODE_DPAD_UP:       m_pad.dpad_up = false; break;
    case AKEYCODE_DPAD_DOWN:     m_pad.dpad_down = false; break;
    case AKEYCODE_DPAD_LEFT:     m_pad.dpad_left = false; break;
    case AKEYCODE_DPAD_RIGHT:    m_pad.dpad_right = false; break;
    }
}

void DualShock::on_axis_motion(int axis, float value) {
    int8_t val = (int8_t)(value * 127.0f);
    switch (axis) {
    case AXIS_LX: m_pad.left_x = val; break;
    case AXIS_LY: m_pad.left_y = val; break;
    case AXIS_RX: m_pad.right_x = val; break;
    case AXIS_RY: m_pad.right_y = val; break;
    }
}

uint8_t DualShock::sio2_read() {
    if (m_sio2_pos < m_sio2_len) return m_sio2_buf[m_sio2_pos++];
    return 0;
}

void DualShock::sio2_write(uint8_t val) {
    if (m_sio2_pos == 0) {
        memset(m_sio2_buf, 0, sizeof(m_sio2_buf));
        // Build response based on controller query
        uint16_t btn = 0;
        if (m_pad.cross)     btn |= PS2_CROSS;
        if (m_pad.circle)    btn |= PS2_CIRCLE;
        if (m_pad.square)    btn |= PS2_SQUARE;
        if (m_pad.triangle)  btn |= PS2_TRIANGLE;
        if (m_pad.l1)        btn |= PS2_L1;
        if (m_pad.r1)        btn |= PS2_R1;
        if (m_pad.l2)        btn |= PS2_L2;
        if (m_pad.r2)        btn |= PS2_R2;
        if (m_pad.start)     btn |= PS2_START;
        if (m_pad.select)    btn |= PS2_SELECT;
        if (m_pad.l3)        btn |= PS2_L3;
        if (m_pad.r3)        btn |= PS2_R3;
        if (m_pad.dpad_up)   btn |= PS2_DPAD_UP;
        if (m_pad.dpad_down) btn |= PS2_DPAD_DOWN;
        if (m_pad.dpad_left) btn |= PS2_DPAD_LEFT;
        if (m_pad.dpad_right)btn |= PS2_DPAD_RIGHT;

        if (val == 0x01) { // Start of poll
            m_sio2_buf[0] = 0xFF; // Header
            m_sio2_buf[1] = 0x41; // ID: DualShock 2 analog mode
            m_sio2_buf[2] = 0x5A; // OK
            m_sio2_buf[3] = (btn >> 0) & 0xFF; // Button byte 3
            m_sio2_buf[4] = (btn >> 8) & 0xFF; // Button byte 4
            m_sio2_buf[5] = m_pad.right_x;      // Right stick X
            m_sio2_buf[6] = m_pad.right_y;      // Right stick Y
            m_sio2_buf[7] = m_pad.left_x;       // Left stick X
            m_sio2_buf[8] = m_pad.left_y;       // Left stick Y
            // Pressure-sensitive data (bytes 9-17)
            m_sio2_buf[9]  = m_pad.pressure_right_x;
            m_sio2_buf[10] = m_pad.pressure_right_y;
            m_sio2_buf[11] = m_pad.pressure_left_x;
            m_sio2_buf[12] = m_pad.pressure_left_y;
            m_sio2_buf[13] = m_pad.pressure_triangle;
            m_sio2_buf[14] = m_pad.pressure_circle;
            m_sio2_buf[15] = m_pad.pressure_cross;
            m_sio2_buf[16] = m_pad.pressure_square;
            m_sio2_buf[17] = m_pad.pressure_l1;
            m_sio2_buf[18] = m_pad.pressure_r1;
            m_sio2_buf[19] = m_pad.pressure_l2;
            m_sio2_buf[20] = m_pad.pressure_r2;
            m_sio2_len = 21;
            m_sio2_pos = 1; // Position after header byte
        }
    } else {
        m_sio2_pos++;
    }
}

uint32_t DualShock::padmcu_read(uint32_t addr) {
    uint32_t reg = (addr >> 4) & 0xF;
    return m_padmcu_regs[reg];
}

void DualShock::padmcu_write(uint32_t addr, uint32_t val) {
    uint32_t reg = (addr >> 4) & 0xF;
    m_padmcu_regs[reg] = val;
}

void input_init() {
    g_pad_singleton.reset();
    LOGI("Input system initialized (DualShock 2)");
}

void input_shutdown() {
    LOGI("Input system shutdown");
}

DualShock* input_get_pad() { return &g_pad_singleton; }
