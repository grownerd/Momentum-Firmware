#include "furi_hal_nfc_i.h"
#include "furi_hal_nfc_tech_i.h"

#include <furi.h>
#include <furi_hal_resources.h>
#include <furi_hal_vibro.h>

#include <digital_signal/presets/nfc/legic_prime_signal.h>
#include <nfc/helpers/legic_prng.h>
#include <nfc/helpers/legic_prime_crc.h>

#define TAG "FuriHalLegicPrime"

// Prevent FDT timer from starting
#define FURI_HAL_NFC_LEGIC_PRIME_LISTENER_FDT_COMP_FC (INT32_MAX)
#define RX_BUF_DEPTH (1024UL)

#if 0
#define LSB_START_T (143)
#define LSB_START_T_TOL (20)
#define BIT_T_0 (48)
#define BIT_T_0_TOL (20)
#define BIT_T_1 (41)
#define BIT_T_1_TOL (20)
#define MAX_ANSWER_BITS (12)
#define ACK_PAUSE_COMP (42)
#define RX_TIMEOUT_T (355)
#define RX_TIMEOUT_ACK_T (253)

#else

#define LSB_START_T (143)
#define LSB_START_T_TOL (20)
#define BIT_T_0 (48)
#define BIT_T_0_TOL (20)
#define BIT_T_1 (41)
#define BIT_T_1_TOL (20)
#define MAX_ANSWER_BITS (12)
#define ACK_PAUSE_COMP (42)
#define RX_FWT_BEFORE_ACK_T (400)
#define RX_TIMEOUT_T (355) // just enough to receive 12 bits!
#define RX_FWT_T (19100) // ~1700us
#define RX_TIMEOUT_ACK_T (250) // just enough to receive 6 bits!
#define RX_FWT_ACK_T (11500) // ~1155us
#define RX_LOOP_T (1)
#endif

static volatile int timeout_ack = RX_TIMEOUT_ACK_T;

static volatile int fwt0 = RX_FWT_BEFORE_ACK_T;;
static volatile int fwt1 = RX_FWT_ACK_T;
static volatile int fwt2 = RX_FWT_T;


static volatile int16_t irq_bits = 0;

static LegicPrime_Signal* legic_prime_signal = NULL;

static uint8_t rx_bytes[RX_BUF_DEPTH];
static uint8_t rx_buf[RX_BUF_DEPTH];
static size_t rx_buf_idx = 0;
static volatile int32_t rx_timeout = 0;
static uint32_t rx_fwt = 0;

static crc_t legic_crc;

// private functions

static int is_within_tolerance(int val, int ref, int tol)
{
    return ((val >= (ref - tol)) && (val <= (ref + tol)));
}

static uint8_t calc_crc4(uint16_t cmd, uint8_t cmd_sz, uint8_t value) {
    crc_clear(&legic_crc);
    crc_update(&legic_crc, (value << cmd_sz) | cmd, 8 + cmd_sz);
    return crc_finish(&legic_crc);
}

static void p_rx_cb(void* ctx)
{
    UNUSED(ctx);
    FURI_CRITICAL_ENTER();
    rx_buf[rx_buf_idx] = 1;
    rx_buf_idx++;
    if (rx_buf_idx >= RX_BUF_DEPTH)
        rx_buf_idx = RX_BUF_DEPTH-1;
    irq_bits++;
    FURI_CRITICAL_EXIT();
}

FuriHalNfcError p_poller_tx(
    uint16_t tx_data,
    size_t tx_bits) {
    furi_check(legic_prime_signal);

    rx_buf_idx = 0;
    memset(rx_buf, 0, sizeof(rx_buf[0]) * RX_BUF_DEPTH);

    // Send signal
    legic_prime_signal_tx(legic_prime_signal, tx_data ^ legic_prng_get_bits(tx_bits), tx_bits);

    switch(tx_bits)
    {
    case 6:
        //rx_fwt = fwt0;
        //rx_timeout = 0;
        //break;
        return FuriHalNfcErrorNone;

    case 7:
        rx_fwt = fwt1;
        //rx_timeout = RX_TIMEOUT_ACK_T;
        rx_timeout = timeout_ack;
        break;

    case 9:
    case 11:
        rx_fwt = fwt2;
        rx_timeout = RX_TIMEOUT_T;
        break;

    default:
        break;
    }

    // configure miso gpio as interrupt pin and add callback to fill buffer
    furi_hal_gpio_init(&gpio_spi_r_miso, GpioModeInterruptRise, GpioPullDown, GpioSpeedVeryHigh);
    furi_hal_gpio_add_int_callback(&gpio_spi_r_miso, p_rx_cb, NULL);

    // Claim highest priority, so we don't get interrupted too often!
    FuriThreadPriority prio = furi_thread_get_current_priority();
    furi_thread_set_current_priority(FuriThreadPriorityHighest);

    furi_hal_nfc_timer_fwt_start(rx_fwt);

    furi_kernel_lock();
    while (true)
    {
        //rx_buf[rx_buf_idx++] = furi_hal_gpio_read(&gpio_spi_r_miso);
        rx_buf[rx_buf_idx++] = 0;

        furi_delay_us(RX_LOOP_T);
        rx_timeout -= irq_bits;
        irq_bits = 0;
        if (rx_timeout-- <= 0) break;
    }
    furi_kernel_unlock();

    // Set prio back to normal
    furi_thread_set_current_priority(prio);

    furi_hal_gpio_remove_int_callback(&gpio_spi_r_miso);
    furi_thread_flags_wait(FuriHalNfcEventInternalTypeTimerFwtExpired, FuriFlagWaitAny, FuriWaitForever);

    return FuriHalNfcErrorNone;
}

uint16_t p_poller_rx( size_t rx_bits) {

    uint16_t bits = 0x0000;

    uint8_t current_symbol = 0;
    uint8_t last_symbol = 0;
    uint32_t consec_bits = 0;
    uint32_t bit_trans = 0;
    uint8_t bit_idx = 0;

    uint32_t i = 0;
    for (; i < rx_buf_idx; i++)
    {
        current_symbol = rx_buf[i];

        if (current_symbol != last_symbol)
        {
            FURI_LOG_T(TAG, "symbol: %x:  count: %4ld", last_symbol, consec_bits);

            for (uint8_t b=1; b < (MAX_ANSWER_BITS+1); b++)
            {
                if ((is_within_tolerance(consec_bits, LSB_START_T, LSB_START_T_TOL)) && (!bit_trans))
                    break;
                else if (consec_bits > (LSB_START_T+LSB_START_T_TOL))
                    consec_bits -= LSB_START_T;

                uint32_t t_bit = last_symbol ? BIT_T_1 : BIT_T_0;
                uint32_t t_tol = last_symbol ? BIT_T_1_TOL : BIT_T_0_TOL;
                uint8_t num_bits = 0;
                if (is_within_tolerance(consec_bits, t_bit*b, t_tol))
                {
                    num_bits = b;
                }
                while (num_bits--)
                {
                    bits |= (last_symbol & 0x01) << bit_idx++;
                }
            }

            bit_trans++;
            consec_bits = 0;
        }
        else consec_bits++;

        last_symbol = current_symbol;
    }
// BREAK!
//__asm volatile("bkpt 0");

    FURI_LOG_D(TAG, "decoded bits: 0%04X, total symbols: %ld, total transitions: %ld", bits, i, bit_trans);

    return bits ^ legic_prng_get_bits(rx_bits);
}


static void p_poller_setup(void)
{
    uint16_t iv_frame = 0x01;
    uint16_t ack_frame = 0;
    uint16_t answer_frame = 0;

    while (true)
    {
        furi_delay_ms(3);
        legic_prng_init(0);

        p_poller_tx(iv_frame, 7);

        legic_prng_init(iv_frame);
        legic_prng_forward(2);

        answer_frame = p_poller_rx(6);
        legic_prng_forward(3);

        switch (answer_frame) {
            case 0x0D:
                //ack_frame = 0x19;
                FURI_LOG_E(TAG, "wrong tag type recognized: 0x%04X", answer_frame);
                break;

            case 0x1D:
            case 0x3D:
                ack_frame = 0x39;
                break;

            default:
                FURI_LOG_E(TAG, "tag type not recognized: 0x%04X", answer_frame);
                break;
        }
        if (ack_frame) break;
    }
    p_poller_tx(ack_frame, 6);
    furi_delay_us(ACK_PAUSE_COMP);
}




// public functions

// This function is used by poller and listener init functions
static FuriHalNfcError furi_hal_nfc_legic_prime_common_init(const FuriHalSpiBusHandle* handle) {

    furi_hal_vibro_on(0);
#if 1
    // This filter was filtering the subcarrier payload!
    // lpf 300kHz, hpf 12 + 80kHz
    st25r3916_write_reg(handle, ST25R3916_REG_RX_CONF1,
        ST25R3916_REG_RX_CONF1_ch_sel_AM
        | ST25R3916_REG_RX_CONF1_lp_300khz
        //| ST25R3916_REG_RX_CONF1_hz_60_200khz
    );
#else
    st25r3916_write_reg(handle, ST25R3916_REG_RX_CONF1, 0x00);
#endif

#if 0
    // AGC doesn't seem to make any difference, other than adding noise!
    // AGC enabled, ratio 3:1, squelch after TX
    st25r3916_write_reg(
        handle,
        ST25R3916_REG_RX_CONF2,
        ST25R3916_REG_RX_CONF2_agc6_3 | ST25R3916_REG_RX_CONF2_agc_m |
            ST25R3916_REG_RX_CONF2_agc_en
            | ST25R3916_REG_RX_CONF2_sqm_dyn
        );
#else
    st25r3916_write_reg( handle, ST25R3916_REG_RX_CONF2, 0x00);
#endif

    // HF operation, full gain on AM and PM channels
    st25r3916_write_reg(handle, ST25R3916_REG_RX_CONF3, 0x00);
    // No gain reduction on AM and PM channels
    st25r3916_write_reg(handle, ST25R3916_REG_RX_CONF4, 0x00);

    // Correlator config (off)
    st25r3916_write_reg(handle, ST25R3916_REG_CORR_CONF1, 0x00);
    st25r3916_write_reg(handle, ST25R3916_REG_CORR_CONF2, 0x00);

    // TX driver full AM modulation (40%)
    st25r3916_write_reg(handle, ST25R3916_REG_TX_DRIVER, ST25R3916_REG_TX_DRIVER_am_mod_40percent);

    // Stream mode config
    st25r3916_write_reg(
        handle, ST25R3916_REG_MODE, ST25R3916_REG_MODE_om_subcarrier_stream | ST25R3916_REG_MODE_tr_am_ook);

    st25r3916_write_reg(
        handle, ST25R3916_REG_STREAM_MODE,
        ST25R3916_REG_STREAM_MODE_scf_sc212
        | ST25R3916_REG_STREAM_MODE_stx_212
        | ST25R3916_REG_STREAM_MODE_scp_1pulse
        //| ST25R3916_REG_STREAM_MODE_scp_8pulses
    );

    st25r3916_write_reg(
        handle, ST25R3916_REG_AUX,
        ST25R3916_REG_AUX_dis_corr_correlator
        //ST25R3916_REG_AUX_dis_corr_coherent
    );
    return FuriHalNfcErrorNone;
}

// Used by FuriHalNfcTechBase
static FuriHalNfcError furi_hal_nfc_legic_prime_poller_init(const FuriHalSpiBusHandle* handle) {
#if 0
    UNUSED(handle);
#else

    furi_check(legic_prime_signal == NULL);
    legic_prime_signal = legic_prime_signal_alloc(&gpio_spi_r_mosi);

    st25r3916_write_reg(
        handle,
        ST25R3916_REG_OP_CONTROL,
        ST25R3916_REG_OP_CONTROL_en | ST25R3916_REG_OP_CONTROL_rx_en |
            ST25R3916_REG_OP_CONTROL_en_fd_auto_efd);
#endif

    return furi_hal_nfc_legic_prime_common_init(handle);
}

// Used by FuriHalNfcTechBase
static FuriHalNfcError furi_hal_nfc_legic_prime_poller_deinit(const FuriHalSpiBusHandle* handle) {
#if 0
    UNUSED(handle);
#else

    if(legic_prime_signal) {
        legic_prime_signal_free(legic_prime_signal);
        legic_prime_signal = NULL;
    }

    st25r3916_write_reg(
        handle,
        ST25R3916_REG_OP_CONTROL,
        ST25R3916_REG_OP_CONTROL_en | ST25R3916_REG_OP_CONTROL_rx_en |
            ST25R3916_REG_OP_CONTROL_en_fd_auto_efd);

    //furi_hal_nfc_low_power_mode_start();
#endif

    return FuriHalNfcErrorNone;
}

// Used by FuriHalNfcTechBase
static FuriHalNfcError furi_hal_nfc_legic_prime_listener_init(const FuriHalSpiBusHandle* handle) {
    furi_check(legic_prime_signal == NULL);
    legic_prime_signal = legic_prime_signal_alloc(&gpio_spi_r_mosi);

#if 1
    UNUSED(handle);
#else
    st25r3916_write_reg(
        handle,
        ST25R3916_REG_OP_CONTROL,
        ST25R3916_REG_OP_CONTROL_en | ST25R3916_REG_OP_CONTROL_rx_en |
            ST25R3916_REG_OP_CONTROL_en_fd_auto_efd);
    st25r3916_write_reg(
        handle, ST25R3916_REG_MODE, ST25R3916_REG_MODE_targ_targ | ST25R3916_REG_MODE_om0);
    st25r3916_write_reg(
        handle,
        ST25R3916_REG_PASSIVE_TARGET,
        ST25R3916_REG_PASSIVE_TARGET_fdel_2 | ST25R3916_REG_PASSIVE_TARGET_fdel_0 |
            ST25R3916_REG_PASSIVE_TARGET_d_ac_ap2p | ST25R3916_REG_PASSIVE_TARGET_d_212_424_1r);

    st25r3916_write_reg(handle, ST25R3916_REG_MASK_RX_TIMER, 0x02);

    st25r3916_direct_cmd(handle, ST25R3916_CMD_STOP);
    uint32_t interrupts =
        (ST25R3916_IRQ_MASK_FWL | ST25R3916_IRQ_MASK_TXE | ST25R3916_IRQ_MASK_RXS |
         ST25R3916_IRQ_MASK_RXE | ST25R3916_IRQ_MASK_PAR | ST25R3916_IRQ_MASK_CRC |
         ST25R3916_IRQ_MASK_ERR1 | ST25R3916_IRQ_MASK_ERR2 | ST25R3916_IRQ_MASK_NRE |
         ST25R3916_IRQ_MASK_EON | ST25R3916_IRQ_MASK_EOF | ST25R3916_IRQ_MASK_WU_A_X |
         ST25R3916_IRQ_MASK_WU_A);
    // Clear interrupts
    st25r3916_get_irq(handle);
    // Enable interrupts
    st25r3916_mask_irq(handle, ~interrupts);
    // Enable auto collision resolution
    st25r3916_clear_reg_bits(
        handle, ST25R3916_REG_PASSIVE_TARGET, ST25R3916_REG_PASSIVE_TARGET_d_106_ac_a);
    st25r3916_direct_cmd(handle, ST25R3916_CMD_GOTO_SENSE);
#endif

    return furi_hal_nfc_legic_prime_common_init(handle);
}

// Used by FuriHalNfcTechBase
static FuriHalNfcError furi_hal_nfc_legic_prime_listener_deinit(const FuriHalSpiBusHandle* handle) {
    UNUSED(handle);

    if(legic_prime_signal) {
        legic_prime_signal_free(legic_prime_signal);
        legic_prime_signal = NULL;
    }

    return FuriHalNfcErrorNone;
}

// Used by FuriHalNfcTechBase
static FuriHalNfcEvent furi_hal_nfc_legic_prime_listener_wait_event(uint32_t timeout_ms) {
    FuriHalNfcEvent event = furi_hal_nfc_wait_event_common(timeout_ms);
    const FuriHalSpiBusHandle* handle = &furi_hal_spi_bus_handle_nfc;

    if(event & FuriHalNfcEventListenerActive) {
        st25r3916_set_reg_bits(
            handle, ST25R3916_REG_PASSIVE_TARGET, ST25R3916_REG_PASSIVE_TARGET_d_106_ac_a);
    }

    return event;
}


// Used by FuriHalNfcTechBase
FuriHalNfcError furi_hal_nfc_legic_prime_listener_tx(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* tx_data,
    size_t tx_bits) {
    FuriHalNfcError error = FuriHalNfcErrorNone;

#if 1
    UNUSED(handle);
    UNUSED(tx_data);
    UNUSED(tx_bits);
#else
    do {
        error = furi_hal_nfc_common_fifo_tx(handle, tx_data, tx_bits);
        if(error != FuriHalNfcErrorNone) break;

        bool tx_end = furi_hal_nfc_event_wait_for_specific_irq(handle, ST25R3916_IRQ_MASK_TXE, 10);
        if(!tx_end) {
            error = FuriHalNfcErrorCommunicationTimeout;
            break;
        }

    } while(false);
#endif

    return error;
}

// Used by FuriHalNfcTechBase
FuriHalNfcError furi_hal_nfc_legic_prime_listener_sleep(const FuriHalSpiBusHandle* handle) {
#if 1
    UNUSED(handle);
#else
    // Enable auto collision resolution
    st25r3916_clear_reg_bits(
        handle, ST25R3916_REG_PASSIVE_TARGET, ST25R3916_REG_PASSIVE_TARGET_d_106_ac_a);
    st25r3916_direct_cmd(handle, ST25R3916_CMD_STOP);
    st25r3916_direct_cmd(handle, ST25R3916_CMD_GOTO_SLEEP);
#endif

    return FuriHalNfcErrorNone;
}

// Used by FuriHalNfcTechBase
FuriHalNfcError furi_hal_nfc_legic_prime_listener_idle(const FuriHalSpiBusHandle* handle) {
#if 1
    UNUSED(handle);
#else
    // Enable auto collision resolution
    st25r3916_clear_reg_bits(
        handle, ST25R3916_REG_PASSIVE_TARGET, ST25R3916_REG_PASSIVE_TARGET_d_106_ac_a);
    st25r3916_direct_cmd(handle, ST25R3916_CMD_STOP);
    st25r3916_direct_cmd(handle, ST25R3916_CMD_GOTO_SENSE);
#endif

    return FuriHalNfcErrorNone;
}


FuriHalNfcError furi_hal_nfc_legic_prime_poller_tx(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* tx_data,
    size_t tx_bits) {
    furi_check(tx_data);
    furi_check(legic_prime_signal);
    UNUSED(tx_bits);

    st25r3916_direct_cmd(handle, ST25R3916_CMD_TRANSPARENT_MODE);
    // Reconfigure gpio for Transparent mode
    furi_hal_spi_bus_handle_deinit(&furi_hal_spi_bus_handle_nfc);

    // Send signal

    do
    {
        p_poller_setup();

        uint32_t crc_errors = 0;
        for (int h=0; h<256; h++)
        {
            legic_prng_forward(2);
            uint16_t addr = (0 << 1 | 1);
            p_poller_tx(addr, 9);
            legic_prng_forward(2);

            uint16_t rx_data = p_poller_rx(12);

            if (rx_data)
            {
                //FURI_LOG_I(TAG, "read byte %3d: 0x%04X", h, rx_data);
            }

            // split frame into data and crc
            uint8_t byte = rx_data & 0xff;
            uint8_t crc = (rx_data >> 1) & 0xf;
            uint16_t cmd = 1;
            uint8_t cmd_sz = 9;

            // check received against calculated crc
            uint8_t calc_crc = calc_crc4(cmd, cmd_sz, byte);
            if (calc_crc != crc) {
                //FURI_LOG_E(TAG, "!!! crc mismatch: %x != %x !!!",  calc_crc, crc);
                crc_errors++;
                //break;
            }
            else //if (byte == 0x81)
            {
                //FURI_LOG_T(TAG, "CRC MATCH!!! addr: 0x%04X", h);
                rx_bytes[h] = byte;
            }
            legic_prng_forward(1);
        }
        FURI_LOG_I(TAG, "CRC errors: %ld", crc_errors);
    }
    while (true);

    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_legic_prime_poller_rx(
    const FuriHalSpiBusHandle* handle,
    uint8_t* rx_data,
    size_t rx_data_size,
    size_t* rx_bits) {
    UNUSED(handle);
    UNUSED(rx_data);
    UNUSED(rx_data_size);
    UNUSED(rx_bits);

    //p_poller_rx(rx_data, rx_data_size, rx_bits);

    // Configure gpio back to SPI and exit transparent
    
    furi_hal_gpio_write(&gpio_spi_r_mosi, false);
    furi_hal_spi_bus_handle_init(&furi_hal_spi_bus_handle_nfc);

    return FuriHalNfcErrorNone;
}

const FuriHalNfcTechBase furi_hal_nfc_legic_prime = {
    .poller =
        {
            .compensation =
                {
                    //.fdt = FURI_HAL_NFC_POLLER_FDT_COMP_FC,
                    //.fwt = FURI_HAL_NFC_POLLER_FWT_COMP_FC,
// positive values shorten timeoute, negative values extend timeouts!!!
// but these don't seem to to anything at all!
                    .fdt = (0),
                    .fwt = (0),
                },
            .init = furi_hal_nfc_legic_prime_poller_init,
            .deinit = furi_hal_nfc_legic_prime_poller_deinit,
            .wait_event = furi_hal_nfc_wait_event_common,
            .tx = furi_hal_nfc_legic_prime_poller_tx,
            .rx = furi_hal_nfc_legic_prime_poller_rx,
        },

    .listener =
        {
            .compensation =
                {
                    .fdt = FURI_HAL_NFC_LEGIC_PRIME_LISTENER_FDT_COMP_FC,
                },
            .init = furi_hal_nfc_legic_prime_listener_init,
            .deinit = furi_hal_nfc_legic_prime_listener_deinit,
            .wait_event = furi_hal_nfc_legic_prime_listener_wait_event,
            .tx = furi_hal_nfc_legic_prime_listener_tx,
            .rx = furi_hal_nfc_common_fifo_rx,
            .sleep = furi_hal_nfc_legic_prime_listener_sleep,
            .idle = furi_hal_nfc_legic_prime_listener_idle,
        },
};
