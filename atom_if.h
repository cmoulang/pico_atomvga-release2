#pragma once

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "sm.pio.h"

#define EB_ADD_BITS 16
#define EB_BUFFER_SIZE 0x10000

enum eb_perm
{
    EB_PERM_WRITE_ONLY = 0b00,
    EB_PERM_READ_WRITE = 0b01,
    EB_PERM_NO_ACCESS = 0b10,
    EB_PERM_READ_ONLY = 0b11,
};

static_assert(sizeof(void *) == 4);
static_assert(sizeof(int) == 4);
static_assert(sizeof(short) == 2);

// Buffer spans 6502 address space
volatile _Alignas(EB_BUFFER_SIZE) union
{
    volatile uint16_t m_16[EB_BUFFER_SIZE];
    volatile uint8_t m_8[EB_BUFFER_SIZE * 2];
} eb_memory;

#define EB_EVENT_QUEUE_BITS 5
#define EB_EVENT_QUEUE_LEN ((1 << EB_EVENT_QUEUE_BITS) / 4)
volatile _Alignas(1 << EB_EVENT_QUEUE_BITS) u_int32_t eb_event_queue[EB_EVENT_QUEUE_LEN];

static void eb2_address_program_init(PIO pio, uint sm)
{
    uint offset;

#if (R65C02==1)
    offset = pio_add_program(pio, &eb2_addr_65C02_program);
    pio_sm_config c = eb2_addr_65C02_program_get_default_config(offset);
#else
    offset = pio_add_program(pio, &eb2_addr_other_program);
    pio_sm_config c = eb2_addr_other_program_get_default_config(offset);
#endif

    (pio)->input_sync_bypass = (0xFF << PIN_A0) | (1 << PIN_R_NW);

    for (int pin = PIN_A0; pin < PIN_A0 + 8; pin++)
    {
        pio_gpio_init(pio, pin);
        gpio_set_pulls(pin, true, false);
    }

    for (int pin = PIN_MUX_DATA; pin < PIN_MUX_DATA + 3; pin++)
    {
        pio_gpio_init(pio, pin);
        gpio_set_pulls(pin, true, false);
    }

    pio_sm_set_pins_with_mask(pio, sm,
                              (0x07 << PIN_MUX_DATA),
                              (0x07 << PIN_MUX_DATA));

    pio_sm_set_consecutive_pindirs(pio, sm, PIN_MUX_DATA, 3, true);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_A0, 8, false);

    sm_config_set_jmp_pin(&c, PIN_A0 + 7);      // == A15
    sm_config_set_in_pins(&c, PIN_A0);
    sm_config_set_out_pins(&c, PIN_A0, 8);
    sm_config_set_set_pins(&c, PIN_A0, 8);

    sm_config_set_sideset(&c, 4, true, false);
    sm_config_set_sideset_pins(&c, PIN_MUX_DATA);

    sm_config_set_in_shift(&c, false, true, 16);

    // Calculate address for low and high 64K chunks: 0x20012002 on rp2040
    uint address = (uint)&eb_memory >> 16;
    address = (address << 16) | (address + 1);

    pio_sm_put(pio, sm, address); 
    pio_sm_exec(pio, sm, pio_encode_pull(false, true));
    pio_sm_exec(pio, sm, pio_encode_mov(pio_x, pio_osr));

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
static uint event_queue_chan;

static void eb_setup_dma(PIO pio, int eb2_address_sm,
                         int eb2_access_sm)
{
    address_chan = dma_claim_unused_channel(true);
    read_data_chan = dma_claim_unused_channel(true);
    address_chan2 = dma_claim_unused_channel(true);
    write_data_chan = dma_claim_unused_channel(true);
    event_queue_chan = dma_claim_unused_channel(true);

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
    //channel_config_set_dreq(&c, pio_get_dreq(pio, eb2_access_sm, true));
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
    channel_config_set_chain_to(&c, event_queue_chan);
    dma_channel_configure(
        write_data_chan,
        &c,
        NULL, // write address set by DMA
        &pio->rxf[eb2_access_sm],
        1,
        false);

    // Updates the event queue
    c = dma_channel_get_default_config(event_queue_chan);
    channel_config_set_high_priority(&c, true);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, EB_EVENT_QUEUE_BITS);
    dma_channel_configure(
        event_queue_chan,
        &c,
        &eb_event_queue,
        &dma_channel_hw_addr(write_data_chan)->write_addr,
        1,
        false);
}

/// @brief initialise and start the PIO and DMA interface to the 6502 bus
/// @param pio the pio instance to use
static void eb_init(PIO pio) //, irq_handler_t handler)
{
    uint eb2_address_sm = 0;
    eb2_address_program_init(pio, eb2_address_sm);

    uint eb2_access_sm = 1;
    eb2_access_program_init(pio, eb2_access_sm);

    eb_setup_dma(pio, eb2_address_sm, eb2_access_sm);

    pio_enable_sm_mask_in_sync(pio, 1u << eb2_address_sm | 1u << eb2_access_sm);
}

/// @brief set the read/write permissions for an address
/// @param address 6502 address
/// @param  perm see enum for possible values
static inline void eb_set_perm_byte(uint16_t address, enum eb_perm perm)
{
    eb_memory.m_8[address * 2 + 1] = perm;
}

/// @brief set the read/write permissions for a range of addresses
/// @param start 6502 starting address
/// @param  perm see enum for possible values
/// @param size number of bytes to set
static void eb_set_perm(uint16_t start, enum eb_perm perm, size_t size)
{
    hard_assert(start + size <= EB_BUFFER_SIZE);
    for (size_t i = start; i < start + size; i++)
    {
        eb_set_perm_byte(i, perm);
    }
}

/// @brief get a byte value
/// @param address the 6502 address
/// @return the value of the byte
static inline uint8_t eb_get(uint16_t address)
{
    return eb_memory.m_8[address * 2];
}

/// @brief get a 32 value
/// @param address the 6502 address
/// @return the 32 bit value
static inline uint32_t eb_get32(uint16_t address)
{
    uint32_t result =
        (eb_memory.m_8[address * 2] << 24) +
        (eb_memory.m_8[address * 2 + 2] << 16)+
        (eb_memory.m_8[address * 2 + 4] << 8)+
        (eb_memory.m_8[address * 2 + 6] << 0);

    return result;
}

/// @brief set a byte to a new value
/// @param address the 6502 address
/// @param value the new value
static inline void eb_set(uint16_t address, unsigned char value)
{
    eb_memory.m_8[address * 2] = value;
}

/// @brief get a string of chars
/// @param buffer destination buffer
/// @param size number of chars to get
/// @param address 6502 address of source
static inline void eb_get_chars(char *buffer, size_t size, uint16_t address)
{
    for (size_t i = 0; i < size; i++)
    {
        buffer[i] = eb_get(address + i);
    }
}

/// @brief copy a string of chars to memory
/// @param address 6502 destination address
/// @param buffer source
/// @param size number of chars to copy
static inline void eb_set_chars(uint16_t address, char *buffer, size_t size)
{
    hard_assert(address + size <= EB_BUFFER_SIZE);
    for (size_t i = 0; i < size; i++)
    {
        eb_set(address + i, buffer[i]);
    }
}

/// @brief copies a value to each location starting at address
/// @param address the address to start at
/// @param c the value to copy
/// @param size the number of loactions to set
static inline void eb_memset(uint16_t address, char c, size_t size)
{
    hard_assert(address + size <= EB_BUFFER_SIZE);
    for (size_t i = address; i < address + size; i++)
    {
        eb_set(i, c);
    }
}

