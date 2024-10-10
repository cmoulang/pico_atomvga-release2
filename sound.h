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


enum sc_osc_type
{
    SC_OSC_TRIANGLE = 1,
    SC_OSC_SAWTOOTH = 2
};

static uint16_t sc_sin_wave[SC_WAVE_TABLE_LENGTH];
typedef uint16_t (*sc_wave_function_t)(uint32_t p);

typedef struct
{
    int freq;
    int pinc;
    uint32_t p;
    enum sc_osc_type osc;
    u_int8_t* sid_addr;
} sc_voc_voice;

static volatile sc_voc_voice sc_voc[SC_NOOF_VOICES];

static struct repeating_timer sc_timer;

static inline int sc_freq_from_sid(uint8_t addr)
{
    int freq = eb_get(addr);
    freq += eb_get(addr + 1) << 8;
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

static inline void sc_voc_init(volatile sc_voc_voice *voice, enum sc_osc_type osc)
{
    voice->p = 0;
    voice->pinc = 0;
    voice->osc = osc;
}

static inline uint16_t sc_triangle(uint32_t p)
{
    if (p & (1 << 31))
    {
        p = ~p;
    }
    return p >> 15;
}

static inline uint16_t sc_sawtooth(uint32_t p)
{
    return p >> 16;
}

static inline uint16_t sc_noise(uint32_t p)
{
    return rand();
}

static inline uint16_t sc_sin(uint32_t p)
{
    size_t index = p / (UINT32_MAX / SC_WAVE_TABLE_LENGTH);
    return sc_sin_wave[index];
}

static inline uint16_t sc_voc_next_sample(volatile sc_voc_voice *voice)
{
    voice->p += voice->pinc;
    u_int16_t result = 0;
    if (voice->osc == SC_OSC_TRIANGLE)
    {
        result = sc_triangle(voice->p);
    }
    else if (voice->osc == SC_OSC_SAWTOOTH)
    {
        result = sc_sawtooth(voice->p);        
    }
    return result;
}

bool sc_timer_callback(struct repeating_timer *t)
{
    static int sample;
    pwm_set_gpio_level(SC_PIN, sample);
 
    sample = 0;
    for (int i = 0; i < SC_NOOF_VOICES; i++)
    {
        sample += sc_voc_next_sample(&sc_voc[i]);
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

 
    for (int i = 0; i < SC_NOOF_VOICES; i++)
    {
        sc_voc_init(&sc_voc[i], SC_OSC_SAWTOOTH);
    }

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

static void sc_pwave(char* name, sc_wave_function_t wave_function)
{
    const int steps = 32;
    puts("");
    printf(name);
    for (int i=0; i < steps; i++)
    {
        uint32_t p = UINT32_MAX / steps * i; 
        printf("%4d", wave_function(p) >> 8);
    }
}

static void sc_demo()
{
    puts("");
    sc_pwave("Sawtooth:  ", sc_sawtooth);
    sc_pwave("Triangle:  ", sc_triangle);
    sc_pwave("Sine wave: ", sc_sin);
    sc_pwave("Noise:     ", sc_noise);
    puts("");

    for (int i = 1; i < 8; i++)
    {
        sleep_ms(100);
        for (int v = 0; v < SC_NOOF_VOICES; v++)
        {
            int x = rand() % 1000;
            x = x + 500;
            sc_voc_set_freq(&sc_voc[v], x);
        }
    }
    sleep_ms(400);
    for (int v = 0; v < SC_NOOF_VOICES; v++)
    {
        sc_voc_set_freq(&sc_voc[v], 0);
    }
}
