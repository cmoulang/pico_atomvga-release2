#pragma once

#include "atom_if.h"
#include "sound.h"
#include "hardware/watchdog.h"

#define YARRB_REG0 0xBFFE
#define YARRB_4MHZ 0x20

/*

EXAMPLE FOR THE DMA INTERFACE

*/

/// @brief calculate an estimate for pi using the random dart throw algorithm
/// @param count additional number of trials
/// @return an estimate for pi
double estimate_pi(int count)
{
    static uint64_t noof_trials = 0;
    static uint64_t in_pie = 0;
    static const uint64_t rand_max_squared = (uint64_t)RAND_MAX * RAND_MAX;
    for (int i = 0; i < count; i++)
    {
        uint64_t x = rand();
        uint64_t y = rand();
        x = x * x + y * y;
        if (x < rand_max_squared)
        {
            in_pie++;
        }
    }

    noof_trials += count;
    return (double)(4 * in_pie) / noof_trials;
}

volatile bool vdu_updated_flag = false;
volatile bool sid_updated_flag = false;


void handler()
{
    dma_hw->ints1 = 1u << eb_get_event_chan();
    
    int address = eb_get_event();
    while (address > 0)
    {
        if (address == YARRB_REG0)
        {
            if ((eb_get(YARRB_REG0) & YARRB_4MHZ) && (watchdog_hw->scratch[0] != EB_65C02_MAGIC_NUMBER))
            {
                puts("YARRB set to 4MHz mode");
                // Shut down the 6502 interface, set the magic number and reboot...
                eb_shutdown();
                sc_shutdown();
                watchdog_hw->scratch[0] = EB_65C02_MAGIC_NUMBER;
                watchdog_enable(0, true);
            }
        }
        if (address >= FB_ADDR && address < FB_ADDR + 0x1800)
        {
            vdu_updated_flag = true;
        }
        else if (address >= SID_BASE_ADDR && address < SID_BASE_ADDR + SID_LEN)
        {
            sid_updated_flag = true;
        }
        address = eb_get_event();
    }
}

static bool vdu_updated()
{
    irq_set_enabled(DMA_IRQ_1, false);
    bool result = vdu_updated_flag;
    vdu_updated_flag = false;
    irq_set_enabled(DMA_IRQ_1, true);
    return result;
}

static bool sid_updated()
{
    irq_set_enabled(DMA_IRQ_1, false);
    bool result = sid_updated_flag;
    sid_updated_flag = false;
    irq_set_enabled(DMA_IRQ_1, true);
    return result;
}

static void demo_init()
{
    eb_set_perm(0xA00, EB_PERM_READ_WRITE, 0x100);
    eb_set_perm(SID_BASE_ADDR, EB_PERM_WRITE_ONLY, 21);
    eb_set_perm(SID_BASE_ADDR + 21, EB_PERM_READ_ONLY, 8);
    eb_set_perm_byte(YARRB_REG0, EB_PERM_WRITE_ONLY);
}

static void atom_to_ascii(char *atom, int len)
{
    for (int i = 0; i < len; i++)
    {
        int v = atom[i];
        if (v < 0x80)
        {
            v = v ^ 0x60;
        }
        v = v - 0x20;
        if (!isprint(v))
        {
            v = '.';
        }
        atom[i] = v;
    }
}

int get_mode();
void print_screen(bool);
void print_sid();

void demo_loop()
{
    sc_init();

    // Tell the DMA to raise IRQ line 1 when the eb_event_chan finishes copying the address
    dma_channel_set_irq1_enabled(eb_get_event_chan(), true);

    // Configure the processor to run dma_handler() when DMA IRQ 1 is asserted
    irq_set_exclusive_handler(DMA_IRQ_1, handler);
    irq_set_enabled(DMA_IRQ_1, true);
    dma_hw->ints1 = 1u << eb_get_event_chan();
    for (;;)
    {
        __wfi();
        if (vdu_updated())
        {
            print_screen(false);
        }
        if (sid_updated())
        {
            print_sid();
        }
    }
}

static void hide_cursor()
{
    printf("\e["
           "?25l");
}

static void show_cursor()
{
    printf("\e["
           "?25h");
}

void print_screen(bool col80)
{
    const int max_cols = 80;
    int noof_rows = 16;
    int noof_cols = 32;
    if (col80)
    {
        noof_rows = 40;
        noof_cols = 80;
    }

    hide_cursor();
    printf("\e[1;1H");
    for (int row = 0; row < noof_rows; row++)
    {
        char content[max_cols + 1];
        eb_get_chars(content, noof_cols, FB_ADDR + row * noof_cols);
        atom_to_ascii(content, noof_cols);
        content[noof_cols] = 0;
        puts(content);
    }
    show_cursor();
}

void print_sid()
{
    hide_cursor();
    static const char *vreg_format[] = {
        "Freq Lo      %2x",
        "Freq Hi      %2x",
        "PW Lo        %2x",
        "PW Hi        %2x",
        "Control      %2x",
        "Attack/Decay %2x",
        "Sustain/Rel  %2x",
    };
    printf("\e[18;1H");

    for (int reg = 0; reg < 7; reg++)
    {
        for (int voice = 0; voice < 3; voice++)
        {
            {
                printf(vreg_format[reg], eb_get(SID_BASE_ADDR + reg + voice * 7));
                printf("    ");
            }
        }
        puts("");
    }
    show_cursor();
}