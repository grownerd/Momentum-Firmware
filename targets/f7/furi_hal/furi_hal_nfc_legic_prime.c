#include "furi_hal_nfc_i.h"
#include "furi_hal_nfc_tech_i.h"

#include <furi.h>
#include <furi_hal_resources.h>

#include <digital_signal/presets/nfc/legic_prime_signal.h>

#define TAG "FuriHalLegicPrime"

// Prevent FDT timer from starting
#define FURI_HAL_NFC_LEGIC_PRIME_LISTENER_FDT_COMP_FC (INT32_MAX)

static LegicPrime_Signal* legic_prime_signal = NULL;

// TODO: record timestamps for interrupts!
typedef struct {
    bool level;
    uint32_t ticks;
} TagBit;
static TagBit tag_bit[100];
static bool last_bit = 0;

static uint8_t rx_buf[1024];
static size_t rx_buf_idx = 0;

// This function is used by poller and listener init functions
static FuriHalNfcError furi_hal_nfc_legic_prime_common_init(const FuriHalSpiBusHandle* handle) {

    // lpf 300kHz, hpf 12 + 80kHz
    st25r3916_write_reg(handle, ST25R3916_REG_RX_CONF1, ST25R3916_REG_RX_CONF1_lp_300khz |
        ST25R3916_REG_RX_CONF1_hz_12_80khz);

// TODO: do we need to change this?
    // AGC enabled, ratio 3:1, squelch after TX
    st25r3916_write_reg(
        handle,
        ST25R3916_REG_RX_CONF2,
        ST25R3916_REG_RX_CONF2_agc6_3 | ST25R3916_REG_RX_CONF2_agc_m |
            ST25R3916_REG_RX_CONF2_agc_en | ST25R3916_REG_RX_CONF2_sqm_dyn);

// TODO: do we need to change this?
    // HF operation, full gain on AM and PM channels
    st25r3916_write_reg(handle, ST25R3916_REG_RX_CONF3, 0x00);
    // No gain reduction on AM and PM channels
    st25r3916_write_reg(handle, ST25R3916_REG_RX_CONF4, 0x00);

    // tx driver full am modulation (40%)
    st25r3916_write_reg(handle, ST25R3916_REG_TX_DRIVER, ST25R3916_REG_TX_DRIVER_am_mod_40percent);

// FIXME: disable this properly!
#if 0
    // Correlator config
    st25r3916_write_reg(
        handle,
        ST25R3916_REG_CORR_CONF1,
        ST25R3916_REG_CORR_CONF1_corr_s0 | ST25R3916_REG_CORR_CONF1_corr_s4 |
            ST25R3916_REG_CORR_CONF1_corr_s6);
    // Sleep mode disable, 424kHz mode off
#endif
    st25r3916_write_reg(handle, ST25R3916_REG_CORR_CONF1, 0x00);
    st25r3916_write_reg(handle, ST25R3916_REG_CORR_CONF2, 0x00);

    st25r3916_write_reg(
        handle, ST25R3916_REG_MODE, ST25R3916_REG_MODE_om_subcarrier_stream);

    st25r3916_write_reg(
        handle, ST25R3916_REG_STREAM_MODE, ST25R3916_REG_STREAM_MODE_scf_sc212);
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

static void furi_hal_legic_prime_rx_cb(void* ctx)
{
    UNUSED(ctx);

    //rx_buf[rx_buf_idx % 1024] = furi_hal_gpio_read(&gpio_spi_r_miso);
    //tag_bit[rx_buf_idx % 100].level = furi_hal_gpio_read(&gpio_spi_r_miso);
    tag_bit[rx_buf_idx % 100].level = !last_bit;
    tag_bit[rx_buf_idx % 100].ticks = furi_get_tick();
    rx_buf_idx++;

    // This doesn't work or we never get any data...
    // FIXME: this needs to be done with a timer!
    //if (rx_buf_idx >= 1024)

        //furi_hal_nfc_event_set(FuriHalNfcEventInternalTypeTimerFwtExpired);
        //furi_hal_nfc_event_set(FuriHalNfcEventInternalTypeTransparentDataReceived);
}

FuriHalNfcError furi_hal_nfc_legic_prime_poller_tx(
    const FuriHalSpiBusHandle* handle,
    const uint8_t* tx_data,
    size_t tx_bits) {
    furi_check(tx_data);

    furi_check(legic_prime_signal);

    //st25r3916_direct_cmd(handle, ST25R3916_CMD_UNMASK_RECEIVE_DATA);

    memset(tag_bit, 0, sizeof(TagBit) * 100);
    rx_buf_idx = 0;

    st25r3916_direct_cmd(handle, ST25R3916_CMD_TRANSPARENT_MODE);
    // Reconfigure gpio for Transparent mode
    furi_hal_spi_bus_handle_deinit(&furi_hal_spi_bus_handle_nfc);

    // configure miso gpio as interrupt pin and add callback to fill buffer
    furi_hal_gpio_init(&gpio_spi_r_miso, GpioModeInterruptRiseFall, GpioPullDown, GpioSpeedVeryHigh);
    furi_hal_gpio_add_int_callback(&gpio_spi_r_miso, furi_hal_legic_prime_rx_cb, NULL);

    // Send signal
    legic_prime_signal_tx(legic_prime_signal, tx_data, tx_bits);

    // this fucks with the mosi line and should be avoided until we're done reading!
    furi_delay_ms(2); // 2ms is plenty!
    furi_hal_nfc_event_set(FuriHalNfcEventInternalTypeTimerBlockTxExpired);

    return FuriHalNfcErrorNone;
}

FuriHalNfcError furi_hal_nfc_legic_prime_poller_rx(
    const FuriHalSpiBusHandle* handle,
    uint8_t* rx_data,
    size_t rx_data_size,
    size_t* rx_bits) {

    UNUSED(handle);
    UNUSED(rx_data_size);

    // read internal rx buffer

    memcpy(rx_data, rx_buf, rx_buf_idx);
    *rx_bits = rx_buf_idx;

    furi_hal_gpio_remove_int_callback(&gpio_spi_r_miso);

    // TODO: do this after rx is done
    // Configure gpio back to SPI and exit transparent
    //furi_hal_gpio_write(&gpio_spi_r_mosi, false);
    //furi_hal_spi_bus_handle_init(&furi_hal_spi_bus_handle_nfc);

    return FuriHalNfcErrorNone;
}

const FuriHalNfcTechBase furi_hal_nfc_legic_prime = {
    .poller =
        {
            .compensation =
                {
                    .fdt = FURI_HAL_NFC_POLLER_FDT_COMP_FC,
                    .fwt = FURI_HAL_NFC_POLLER_FWT_COMP_FC,
                },
            .init = furi_hal_nfc_legic_prime_poller_init,
            .deinit = furi_hal_nfc_legic_prime_poller_deinit,
            .wait_event = furi_hal_nfc_wait_event_common,
            .tx = furi_hal_nfc_legic_prime_poller_tx,
            .rx = furi_hal_nfc_legic_prime_poller_rx,
            //.rx = furi_hal_nfc_common_fifo_rx,
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
