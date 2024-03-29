/* Copyright (C) 1883 Thomas Edison - All Rights Reserved
 * You may use, distribute and modify this code under the
 * terms of the GPLv2 license, which unfortunately won't be
 * written for another century.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <limits.h>
#include <memory.h>
#include <stdlib.h>

#include "comm.h"
#include "comm.pio.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"

#include "pico/multicore.h"
#include "pico/platform.h"
#include "pico/sync.h"

#define PACKED __attribute__((__packed__))

#define PIO pio0       // TX/RX PIO
#define SM_CLK_DIV 2   // 125 MHz / 2
#define ID0_GPIO 10    // bit 0 of node id
#define ID1_GPIO 11    // bit 1 of 2 bit node id
#define TX0_GPIO 2     // pio UART TX pin
#define RX0_GPIO 6     // pio UART RX pin

// Packet header
typedef struct PACKED {
    uint16_t length : 12; // length of packet data in bytes
    uint16_t from : 4;    // source node
    uint8_t to : 4;       // destination node
    uint8_t hops : 4;     // hop count
    uint8_t data[0];      // message data
} pkt_t;

// Buffer
typedef struct PACKED buffer {
    struct buffer* next; // next q entry
    pkt_t pkt;           // packet data
} buf_t;

// Queue
typedef volatile struct {
    buf_t* head; // first entry
    buf_t* tail; // last entry
} q_t;

static spin_lock_t* lock;               // queue mutual exclusion
static int node_id;                     // This node's id
static int rx_sm, tx_sm;                // rx/tx PIO state machines
static int tx_ch, rx_ctl_ch, rx_dat_ch; // dma channels
static q_t free_q, tx_q, rx_q;          // free buffer pool, tx & rx queues
static buf_t *tx_buf, *rx_buf;          // current receive and transmit buffer
static volatile int tx_bsy;             // transmitter busy status
static void (*idle_handler)(void) = NULL; // core1 idle call
static void (*rx_hook)(void) = NULL;      // application rx signalling

// Low level functions. Must be called with lock held.

// push back
static inline void enq(q_t* q, buf_t* buf) {
    buf->next = NULL;
    if (q->head == NULL)
        q->head = buf;
    else
        q->tail->next = buf;
    q->tail = buf;
}

// pop front
static inline buf_t* deq(q_t* q) {
    if (q->head == NULL)
        return NULL;
    buf_t* buf = q->head;
    q->head = buf->next;
    return buf;
}

// Get buffer from free q. If empty then allocate a new one
static inline buf_t* get_buffer() {
    buf_t* buf = deq(&free_q);
    if (buf == NULL)
        buf = malloc(sizeof(buf_t) + COMM_PKT_SIZE);
#if !PICO_MALLOC_PANIC
    if (buf == NULL)
        panic("No RX/TX buffers");
#endif
    return buf;
}

// Start transmit of 1st entry in tx q
static void start_tx(void) {
    // if tx dma already bsy, no need to restart it
    if (tx_bsy)
        return;
    tx_buf = deq(&tx_q); // deq pending message
    if (tx_buf == NULL)
        return; // Nothing to send, don't start
    tx_bsy = 1;
    // configure and start the TX DMA channel
    // temporarilly use next pointer as word count
    uint words = (tx_buf->pkt.length + sizeof(pkt_t) + 3) & ~3;
    tx_buf->next = (buf_t*)words;
    dma_channel_set_trans_count(tx_ch, words + 1, false);
    // good to go
    dma_channel_set_read_addr(tx_ch, tx_buf, true);
}

// Start the 1st rx dma to retrieve the buffer length into the 2nd rx dma descriptor
static void start_rx(void) {
    // allocate a max size receive buffer
    rx_buf = get_buffer();
    // the length will be written directly to the data DMA descriptor
    dma_channel_set_write_addr(rx_dat_ch, &rx_buf->pkt, false);
    dma_channel_start(rx_ctl_ch);
}

// Direct incoming packet to the appropriate destination
static bool forward(buf_t* buf) {
    bool enq_rx = false;
    if (buf->pkt.to == node_id) {
        enq(&rx_q, buf);
        enq_rx = true;
    } else {
        if (buf->pkt.hops == 0)
            enq(buf->pkt.to == COMM_NODES ? &rx_q : &free_q, buf);
        else {
            if (buf->pkt.to == COMM_NODES) { // broadcast
                buf_t* buf2 = get_buffer();
                // make a copy and q it to receive q
                memcpy(&buf2->pkt, &buf->pkt, sizeof(pkt_t) + buf->pkt.length);
                enq(&rx_q, buf2);
                enq_rx = true;
            }
            enq(&tx_q, buf); // enq for retransmission
        }
    }
    return enq_rx;
}

// End of functions called with lock

// Internal functions. IRQ and DMA management

// Handle TX or RX dma done interrupt
static void dma_irq0_handler(void) {
    // Handle receive first since we may be enqueuing a transmit
    if (dma_channel_get_irq0_status(rx_dat_ch)) {
        dma_irqn_acknowledge_channel(DMA_IRQ_0, rx_dat_ch);
        // decrement the hop count
        rx_buf->pkt.hops--;
        // forward the message
        uint msk = spin_lock_blocking(lock);
        if (forward(rx_buf) && rx_hook) rx_hook();
        start_rx(); // restart rx for next message length
        start_tx(); // restart tx if not already started
        spin_unlock(lock, msk);
    }
    // handle transmit
    if (dma_channel_get_irq0_status(tx_ch)) {
        dma_irqn_acknowledge_channel(DMA_IRQ_0, tx_ch);
        uint msk = spin_lock_blocking(lock);
        if (tx_buf) // Free transmitted buffer
            enq(&free_q, tx_buf);
        tx_bsy = 0;
        start_tx(); // restart TX if more left
        spin_unlock(lock, msk);
    }
}

// Initialization (called once)

static void common_rx_dma_config(dma_channel_config* cfg) {
    channel_config_set_transfer_data_size(cfg, DMA_SIZE_32);
    channel_config_set_read_increment(cfg, false);
    channel_config_set_dreq(cfg, pio_get_dreq(PIO, rx_sm, false));
}

static void wait_for(int n) {
    while (gpio_get(RX0_GPIO) != n)
        ;
}

void comm_init_from_core1(void) {
    // read node id
    gpio_set_dir(ID0_GPIO, GPIO_IN);
    gpio_set_dir(ID1_GPIO, GPIO_IN);
    node_id = gpio_get(ID0_GPIO) | (gpio_get(ID1_GPIO) << 1);

    // sync (get all nodes up and ready)
    gpio_init(RX0_GPIO);
    gpio_init(TX0_GPIO);
    gpio_set_dir(RX0_GPIO, GPIO_IN);
    gpio_set_dir(TX0_GPIO, GPIO_IN);
    if (node_id != 0) {
        gpio_put(TX0_GPIO, 0);
        gpio_set_dir(TX0_GPIO, GPIO_OUT);
        wait_for(0);
        gpio_put(TX0_GPIO, 1);
        wait_for(1);
    } else {
        wait_for(0);
        gpio_put(TX0_GPIO, 0);
        gpio_set_dir(TX0_GPIO, GPIO_OUT);
        wait_for(1);
        gpio_put(TX0_GPIO, 1);
    }

    // 32 bit PIO UART
    // TX pio
    tx_sm = pio_claim_unused_sm(PIO, true);
    pio_gpio_init(PIO, TX0_GPIO);
    pio_gpio_init(PIO, TX0_GPIO + 1);
    pio_gpio_init(PIO, TX0_GPIO + 2);
    pio_gpio_init(PIO, TX0_GPIO + 3);
    pio_sm_set_pins_with_mask(PIO, tx_sm, 0xf << TX0_GPIO, 0xf << TX0_GPIO);
    pio_sm_set_pindirs_with_mask(PIO, tx_sm, 0xf << TX0_GPIO, 0xf << TX0_GPIO);
    uint offset = pio_add_program(PIO, &uart_tx_program);
    pio_sm_config sm_cfg = uart_tx_program_get_default_config(offset);
    sm_config_set_out_shift(&sm_cfg, true, false, 32);
    sm_config_set_out_pins(&sm_cfg, TX0_GPIO, 4);
    sm_config_set_sideset_pins(&sm_cfg, TX0_GPIO);
    sm_config_set_clkdiv(&sm_cfg, SM_CLK_DIV);
    pio_sm_init(PIO, tx_sm, offset, &sm_cfg);
    pio_sm_set_enabled(PIO, tx_sm, true);

    // RX pio
    rx_sm = pio_claim_unused_sm(PIO, true);
    pio_gpio_init(PIO, RX0_GPIO);
    pio_gpio_init(PIO, RX0_GPIO + 1);
    pio_gpio_init(PIO, RX0_GPIO + 2);
    pio_gpio_init(PIO, RX0_GPIO + 3);
    pio_sm_set_pindirs_with_mask(PIO, rx_sm, 0, 0xf << RX0_GPIO);
    offset = pio_add_program(PIO, &uart_rx_program);
    sm_cfg = uart_rx_program_get_default_config(offset);
    sm_config_set_in_pins(&sm_cfg, RX0_GPIO);  // for WAIT, IN
    // Shift to right, autopush enabled
    sm_config_set_in_shift(&sm_cfg, true, true, 32);
    // SM transmits 4 bits per 8 execution cycles.
    sm_config_set_clkdiv(&sm_cfg, SM_CLK_DIV);
    pio_sm_init(PIO, rx_sm, offset, &sm_cfg);
    pio_sm_set_enabled(PIO, rx_sm, true);

    // tx DMA
    free_q.head = free_q.tail = tx_q.head = tx_q.tail = rx_q.head = rx_q.tail = NULL;
    tx_buf = NULL;
    tx_bsy = 0;
    tx_ch = dma_claim_unused_channel(true); // get dma channel for tx
    dma_channel_config dma_cfg = dma_channel_get_default_config(tx_ch);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_32);  // 32 bits per transfer
    channel_config_set_write_increment(&dma_cfg, false);
    channel_config_set_dreq(&dma_cfg, pio_get_dreq(PIO, tx_sm, true));  // UART TX dreq driven DMA
    dma_channel_set_irq0_enabled(tx_ch, true);                       // interrupt when done
    // write to UART TX data register, no increment
    dma_channel_configure(tx_ch, &dma_cfg, &PIO->txf[tx_sm], NULL, 1, false);

    // rx DMA
    // control channel
    rx_ctl_ch = dma_claim_unused_channel(true);
    rx_dat_ch = dma_claim_unused_channel(true);
    // control channel
    dma_cfg = dma_channel_get_default_config(rx_ctl_ch);
    common_rx_dma_config(&dma_cfg);
    channel_config_set_write_increment(&dma_cfg, false);
    dma_channel_configure(rx_ctl_ch, &dma_cfg, &dma_hw->ch[rx_dat_ch].al1_transfer_count_trig,
                          &PIO->rxf[rx_sm], 1, false);
    // data channel
    dma_cfg = dma_channel_get_default_config(rx_dat_ch);
    common_rx_dma_config(&dma_cfg);
    channel_config_set_write_increment(&dma_cfg, true);
    dma_channel_configure(rx_dat_ch, &dma_cfg, 0, &PIO->rxf[rx_sm], 0, false);
    dma_channel_set_irq0_enabled(rx_dat_ch, true);

    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    lock = spin_lock_init(spin_lock_claim_unused(true));
    uint msk = spin_lock_blocking(lock);
    start_rx();
    spin_unlock(lock, msk);
}

static void init_core1(void) {
    comm_init_from_core1();
    multicore_fifo_push_blocking(0); // tell core 0 we're ready

    // init complete, enter forever idle loop
    for (;;)                         // handle interrupts
        if (idle_handler)
            idle_handler();
        else
            __wfi();
}

// Public functions

// Launch core 1 to initialize communication
void comm_init(void (*idle)(void), void (*rx_interrupt_hook)(void)) {
    idle_handler = idle;
    rx_hook = rx_interrupt_hook;
    multicore_launch_core1(init_core1);
    multicore_fifo_pop_blocking(); // wait for core 1 initialized
}

// transmit a packet
void comm_transmit(int to, const void* buffer, int length) {
    if (length > COMM_PKT_SIZE)
        length = COMM_PKT_SIZE;
    uint msk = spin_lock_blocking(lock);
    buf_t* buf = get_buffer();
    spin_unlock(lock, msk);
    buf->pkt.length = length;
    buf->pkt.from = node_id;
    buf->pkt.to = to;
    buf->pkt.hops = COMM_NODES - 1;
    if (length)
        memcpy(buf->pkt.data, buffer, length);
    msk = spin_lock_blocking(lock);
    if (forward(buf) && rx_hook) rx_hook();
    start_tx();
    spin_unlock(lock, msk);
}

// Receive ready ?
int comm_receive_ready(void) { return rx_q.head != NULL; }

// Receive a packet
int comm_receive_blocking(int* from, void* buffer, int buf_length) {
    while (!comm_receive_ready())
        tight_loop_contents();
    uint msk = spin_lock_blocking(lock);
    buf_t* buf = deq(&rx_q);
    spin_unlock(lock, msk);
    if (from)
        *from = buf->pkt.from;
    uint8_t l = buf->pkt.length;
    if (l > buf_length)
        l = buf_length;
    if (l)
        memcpy(buffer, buf->pkt.data, l);
    msk = spin_lock_blocking(lock);
    enq(&free_q, buf);
    spin_unlock(lock, msk);
    return l;
}

// return node's id
int comm_id(void) { return node_id; }

// return effective communication baud rate (8 PIO clocks per bit)
int comm_baud(void) { return clock_get_hz(clk_sys) / (SM_CLK_DIV * BIT_CLKS); }
