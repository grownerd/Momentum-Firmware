#include "furi_hal_nfc_i.h"
#include "furi_hal_nfc_tech_i.h"

#include <furi.h>
#include <furi_hal_resources.h>

#include <digital_signal/presets/nfc/legic_prime_signal.h>
#include <nfc/helpers/legic_prime_crc.h>
#include <nfc/helpers/legic_prng.h>
#include <nfc/protocols/legic_prime/legic_prime.h>

#define TAG "FuriHalLegicPrime"

// Prevent FDT timer from starting
#define FURI_HAL_NFC_LEGIC_PRIME_LISTENER_FDT_COMP_FC (INT32_MAX)

#define MAX_RX_ERRORS (100)
#define BITS_IN_BYTE (8)
#define MAX_ANSWER_BITS (12)

/*
 * These should be constatns, but in order to be able to adjust timings
 * in gdb, they have to be static volatile ints.
 *
 * Use the regexes below to turn them from one into the other.
 *
 * s/#define \(\w\+\) \((\d\+)\)\(.*\)/static volatile int32_t \1 = \2;\3/
 * s/static volatile int32_t \(\w\+\) = \((\d\+)\);/#define \1 \2/
 */
static volatile int32_t WRITE_ACK_DELAY = (3540); // ~3.6ms
static volatile int32_t BIT_T_1 = (40);
static volatile int32_t BIT_T_1_TOL = (10);
static volatile int32_t ACK_PAUSE_COMP = (142);
static volatile int32_t RX_FWT_READ_T =
    (21400); // ~1755us from the end of the poller's EOF bit to the start of the
             // first bit in the next poller frame. To be able to read more than
             // one byte at a time (aka one byte per setup phase), this timing
             // is crucial!
static volatile int32_t RX_FWT_WRITE_T =
    (50600); // ~4ms is what the pm3 does, so 4ms is what we do.
static volatile int32_t RX_FWT_ACK_T = (13500); // ~1155us
static volatile int32_t RX_LOOP_T = (95);
static volatile int32_t BIT_T_START_DELAY =
    (270); // 12bits * 100us loop time + 270us start delay == 1470us --> no more
           // than 295us left to do stuff like signal encoding or crc checking!

uint16_t rx_buf[1024];
static volatile bool bypass_crc_check = 0;

static volatile int16_t irq_bits = 0;

static LegicPrime_Signal *legic_prime_signal = NULL;

static uint16_t ones_before_start = 0;
static uint16_t ones_per_slot[MAX_ANSWER_BITS] = {0};

static crc_t legic_crc;
static uint32_t cmd = 0;
static uint8_t tag_type = 0;
static bool transparent_mode = 0;
static bool setup_successful = 0;

/*
 * This is the most important part of this whole thing!
 * DO NOT add any code to this isr, OR things WILL break!
 */
static void p_rx_cb(void *ctx) {
  UNUSED(ctx);
  irq_bits++;
}

// private functions

static int is_within_tolerance(int val, int ref, int tol) {
  return ((val >= (ref - tol)) && (val <= (ref + tol)));
}

static uint8_t calc_crc4(uint16_t cmd, uint8_t cmd_sz, uint8_t value) {
  crc_clear(&legic_crc);
  crc_update(&legic_crc, (value << cmd_sz) | cmd, 8 + cmd_sz);
  return crc_finish(&legic_crc);
}

static bool p_poller_crc_good(uint16_t data, size_t cmd_sz) {

  // split frame into data and crc
  uint8_t byte = data & 0xff;
  uint8_t crc = (data >> 8) & 0xf;

  // check received against calculated crc
  uint8_t calc_crc = calc_crc4(cmd, cmd_sz, byte);
  if (calc_crc != crc) {
    return false;
  } else {
    return true;
  }
}

static void p_enter_transparent(const FuriHalSpiBusHandle *handle) {
  if (transparent_mode)
    return;

  FURI_LOG_T(TAG, "enter transparent mode!");
  furi_hal_nfc_trx_reset();
  st25r3916_set_reg_bits(handle, ST25R3916_REG_OP_CONTROL,
                         ST25R3916_REG_OP_CONTROL_en);

  st25r3916_direct_cmd(handle, ST25R3916_CMD_TRANSPARENT_MODE);
  // Reconfigure gpio for Transparent mode
  furi_hal_spi_bus_handle_deinit(&furi_hal_spi_bus_handle_nfc);

  setup_successful = 0;
  transparent_mode = 1;
}

static void p_exit_transparent(const FuriHalSpiBusHandle *handle) {
  UNUSED(handle);

  if (!transparent_mode)
    return;

  FURI_LOG_T(TAG, "exit transparent mode!");
  // Configure gpio back to SPI and exit transparent mode
  furi_hal_gpio_write(&gpio_spi_r_mosi, false);
  furi_hal_spi_bus_handle_init(&furi_hal_spi_bus_handle_nfc);

  setup_successful = 0;
  transparent_mode = 0;
}

#if 1
static void p_reset_tag(const FuriHalSpiBusHandle *handle) {
  // switch off carrier and wait for 10ms to reset the tag
  st25r3916_direct_cmd(handle, ST25R3916_CMD_STOP);
  st25r3916_clear_reg_bits(handle, ST25R3916_REG_OP_CONTROL,
                           ST25R3916_REG_OP_CONTROL_en);

  furi_delay_ms(10);
}
#endif

FuriHalNfcError p_poller_tx(uint32_t tx_data, size_t tx_bits) {
  furi_check(legic_prime_signal);

  uint8_t bits_expected = 0;
  uint32_t rx_fwt = 0;
  uint32_t rx_bit_start_delay = 0;

  /*
   * Send the signal. Keep in mind that the time the encoding takes, varies with
   * the number of bits to be encoded.
   */
  legic_prime_signal_tx(legic_prime_signal,
                        tx_data ^ legic_prng_get_bits(tx_bits), tx_bits);

  /*
   * we can't rely on the rx command coming at a defined time, so we
   * have to start the actual receiving of the answer right here.
   */
  switch (tx_bits) {
  case 6:
    return FuriHalNfcErrorNone;

  case 7:
    rx_fwt = RX_FWT_ACK_T;
    rx_bit_start_delay = BIT_T_START_DELAY;
    bits_expected = 6;
    break;

  case 9:
  case 11:
    rx_fwt = RX_FWT_READ_T;
    rx_bit_start_delay = BIT_T_START_DELAY;
    bits_expected = MAX_ANSWER_BITS;
    break;

  case 21:
  case 23:
    // wait for a single ack bit after 3.6ms!
    rx_fwt = RX_FWT_WRITE_T;
    rx_bit_start_delay = WRITE_ACK_DELAY;
    bits_expected = 1;
    break;

  default:
    break;
  }

  // configure miso gpio as interrupt pin and add callback to fill buffer
  furi_hal_gpio_init(&gpio_spi_r_miso, GpioModeInterruptRise, GpioPullDown,
                     GpioSpeedVeryHigh);
  furi_hal_gpio_add_int_callback(&gpio_spi_r_miso, p_rx_cb, NULL);

  // Claim highest priority, so we don't get interrupted too often!
  // not sure if this is even necessary when the kernel is locked..
  FuriThreadPriority prio = furi_thread_get_current_priority();
  furi_thread_set_current_priority(FuriThreadPriorityHighest);

  /*
   * Start the frame wait timer.
   */
  furi_hal_nfc_timer_fwt_start(rx_fwt);

  // consider locking the kernel earlier and ditching the priority stuff
  furi_kernel_lock();

  uint8_t bit_idx = 0;
  furi_delay_us(rx_bit_start_delay);

  memset(ones_per_slot, 0, sizeof(uint16_t) * MAX_ANSWER_BITS);
  ones_before_start = irq_bits;
  irq_bits = 0;
  while (bits_expected--) {
    furi_delay_us(RX_LOOP_T);

    FURI_CRITICAL_ENTER();
    ones_per_slot[bit_idx] = irq_bits;
    bit_idx++;
    irq_bits = 0;
    FURI_CRITICAL_EXIT();
  }

  furi_kernel_unlock();

  // Set prio back to normal
  furi_thread_set_current_priority(prio);

  furi_hal_gpio_remove_int_callback(&gpio_spi_r_miso);
  furi_thread_flags_wait(FuriHalNfcEventInternalTypeTimerFwtExpired,
                         FuriFlagWaitAny, FuriWaitForever);

  /*
   * With trace logging enabled, only the first setup answer can be observed.
   */
  FURI_LOG_T(TAG, "bits before start: %d", ones_before_start);
  for (int i = 0; i < MAX_ANSWER_BITS; i++) {
    FURI_LOG_T(TAG, "bits in slot %02d: %4d", i, ones_per_slot[i]);
  }

  return FuriHalNfcErrorNone;
}

uint16_t p_poller_rx(size_t rx_bits) {

  uint16_t bits = 0x0000;
  uint8_t bit_idx = 0;
  uint16_t remaining_bits = 0;

  for (int i = 0; i < MAX_ANSWER_BITS; i++) {
    if (remaining_bits) {
      if (is_within_tolerance((remaining_bits + ones_per_slot[i]), BIT_T_1,
                              BIT_T_1_TOL)) {
        bits |= 1 << bit_idx++;
        FURI_LOG_T(TAG, "adding ONE in slot %d, bits: %d remaining bits: %d", i,
                   ones_per_slot[i], remaining_bits);
      } else {
        bits |= 0 << bit_idx++;
        FURI_LOG_T(TAG, "unused bits! slot %d, bits: %d", i, remaining_bits);
      }

      remaining_bits = 0;
    } else if (ones_per_slot[i] >= (BIT_T_1 - BIT_T_1_TOL)) {
      uint16_t condensed_bits = ones_per_slot[i] / BIT_T_1;
      remaining_bits = ones_per_slot[i] % BIT_T_1;
      if (is_within_tolerance(remaining_bits, BIT_T_1, BIT_T_1_TOL)) {
        remaining_bits = 0;
        condensed_bits++;
      }

      for (int j = 1; j <= condensed_bits; j++) {
        if (is_within_tolerance(ones_per_slot[i], BIT_T_1 * j, BIT_T_1_TOL)) {
        }
        bits |= 1 << bit_idx++;
        FURI_LOG_T(TAG, "adding ONE in slot %d, bits: %d remaining bits: %d", i,
                   ones_per_slot[i], remaining_bits);
      }
    } else {
      remaining_bits = ones_per_slot[i] % BIT_T_1;
      bits |= 0 << bit_idx++;
      FURI_LOG_T(TAG, "adding NULL in slot %d, bits: %d remaining bits: %d", i,
                 ones_per_slot[i], remaining_bits);
    }
  }

  return (rx_bits > 1) ? bits ^ legic_prng_get_bits(rx_bits) : bits;
}

static uint8_t p_poller_setup(uint32_t timeout) {
  uint16_t iv_frame = 0x55;
  uint16_t ack_frame = 0;
  uint16_t answer_frame = 0;

  setup_successful = 0;

  do {
    furi_delay_ms(5);
    legic_prng_init(0);

    p_poller_tx(iv_frame, 7);

    legic_prng_init(iv_frame);
    legic_prng_forward(2);

    answer_frame = p_poller_rx(6);
    legic_prng_forward(3);

    switch (answer_frame) {
    case 0x0D:
      ack_frame = 0x19;
      FURI_LOG_E(TAG, "wrong tag type recognized: 0x%04X", answer_frame);
      break;

    case 0x1D:
    case 0x3D:
      ack_frame = 0x39;
      break;

    default:
      FURI_LOG_E(TAG, "tag type not recognized: 0x%04X", answer_frame);
      answer_frame = 0;
      break;
    }
    if (ack_frame)
      break;
  } while (timeout--);

  if (!ack_frame)
    return 0;

  setup_successful = 1;
  p_poller_tx(ack_frame, 6);
  furi_delay_us(ACK_PAUSE_COMP);

  return (uint8_t)answer_frame & 0xff;
}

static bool p_read_byte(uint16_t addr, size_t cmd_sz) {

  legic_prng_forward(2);
  cmd = (addr << 1) | LegicPrimeCmdRead;
  p_poller_tx(cmd, cmd_sz);
  legic_prng_forward(2);
  rx_buf[addr] = p_poller_rx(MAX_ANSWER_BITS);
  legic_prng_forward(1);

  return (p_poller_crc_good(rx_buf[addr], cmd_sz) || bypass_crc_check);
}

static bool p_write_byte(uint16_t addr, uint8_t data, size_t addr_sz) {

  uint16_t cmd_sz = addr_sz + 1 + 8 + 4; // cmd_sz = addr_sz + cmd + data + crc

  cmd = (addr << 1) | LegicPrimeCmdWrite;

  uint8_t crc = calc_crc4(cmd, addr_sz + 1, data); // calculate crc
  cmd |= data << (addr_sz + 1);                    // append value
  cmd |= (crc & 0xF) << (addr_sz + 1 + 8);         // and crc

  legic_prng_forward(2);
  p_poller_tx(cmd, cmd_sz);
  legic_prng_forward(2);
  rx_buf[addr] = p_poller_rx(1);
  legic_prng_forward(35);

  return rx_buf[addr];
}

// public functions

// This function is used by poller and listener init functions
static FuriHalNfcError
furi_hal_nfc_legic_prime_common_init(const FuriHalSpiBusHandle *handle) {

  // lpf 300kHz, AM modulation
  st25r3916_write_reg(handle, ST25R3916_REG_RX_CONF1,
                      ST25R3916_REG_RX_CONF1_ch_sel_AM |
                          ST25R3916_REG_RX_CONF1_lp_300khz);

  // No AGC
  st25r3916_write_reg(handle, ST25R3916_REG_RX_CONF2, 0x00);

  // HF operation, full gain on AM and PM channels
  st25r3916_write_reg(handle, ST25R3916_REG_RX_CONF3, 0x00);
  // No gain reduction on AM and PM channels
  st25r3916_write_reg(handle, ST25R3916_REG_RX_CONF4, 0x00);

  // Correlator config (off)
  st25r3916_write_reg(handle, ST25R3916_REG_CORR_CONF1, 0x00);
  st25r3916_write_reg(handle, ST25R3916_REG_CORR_CONF2, 0x00);

  // TX driver full AM modulation (40%)
  st25r3916_write_reg(handle, ST25R3916_REG_TX_DRIVER,
                      ST25R3916_REG_TX_DRIVER_am_mod_40percent);

  // Stream mode config
  st25r3916_write_reg(handle, ST25R3916_REG_MODE,
                      ST25R3916_REG_MODE_om_subcarrier_stream |
                          ST25R3916_REG_MODE_tr_am_ook);

  st25r3916_write_reg(handle, ST25R3916_REG_STREAM_MODE,
                      ST25R3916_REG_STREAM_MODE_scf_sc212 |
                          ST25R3916_REG_STREAM_MODE_stx_212 |
                          ST25R3916_REG_STREAM_MODE_scp_1pulse);

  st25r3916_write_reg(handle, ST25R3916_REG_AUX,
                      ST25R3916_REG_AUX_dis_corr_correlator);
  return FuriHalNfcErrorNone;
}

static FuriHalNfcError
furi_hal_nfc_legic_prime_poller_init(const FuriHalSpiBusHandle *handle) {

  furi_check(legic_prime_signal == NULL);
  legic_prime_signal = legic_prime_signal_alloc(&gpio_spi_r_mosi);

  st25r3916_write_reg(handle, ST25R3916_REG_OP_CONTROL,
                      ST25R3916_REG_OP_CONTROL_en |
                          ST25R3916_REG_OP_CONTROL_rx_en |
                          ST25R3916_REG_OP_CONTROL_en_fd_auto_efd);

  return furi_hal_nfc_legic_prime_common_init(handle);
}

static FuriHalNfcError
furi_hal_nfc_legic_prime_poller_deinit(const FuriHalSpiBusHandle *handle) {

  if (legic_prime_signal) {
    legic_prime_signal_free(legic_prime_signal);
    legic_prime_signal = NULL;
  }

  // This is the same as in the init cmd, which seems wrong.
  st25r3916_write_reg(handle, ST25R3916_REG_OP_CONTROL,
                      ST25R3916_REG_OP_CONTROL_en |
                          ST25R3916_REG_OP_CONTROL_rx_en |
                          ST25R3916_REG_OP_CONTROL_en_fd_auto_efd);

  return FuriHalNfcErrorNone;
}

static FuriHalNfcError
furi_hal_nfc_legic_prime_listener_init(const FuriHalSpiBusHandle *handle) {
  furi_check(legic_prime_signal == NULL);
  legic_prime_signal = legic_prime_signal_alloc(&gpio_spi_r_mosi);

#if 1
  UNUSED(handle);
#else
  st25r3916_write_reg(handle, ST25R3916_REG_OP_CONTROL,
                      ST25R3916_REG_OP_CONTROL_en |
                          ST25R3916_REG_OP_CONTROL_rx_en |
                          ST25R3916_REG_OP_CONTROL_en_fd_auto_efd);
  st25r3916_write_reg(handle, ST25R3916_REG_MODE,
                      ST25R3916_REG_MODE_targ_targ | ST25R3916_REG_MODE_om0);
  st25r3916_write_reg(handle, ST25R3916_REG_PASSIVE_TARGET,
                      ST25R3916_REG_PASSIVE_TARGET_fdel_2 |
                          ST25R3916_REG_PASSIVE_TARGET_fdel_0 |
                          ST25R3916_REG_PASSIVE_TARGET_d_ac_ap2p |
                          ST25R3916_REG_PASSIVE_TARGET_d_212_424_1r);

  st25r3916_write_reg(handle, ST25R3916_REG_MASK_RX_TIMER, 0x02);

  st25r3916_direct_cmd(handle, ST25R3916_CMD_STOP);
  uint32_t interrupts = (ST25R3916_IRQ_MASK_FWL | ST25R3916_IRQ_MASK_TXE |
                         ST25R3916_IRQ_MASK_RXS | ST25R3916_IRQ_MASK_RXE |
                         ST25R3916_IRQ_MASK_PAR | ST25R3916_IRQ_MASK_CRC |
                         ST25R3916_IRQ_MASK_ERR1 | ST25R3916_IRQ_MASK_ERR2 |
                         ST25R3916_IRQ_MASK_NRE | ST25R3916_IRQ_MASK_EON |
                         ST25R3916_IRQ_MASK_EOF | ST25R3916_IRQ_MASK_WU_A_X |
                         ST25R3916_IRQ_MASK_WU_A);
  // Clear interrupts
  st25r3916_get_irq(handle);
  // Enable interrupts
  st25r3916_mask_irq(handle, ~interrupts);
  // Enable auto collision resolution
  st25r3916_clear_reg_bits(handle, ST25R3916_REG_PASSIVE_TARGET,
                           ST25R3916_REG_PASSIVE_TARGET_d_106_ac_a);
  st25r3916_direct_cmd(handle, ST25R3916_CMD_GOTO_SENSE);
#endif

  return furi_hal_nfc_legic_prime_common_init(handle);
}

static FuriHalNfcError
furi_hal_nfc_legic_prime_listener_deinit(const FuriHalSpiBusHandle *handle) {
  UNUSED(handle);

  if (legic_prime_signal) {
    legic_prime_signal_free(legic_prime_signal);
    legic_prime_signal = NULL;
  }

  return FuriHalNfcErrorNone;
}

static FuriHalNfcEvent
furi_hal_nfc_legic_prime_listener_wait_event(uint32_t timeout_ms) {
  FuriHalNfcEvent event = furi_hal_nfc_wait_event_common(timeout_ms);
  const FuriHalSpiBusHandle *handle = &furi_hal_spi_bus_handle_nfc;

  if (event & FuriHalNfcEventListenerActive) {
    st25r3916_set_reg_bits(handle, ST25R3916_REG_PASSIVE_TARGET,
                           ST25R3916_REG_PASSIVE_TARGET_d_106_ac_a);
  }

  return event;
}

FuriHalNfcError
furi_hal_nfc_legic_prime_listener_tx(const FuriHalSpiBusHandle *handle,
                                     const uint8_t *tx_data, size_t tx_bits) {
  FuriHalNfcError error = FuriHalNfcErrorNone;

#if 1
  UNUSED(handle);
  UNUSED(tx_data);
  UNUSED(tx_bits);
#else
  do {
    error = furi_hal_nfc_common_fifo_tx(handle, tx_data, tx_bits);
    if (error != FuriHalNfcErrorNone)
      break;

    bool tx_end = furi_hal_nfc_event_wait_for_specific_irq(
        handle, ST25R3916_IRQ_MASK_TXE, 10);
    if (!tx_end) {
      error = FuriHalNfcErrorCommunicationTimeout;
      break;
    }

  } while (false);
#endif

  return error;
}

FuriHalNfcError
furi_hal_nfc_legic_prime_listener_sleep(const FuriHalSpiBusHandle *handle) {
#if 1
  UNUSED(handle);
#else
  // Enable auto collision resolution
  st25r3916_clear_reg_bits(handle, ST25R3916_REG_PASSIVE_TARGET,
                           ST25R3916_REG_PASSIVE_TARGET_d_106_ac_a);
  st25r3916_direct_cmd(handle, ST25R3916_CMD_STOP);
  st25r3916_direct_cmd(handle, ST25R3916_CMD_GOTO_SLEEP);
#endif

  return FuriHalNfcErrorNone;
}

FuriHalNfcError
furi_hal_nfc_legic_prime_listener_idle(const FuriHalSpiBusHandle *handle) {
#if 1
  UNUSED(handle);
#else
  // Enable auto collision resolution
  st25r3916_clear_reg_bits(handle, ST25R3916_REG_PASSIVE_TARGET,
                           ST25R3916_REG_PASSIVE_TARGET_d_106_ac_a);
  st25r3916_direct_cmd(handle, ST25R3916_CMD_STOP);
  st25r3916_direct_cmd(handle, ST25R3916_CMD_GOTO_SENSE);
#endif

  return FuriHalNfcErrorNone;
}

FuriHalNfcError
furi_hal_nfc_legic_prime_poller_tx(const FuriHalSpiBusHandle *handle,
                                   const uint8_t *tx_data, size_t tx_bits) {

  furi_check(tx_data);
  furi_check(legic_prime_signal);
  UNUSED(tx_bits);

  LegicPrimePollerTrxData *trx_data = (LegicPrimePollerTrxData *)tx_data;
  FuriHalNfcError error = FuriHalNfcErrorNone;

  // init crc calculator
  crc_init(&legic_crc, 4, 0x19 >> 1, 0x05, 0);

  // TODO: put this on the heap when everything runs!
  memset(rx_buf, 0, 1024 * sizeof(uint16_t));

  p_enter_transparent(handle);

  do {
    tag_type = p_poller_setup(MAX_RX_ERRORS);

    // If no card is detected, we should return some kind of error!
    if (!tag_type) {
      error = FuriHalNfcErrorNone;
      break;
    }

    // If this is a tag detect command, return immediately after setup!
    if (!trx_data->tag.tagtype) {
      error = FuriHalNfcErrorNone;
      break;
    }

    uint8_t cmd = trx_data->cmd;
    size_t tx_bytes = trx_data->num_addrs;
    uint32_t error_cnt = 0;
    bool success = false;

    for (size_t i = 0; i < tx_bytes; i++) {
      if (cmd == LegicPrimeCmdRead) {
        success = p_read_byte(trx_data->addrs[i], trx_data->tag.cmdsize);
      } else if (cmd == LegicPrimeCmdWrite) {
        success = p_write_byte(trx_data->addrs[i], trx_data->write_data[i],
                               trx_data->tag.addrsize);
      }

      if (success) {
        error_cnt = 0;
        trx_data->bytes_processed++;
      } else {
        i--;
        error_cnt++;
        trx_data->total_errors++;
      }
      if (error_cnt > MAX_RX_ERRORS) {
        error = FuriHalNfcErrorIncompleteFrame;
        break;
      }
    }
  } while (false);

  p_exit_transparent(handle);
  p_reset_tag(handle);
  return error;
}

FuriHalNfcError
furi_hal_nfc_legic_prime_poller_rx(const FuriHalSpiBusHandle *handle,
                                   uint8_t *rx_data, size_t rx_data_size,
                                   size_t *rx_bits) {
  UNUSED(handle);
  UNUSED(rx_data_size);

  LegicPrimePollerTrxData *trx_data = (LegicPrimePollerTrxData *)rx_data;

  FuriHalNfcError error = FuriHalNfcErrorNone;

  do {
    // If this is the answer to a tag detect command, return immediately.
    if (!trx_data->tag.tagtype) {
      trx_data->tag.tagtype = tag_type;
      FURI_LOG_D(TAG, "responding to activate cmd");
      break;
    }

    for (size_t i = 0; i < trx_data->num_addrs; i++) {
      trx_data->response_data[i] = rx_buf[i] & 0xff;
      *rx_bits = i * BITS_IN_BYTE;
    }

  } while (false);

  return error;
}

const FuriHalNfcTechBase furi_hal_nfc_legic_prime = {
    .poller =
        {
            .compensation =
                {
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
