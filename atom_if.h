#pragma once

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "sm.pio.h"

#define ADD_BITS 16
#define BUFFER_SIZE 0x10000

enum eb_perm
{
    EB_PERM_WRITE_ONLY = 0b00,
    EB_PERM_READ_WRITE = 0b01,
    EB_PERM_NO_ACCESS = 0b10,
    EB_PERM_READ_ONLY = 0b11
};

static_assert(sizeof(void *) == 4);
static_assert(sizeof(int) == 4);

// Buffer spans 6502 address space
_Alignas(BUFFER_SIZE) volatile unsigned char memory[BUFFER_SIZE];
_Alignas(BUFFER_SIZE) volatile unsigned char permission[BUFFER_SIZE];

static void eb_read_init(PIO pio, uint sm, int address)
{
    uint offset;

    // // Overwrite instruction with the delay parameter
    // const uint delay_bits = 0b11111;
    // uint16_t x = eb_read_program_instructions[eb_read_offset_set_delay];
    // x = (x & ~delay_bits) | (delay & delay_bits);
    // ((uint16_t *)eb_read_program_instructions)[eb_read_offset_set_delay] = x;

    offset = pio_add_program(pio, &eb_read_program);

    (pio)->input_sync_bypass = (0xFF << PIN_A0) | (1 << PIN_R_NW);

    for (int pin = PIN_A0; pin < PIN_A0 + 8; pin++)
    {
        pio_gpio_init(pio, pin);
        gpio_set_pulls(pin, true, false);
    }

    for (int pin = PIN_MUX_DATA; pin < PIN_MUX_DATA + 3; pin++)
    {
        pio_gpio_init(pio, pin);
       // gpio_set_pulls(pin, true, false);
        gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
      //  gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_4MA);
    }

    pio_sm_set_pins_with_mask(pio, sm,
                              (0x07 << PIN_MUX_DATA),
                              (0x07 << PIN_MUX_DATA));

    pio_sm_set_consecutive_pindirs(pio, sm, PIN_MUX_DATA, 3, true);
    // pio_sm_set_consecutive_pindirs(pio, sm, PIN_A0, 8, false);

    pio_sm_config c = eb_read_program_get_default_config(offset);
    sm_config_set_jmp_pin(&c, PIN_R_NW);
    sm_config_set_in_pins(&c, PIN_A0);
    sm_config_set_out_pins(&c, PIN_A0, 8);
    sm_config_set_set_pins(&c, PIN_A0, 8);

    sm_config_set_sideset(&c, 4, true, false);
    sm_config_set_sideset_pins(&c, PIN_MUX_DATA);

    sm_config_set_in_shift(&c, false, true, 32);
    // sm_config_set_out_shift(&c, false, true, 8);

    pio_sm_init(pio, sm, offset, &c);

    pio_sm_put(pio, sm, address >> ADD_BITS);
    pio_sm_exec(pio, sm, pio_encode_pull(false, true));
    pio_sm_exec(pio, sm, pio_encode_mov(pio_x, pio_osr));
}

static void eb_address_init(PIO pio, int sm, int address)
{
    int offset;

    offset = pio_add_program(pio, &eb_address_program);

    pio_sm_config c = eb_address_program_get_default_config(offset);
    sm_config_set_in_pins(&c, PIN_A0);
    sm_config_set_in_shift(&c, false, true, 32);

    pio_sm_init(pio, sm, offset, &c);

    pio_sm_put(pio, sm, address >> ADD_BITS);
    pio_sm_exec(pio, sm, pio_encode_pull(false, true));
    pio_sm_exec(pio, sm, pio_encode_mov(pio_x, pio_osr));
}

static void eb_write_init(PIO pio, int sm)
{
    int offset;

    offset = pio_add_program(pio, &eb_write_program);

    pio_sm_config c = eb_write_program_get_default_config(offset);
    sm_config_set_in_pins(&c, PIN_A0);
    sm_config_set_set_pins(&c, PIN_MUX_DATA, 3);
    sm_config_set_in_shift(&c, false, true, 8);

    pio_sm_init(pio, sm, offset, &c);
}

static uint perm_address_chan;
static uint perm_data_chan;
static uint mem_address_chan;
static uint mem_address_chan2;
static uint mem_rdata_chan;
static uint mem_wdata_chan;

static void eb_setup_dma(PIO pio, int eb_read_sm,
                         int eb_address_sm,
                         int eb_write_sm)
{
    mem_wdata_chan = dma_claim_unused_channel(true);

    perm_address_chan = dma_claim_unused_channel(true);
    perm_data_chan = dma_claim_unused_channel(true);
    mem_address_chan = dma_claim_unused_channel(true);
    mem_address_chan2 = dma_claim_unused_channel(true);
    mem_rdata_chan = dma_claim_unused_channel(true);

    dma_channel_config c;

    // Copies address from fifo to perm_data_chan
    c = dma_channel_get_default_config(perm_address_chan);
    channel_config_set_high_priority(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, eb_read_sm, false));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_chain_to(&c, mem_address_chan);

    dma_channel_configure(
        perm_address_chan,
        &c,
        &dma_channel_hw_addr(perm_data_chan)->al3_read_addr_trig,
        &pio->rxf[eb_read_sm],
        1,
        true);

    // Copies address from fifo to mem_rdata_chan
    c = dma_channel_get_default_config(mem_address_chan);
    channel_config_set_high_priority(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, eb_address_sm, false));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_chain_to(&c, mem_address_chan2);

    dma_channel_configure(
        mem_address_chan,
        &c,
        &dma_channel_hw_addr(mem_rdata_chan)->al3_read_addr_trig,
        &pio->rxf[eb_address_sm],
        1,
        false);

    // Copies address from mem_rdata_chan to mem_wdata_chan chan
    c = dma_channel_get_default_config(mem_address_chan2);
    channel_config_set_high_priority(&c, true);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_chain_to(&c, perm_address_chan);

    dma_channel_configure(
        mem_address_chan2,
        &c,
        &dma_channel_hw_addr(mem_wdata_chan)->al2_write_addr_trig,
        &dma_channel_hw_addr(mem_rdata_chan)->read_addr,
        1,
        false);

    // Copies data from the perm table to fifo
    c = dma_channel_get_default_config(perm_data_chan);
    channel_config_set_high_priority(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, eb_read_sm, true));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);

        dma_channel_configure(
        perm_data_chan,
        &c,
        &pio->txf[eb_read_sm],
        NULL, // read address set by DMA
        1,
        false);

    // Copies data from the memory to fifo
    c = dma_channel_get_default_config(mem_rdata_chan);
 channel_config_set_high_priority(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, eb_read_sm, true));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(
        mem_rdata_chan,
        &c,
        &pio->txf[eb_read_sm],
        NULL, // read address set by DMA
        1,
        false);

    // Copies data from fifo to memory
    c = dma_channel_get_default_config(mem_wdata_chan);
    channel_config_set_high_priority(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, eb_write_sm, false));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(
        mem_wdata_chan,
        &c,
        NULL, // write address set by DMA
        &pio->rxf[eb_write_sm],
        1,
        false);
}

static void atom_if_init(PIO pio) //, irq_handler_t handler)
{
    uint eb_read_sm = 0;
    eb_read_init(pio, eb_read_sm, (uint)&permission);

    uint eb_address_sm = 1;
    eb_address_init(pio, eb_address_sm, (uint)&memory);

    uint eb_write_sm = 2;
    eb_write_init(pio, eb_write_sm);

    eb_setup_dma(pio, eb_read_sm, eb_address_sm, eb_write_sm);

    // // Tell the DMA to raise IRQ line 0 when the channel finishes a block
    // dma_channel_set_irq1_enabled(last_address_chan, true);

    // // Configure the processor to run dma_handler() when DMA IRQ 0 is asserted
    // irq_set_exclusive_handler(DMA_IRQ_1, handler);
    // irq_set_enabled(DMA_IRQ_1, true);
    // dma_hw->ints1 = 1u << last_address_chan;

    pio_enable_sm_mask_in_sync(pio, 1u << eb_read_sm | 1u << eb_address_sm | 1u << eb_write_sm);
}

static void eb_set_perm(size_t start, enum eb_perm perm, size_t size)
{
    hard_assert(start + size <= BUFFER_SIZE);
    memset((char *)permission + start, perm, size);
}

static inline void eb_set_perm_byte(size_t address, enum eb_perm perm)
{
    hard_assert(address <= BUFFER_SIZE);
    permission[address] = perm;
}