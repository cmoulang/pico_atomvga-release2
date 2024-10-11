#include <math.h>
#include <hardware/pwm.h>
#include <hardware/clocks.h>
#include "pico/stdlib.h"
#include <stdlib.h>

#define SC_WAVE_TABLE_LENGTH 32
#define SC_PIN 22
#define SC_SAMPLE_RATE 20000
#define SC_TICK_US (1000000 / SC_SAMPLE_RATE)
#define SC_NOOF_VOICES 3
#define SC_PWM_WRAP ((UINT16_MAX * 3) >> 6)

// The Atom SID sound board uses #BDC0 to #BDDF
#define SID_BASE_ADDR 0xBDC0
#define SID_LEN 29

// REG #	DATA                                           REG NAME TYPE
// (Hex)	d7    d6    d5    d4    d3    d2    d1    d0
// - --------------------------------------------------------------------

// Voice 1:

// 00     F7    F6    F5    F4    F3    F2    F1    F0    FREQ LO  Write
// 01     F15   F14   F13   F12   F11   F10   F9    F8    FREQ HI  Write
// 02     PW7   PW6   PW5   PW4   PW3   PW2   PW1   PW0   PW LO	Write
// 03     -     -     -     -     PW11  PW10  PW9   PW8   PW HI    Write
// 04     Noise Pulse ///   /\/\  TEST  RING  SYNC  GATE  CONTROL  Write
// 05     ATK3  ATK2  ATK1  ATK0  DCY3  DCY2  DCY1  DCY0  ATK/DCY	Write
// 06     STN3  STN2  STN1  STN0  RLS3  RLS2  RLS1  RLS0  STN/RLS  Write

// Voice 2:

// 07     F7    F6    F5    F4    F3    F2    F1    F0    FREQ LO  Write
// 08     F15   F14   F13   F12   F11   F10   F9    F8    FREQ HI  Write
// 09     PW7   PW6   PW5   PW4   PW3   PW2   PW1   PW0   PW LO	Write
// 0A     -     -     -     -     PW11  PW10  PW9   PW8   PW HI    Write
// 0B     Noise Pulse ///   /\/\  TEST  RING  SYNC  GATE  CONTROL  Write
// 0C     ATK3  ATK2  ATK1  ATK0  DCY3  DCY2  DCY1  DCY0  ATK/DCY	Write
// 0D     STN3  STN2  STN1  STN0  RLS3  RLS2  RLS1  RLS0  STN/RLS  Write

// Voice 3:

// 0E     F7    F6    F5    F4    F3    F2    F1    F0    FREQ LO  Write
// 0F     F15   F14   F13   F12   F11   F10   F9    F8    FREQ HI  Write
// 10     PW7   PW6   PW5   PW4   PW3   PW2   PW1   PW0   PW LO	Write
// 11     -     -     -     -     PW11  PW10  PW9   PW8   PW HI    Write
// 12     Noise Pulse ///   /\/\  TEST  RING  SYNC  GATE  CONTROL  Write
// 13     ATK3  ATK2  ATK1  ATK0  DCY3  DCY2  DCY1  DCY0  ATK/DCY	Write
// 14     STN3  STN2  STN1  STN0  RLS3  RLS2  RLS1  RLS0  STN/RLS  Write

// Filter:

// 15     -     -     -     -     -     FC2   FC1   FC0   FC LO	Write
// 16     FC10  FC9   FC8   FC7   FC6   FC5   FC4   FC3   FC HI    Write
// 17     RES3  RES2  RES1  RES0  FILEX FILT3 FILT2 FILT1 RES/FILT Write
// 18     3 OFF HP    BP    LP    VOL3  VOL2  VOL1  VOL0  MODE/VOL Write

// Misc.:

// 19     PX7   PX6   PX5   PX4   PX3   PX2   PX1   PX0   POT X    Read
// 1A     PY7   PY6   PY5   PY4   PY3   PY2   PY1   PY0   POT Y    Read
// 1B     O7    O6    O5    O4    O3    O2    O1    O0    OSC3/RND Read
// 1C     E7    E6    E5    E4    E3    E2    E1    E0    ENV3     Read

#define FRE_LO 0
#define FREQ_HI 1
#define PW_LO 2
#define PW_HI 3
#define CONTROL 4
#define ATK_DCY 5
#define STN_RLS 6
#define MODE_VOL 0x18
#define MODE_3_OFF 0x80

enum sc_osc_type
{
    SC_OSC_TRIANGLE = 0x10,
    SC_OSC_SAWTOOTH = 0x20,
    SC_OSC_PULSE = 0x40,
    SC_OSC_NOISE = 0x80
};

static uint16_t sc_sin_wave[SC_WAVE_TABLE_LENGTH];
typedef uint16_t (*sc_wave_function_t)(uint32_t p);

typedef struct
{
    int freq;
    int pinc;
    uint32_t p;
    u_int8_t *sid_addr;
} sc_voc_voice;

static volatile sc_voc_voice sc_voc[SC_NOOF_VOICES];

static struct repeating_timer sc_timer;

/// @brief get a byte from a sid register 
/// @param reg the register 0-28
/// @return 
static inline uint sid_get(uint16_t reg)
{
    return eb_get(SID_BASE_ADDR + reg);
}

static inline int sc_freq_from_sid(uint16_t addr)
{
    int freq = sid_get(addr);
    freq += sid_get(addr + 1) << 8;
    freq = freq * 149 / 2500;
    return freq;
}

static void sc_voc_set_freq(volatile sc_voc_voice *voice, int freq)
{
    if (voice->freq == freq)
    {
        return;
    }
    voice->freq = freq;
    voice->pinc = freq * (UINT32_MAX / SC_SAMPLE_RATE);
}

static inline void sc_voc_init(volatile sc_voc_voice *voice)
{
    voice->p = 0;
    voice->pinc = 0;
}

static inline uint16_t sc_sin(uint32_t p)
{
    size_t index = p / (UINT32_MAX / SC_WAVE_TABLE_LENGTH);
    return sc_sin_wave[index];
}

static inline uint16_t sc_voc_next_sample(int index)
{
    const int v = index * 7;
    volatile sc_voc_voice *voice = &sc_voc[index];

    u_int8_t control = sid_get(v + CONTROL);
    u_int16_t result = 0;

    if (control & 0x01)
    {
        sc_voc_set_freq(voice, sc_freq_from_sid(v));
        voice->p += voice->pinc;
    }

    if (control & SC_OSC_TRIANGLE)
    {
        uint32_t x = voice->p;
        if (x & (1 << 31))
        {
            x = ~x;
        }
        result = x >> 15;
    }
    else if (control & SC_OSC_SAWTOOTH)
    {
        result = voice->p >> 16;
    }
    else if (control & SC_OSC_NOISE)
    {
        result = rand();
    }
    else if (control & SC_OSC_PULSE)
    {
        uint32_t x = voice->p;
        if (x & (1 << 31)) {
            result = 0xFFFF;
        } else {
            result = 0;
        }
    }
    return result;
}

bool sc_timer_callback(struct repeating_timer *t)
{
    static int sample;
    pwm_set_gpio_level(SC_PIN, sample);

    sample += sc_voc_next_sample(0);
    sample += sc_voc_next_sample(1);
    int x = sid_get(MODE_VOL);
    if (!(x & MODE_3_OFF))
    {
        sample += sc_voc_next_sample(2);
    }
    sample = sample >> 6;
    return true;
}

static void sc_init()
{

    for (size_t i = 0; i < SC_WAVE_TABLE_LENGTH; i++)
    {
        uint16_t x = (sin((double)2 * M_PI * i / SC_WAVE_TABLE_LENGTH) + 1) / 2 * UINT16_MAX;
        sc_sin_wave[i] = x;
    }

    sc_voc_init(&sc_voc[0]);
    sc_voc_init(&sc_voc[1]);
    sc_voc_init(&sc_voc[2]);

    gpio_set_dir(SC_PIN, GPIO_OUT);
    gpio_set_function(SC_PIN, GPIO_FUNC_PWM);

    int audio_pin_slice = pwm_gpio_to_slice_num(SC_PIN);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv(&c, 1);
    pwm_config_set_phase_correct(&c, false);
    pwm_config_set_wrap(&c, SC_PWM_WRAP);
    pwm_init(audio_pin_slice, &c, true);
    pwm_set_gpio_level(SC_PIN, 0);
    gpio_set_drive_strength(SC_PIN, GPIO_DRIVE_STRENGTH_12MA);

    uint slice_num = pwm_gpio_to_slice_num(SC_PIN);
    pwm_set_enabled(slice_num, true);

    bool ok = add_repeating_timer_us(-SC_TICK_US, sc_timer_callback, NULL, &sc_timer);
    hard_assert(ok);
}

static bool sc_shutdown()
{
    bool result = cancel_repeating_timer(&sc_timer);
    sleep_us(SC_TICK_US * 2);
    pwm_set_gpio_level(SC_PIN, 0);
    return result;
}
