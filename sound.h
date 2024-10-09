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

static uint16_t sc_triangle_wave[SC_WAVE_TABLE_LENGTH];
static uint16_t sc_sin_wave[SC_WAVE_TABLE_LENGTH];

typedef struct
{
    int freq;
    int pinc;
    uint32_t p;
    uint16_t *wave_table;
} sc_voc_voice;

static volatile sc_voc_voice sc_voc[SC_NOOF_VOICES];

static struct repeating_timer sc_timer;

static void sc_voc_set_freq(volatile sc_voc_voice *voice, int freq)
{
    if (voice->freq == freq)
    {
        return;
    }
    voice->freq = freq;
    if (freq == 0)
    {
        voice->pinc = 0;
    }
    else
    {
        voice->pinc = freq * (UINT32_MAX / SC_SAMPLE_RATE);
    }
}

static inline void sc_voc_init(volatile sc_voc_voice *voice, uint16_t *wave_table)
{
    voice->p = 0;
    voice->pinc = 0;
    voice->wave_table = wave_table;
}

static inline uint16_t sc_voc_next_sample(volatile sc_voc_voice *voice)
{
    size_t index = voice->p / (UINT32_MAX / SC_WAVE_TABLE_LENGTH);
    voice->p += voice->pinc;
    return voice->wave_table[index];
}

bool sc_timer_callback(struct repeating_timer *t)
{
    int sample = 0;
    for (int i = 0; i < SC_NOOF_VOICES; i++)
    {
        sample += sc_voc_next_sample(&sc_voc[i]);
    }
    pwm_set_gpio_level(SC_PIN, sample >> 9);
    return true;
}

static void sc_init()
{
    for (size_t i = 0; i < SC_WAVE_TABLE_LENGTH; i++)
    {
        uint16_t x = (sin((double)2 * M_PI * i / SC_WAVE_TABLE_LENGTH) + 1) / 2 * UINT16_MAX;
        sc_sin_wave[i] = x;
    }

    int inc = UINT16_MAX / SC_WAVE_TABLE_LENGTH * 2;
    int p = 0;

    for (int i = 0; i < SC_WAVE_TABLE_LENGTH / 4; i++)
    {
        sc_triangle_wave[p++] = i * inc + UINT16_MAX / 2;
    }

    for (int i = 0; i < SC_WAVE_TABLE_LENGTH / 2; i++)
    {
        sc_triangle_wave[p++] = UINT16_MAX - i * inc;
    }

    for (int i = 0; i < SC_WAVE_TABLE_LENGTH / 4; i++)
    {
        sc_triangle_wave[p++] = i * inc;
    }

    for (int i = 0; i < SC_NOOF_VOICES; i++)
    {
        sc_voc_init(&sc_voc[i], sc_triangle_wave);
    }

    gpio_set_dir(SC_PIN, GPIO_OUT);
    gpio_set_function(SC_PIN, GPIO_FUNC_PWM);

    int audio_pin_slice = pwm_gpio_to_slice_num(SC_PIN);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv(&c, 1);
    pwm_config_set_phase_correct(&c, false);
    pwm_config_set_wrap(&c, 2200);
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

static void sc_demo()
{

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
