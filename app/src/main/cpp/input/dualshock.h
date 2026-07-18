// input/dualshock.h
// DualShock 2 controller input for PS2 emulator via Android NDK Game Controller API
#pragma once
#include <cstdint>

struct PadState {
    bool connected;
    // Buttons (active high when pressed)
    bool cross, circle, square, triangle;
    bool l1, r1, l2, r2;
    bool start, select;
    bool l3, r3; // stick clicks
    // D-Pad
    bool dpad_up, dpad_down, dpad_left, dpad_right;
    // Analog sticks (-128 to 127)
    int8_t left_x, left_y;
    int8_t right_x, right_y;
    // Pressure-sensitive buttons (0-255) for DualShock 2
    uint8_t pressure_cross, pressure_circle, pressure_square, pressure_triangle;
    uint8_t pressure_l1, pressure_r1, pressure_l2, pressure_r2;
};

class DualShock {
public:
    DualShock();
    void reset();
    void poll(); // Poll Android input devices
    const PadState& get_state() const { return m_pad; }

    // Map Android keycodes to PS2 buttons
    void on_key_down(int keycode);
    void on_key_up(int keycode);
    void on_axis_motion(int axis, float value);

    // IOP PADMCU register interface (SIO2)
    uint8_t  sio2_read();
    void     sio2_write(uint8_t val);
    uint32_t padmcu_read(uint32_t addr);
    void     padmcu_write(uint32_t addr, uint32_t val);

private:
    PadState m_pad;
    uint8_t  m_sio2_buf[256];
    int      m_sio2_pos;
    int      m_sio2_len;
    uint32_t m_padmcu_regs[16];
    bool     m_polling;
};

// Initialize input subsystem
void input_init();
void input_shutdown();
DualShock* input_get_pad();
