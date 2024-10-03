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
static_assert(sizeof(short) == 2);

// Buffer spans 6502 address space
_Alignas(BUFFER_SIZE) union
{
    volatile uint8_t m_8[BUFFER_SIZE * 2];
    volatile uint16_t m_16[BUFFER_SIZE];
    volatile uint32_t m_32[BUFFER_SIZE / 2];
    volatile uint64_t m_64[BUFFER_SIZE / 4];
} data;

static void eb2_address_program_init(PIO pio, uint sm)
{
    uint offset;

    offset = pio_add_program(pio, &eb2_address_program);

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
        // gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_4MA);
    }

    pio_sm_set_pins_with_mask(pio, sm,
                              (0x07 << PIN_MUX_DATA),
                              (0x07 << PIN_MUX_DATA));

    pio_sm_set_consecutive_pindirs(pio, sm, PIN_MUX_DATA, 3, true);
    // pio_sm_set_consecutive_pindirs(pio, sm, PIN_A0, 8, false);

    pio_sm_config c = eb2_address_program_get_default_config(offset);
    sm_config_set_jmp_pin(&c, PIN_A0 + 7);
    sm_config_set_in_pins(&c, PIN_A0);
    sm_config_set_out_pins(&c, PIN_A0, 8);
    sm_config_set_set_pins(&c, PIN_A0, 8);

    sm_config_set_sideset(&c, 4, true, false);
    sm_config_set_sideset_pins(&c, PIN_MUX_DATA);

    sm_config_set_in_shift(&c, false, true, 28);

    pio_sm_init(pio, sm, offset, &c);
}

static void eb2_access_program_init(PIO pio, int sm)
{
    int offset;

    offset = pio_add_program(pio, &eb2_access_program);

    pio_sm_config c = eb2_access_program_get_default_config(offset);
    sm_config_set_jmp_pin(&c, PIN_R_NW);
    sm_config_set_in_pins(&c, PIN_A0);
    sm_config_set_in_shift(&c, false, true, 8);

    sm_config_set_out_pins(&c, PIN_A0, 8);
    sm_config_set_set_pins(&c, PIN_A0, 8);

    sm_config_set_sideset(&c, 4, true, false);
    sm_config_set_sideset_pins(&c, PIN_MUX_DATA);

    pio_sm_init(pio, sm, offset, &c);
}

static uint address_chan;
static uint read_data_chan;
static uint address_chan2;
static uint write_data_chan;

static void eb_setup_dma(PIO pio, int eb2_address_sm,
                         int eb2_access_sm)
{
    address_chan = dma_claim_unused_channel(true);
    read_data_chan = dma_claim_unused_channel(true);
    address_chan2 = dma_claim_unused_channel(true);
    write_data_chan = dma_claim_unused_channel(true);

    dma_channel_config c;

    // Copies address from fifo to read_data_chan
    c = dma_channel_get_default_config(address_chan);
    channel_config_set_high_priority(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, eb2_address_sm, false));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);

    dma_channel_configure(
        address_chan,
        &c,
        &dma_channel_hw_addr(read_data_chan)->al3_read_addr_trig,
        &pio->rxf[eb2_address_sm],
        1,
        true);

    // Copies data from the memory to fifo
    c = dma_channel_get_default_config(read_data_chan);
    channel_config_set_high_priority(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, eb2_access_sm, true));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_chain_to(&c, address_chan2);

    dma_channel_configure(
        read_data_chan,
        &c,
        &pio->txf[eb2_access_sm],
        NULL, // read address set by DMA
        1,
        false);

    // Copies address from mem_rdata_chan to write_data_chan
    c = dma_channel_get_default_config(address_chan2);
    channel_config_set_high_priority(&c, true);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_chain_to(&c, address_chan);

    dma_channel_configure(
        address_chan2,
        &c,
        &dma_channel_hw_addr(write_data_chan)->al2_write_addr_trig,
        &dma_channel_hw_addr(read_data_chan)->read_addr,
        1,
        false);

    // Copies data from fifo to memory
    c = dma_channel_get_default_config(write_data_chan);
    channel_config_set_high_priority(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, eb2_access_sm, false));
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(
        write_data_chan,
        &c,
        NULL, // write address set by DMA
        &pio->rxf[eb2_access_sm],
        1,
        false);
}

static void atom_if_init(PIO pio) //, irq_handler_t handler)
{
    uint eb2_address_sm = 0;
    eb2_address_program_init(pio, eb2_address_sm);

    uint eb2_access_sm = 1;
    eb2_access_program_init(pio, eb2_access_sm);

    eb_setup_dma(pio, eb2_address_sm, eb2_access_sm);

    pio_enable_sm_mask_in_sync(pio, 1u << eb2_address_sm | 1u << eb2_access_sm);
}

static inline void eb_set_perm_byte(size_t address, enum eb_perm perm)
{
    data.m_8[address * 2 + 1] = perm;
}

static void eb_set_perm(size_t start, enum eb_perm perm, size_t size)
{
    hard_assert(start + size <= BUFFER_SIZE);
    for (size_t i = start; i < start + size; i++)
    {
        eb_set_perm_byte(i, perm);
    }
}

static inline uint8_t memory_get(size_t address)
{
    return data.m_8[address * 2];
}

static inline uint32_t memory_get32(size_t address)
{
    uint32_t result =
        (data.m_8[address * 2] << 24) +
        (data.m_8[address * 2 + 2] << 16)+
        (data.m_8[address * 2 + 4] << 8)+
        (data.m_8[address * 2 + 6] << 0);

    return result;
}

static inline void memory_set(size_t address, unsigned char value)
{
    data.m_8[address * 2] = value;
}

static inline void memory_get_chars(char *buffer, size_t size, size_t address)
{
    for (size_t i = 0; i > size; i++)
    {
        buffer[i] = memory_get(address + i);
    }
}

static inline void memory_set_chars(size_t address, char *buffer, size_t size)
{
    for (size_t i = 0; i > size; i++)
    {
        memory_set(address + i, buffer[i]);
    }
}

static inline void memory_memset(size_t address, char c, size_t size)
{
    for (size_t i = address; i < address + size; i++)
    {
        memory_set(i, c);
    }
}