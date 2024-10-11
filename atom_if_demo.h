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

/// @brief obtain the current output location for the event queue
/// @return index of next entry to be written
static inline uint get_in_ptr()
{
    uint x = (uint)dma_channel_hw_addr(event_queue_chan)->write_addr;
    return (x - (uint)&eb_event_queue) / 4;
}

/// @brief get the 6502 address from the pico memory address
/// @param pico_address the pico address
/// @return the corresponding 6502 address
static inline uint get_6502_address(uint pico_address)
{
    return (pico_address - (uint)&eb_memory) / 2;
}

volatile bool r65c02_mode = false;

void handler()
{
    static size_t out_ptr = 0;

    dma_hw->ints1 = 1u << event_queue_chan;
    while (out_ptr != get_in_ptr())
    {
        uint address = get_6502_address(eb_event_queue[out_ptr]);
        if (address == YARRB_REG0)
        {
            if (!r65c02_mode && (eb_get(YARRB_REG0) & YARRB_4MHZ))
            {
                puts("YARRB set to 4MHz mode");
                // Shut down the 6502 interface, set the magic number and reboot..
                eb_shutdown();
                watchdog_hw->scratch[0] = MAGIC_4MHZ_NUMBER;
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

        out_ptr = (out_ptr + 1) % EB_EVENT_QUEUE_LEN;
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
    if (watchdog_hw->scratch[0] == MAGIC_4MHZ_NUMBER)
    {
        r65c02_mode = true;
    }
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

    // Tell the DMA to raise IRQ line 1 when the event_queue_chan finishes copying the address
    dma_channel_set_irq1_enabled(event_queue_chan, true);

    // Configure the processor to run dma_handler() when DMA IRQ 1 is asserted
    irq_set_exclusive_handler(DMA_IRQ_1, handler);
    irq_set_enabled(DMA_IRQ_1, true);
    dma_hw->ints1 = 1u << event_queue_chan;

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