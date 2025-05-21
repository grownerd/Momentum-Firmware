#include "furi_hal_nfc.h"
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
#define BIT_DEBUG_BUFFER_SIZE (64)

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
static volatile int32_t BIT_T_1 = (80);
static volatile int32_t BIT_T_1_TOL = (20);
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

static volatile int32_t LISTENER_TX_DELAY_T = (760);       // ~330us
static volatile int32_t LISTENER_TX_ACK_DELAY_T = (46000); // ~3.5ms
static volatile int32_t LISTENER_ACK_DELAY_US = (3400);    // ~3.5ms
static volatile int32_t LISTENER_LOOP_T = (1);
static volatile int32_t LISTENER_ONE_T = (32);
static volatile int32_t LISTENER_ZERO_T = (16);
static volatile int32_t LISTENER_PAUSE_T = (5);
static volatile int32_t LISTENER_BIT_T = (41); // one_t + pause_t
static volatile int32_t LISTENER_BIT_TOL_T = (4);
static volatile int32_t LISTENER_TIMEOUT_T =
    (41); // shorten this no more than  one_t + pause_t
static volatile int32_t LISTENER_RX_FRAME_TIMEOUT_T = (4000);
static volatile bool bit_debugging_enabled = false;
static volatile uint32_t debug_bit_idx = 0;

static volatile int32_t rng_steps_rx = (3);
static volatile int32_t rng_steps_tx = (2);
static volatile int32_t rng_steps_setup_back = (1);
static volatile int32_t rng_steps_write_ack = (35);

static volatile bool bypass_crc_check = 0;

LegicPrimeData *sim_tag = NULL;
uint16_t *rx_buf = NULL;
static LegicPrime_Signal *legic_prime_signal = NULL;

static uint16_t *ones_per_slot = NULL;
static uint16_t ones_before_start = 0;
static volatile int16_t irq_bits = 0;

static crc_t legic_crc;
static uint32_t cmd = 0;
static uint8_t tag_type = 0;
static bool transparent_mode = 0;

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
                        tx_data ^ legic_prng_get_bits(tx_bits), tx_bits, true);

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
  /*
   * FIXME: @flipper gpio hal:
   * This used to be GpioModeInterruptRise only, but after using listener rx,
   * which uses GpioModeInterruptRiseFall, the poller isr oddly received twice
   * as many irqs, which indicates that the furi_hal_gpio_init() function does
   * not work properly and does not reset the irq bits!
   * */
  furi_hal_gpio_init(&gpio_spi_r_miso, GpioModeInterruptRiseFall, GpioPullDown,
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
  furi_hal_nfc_timer_fwt_stop();

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
      FURI_LOG_T(TAG, "wrong tag type recognized: 0x%04X", answer_frame);
      break;

    case 0x1D:
    case 0x3D:
      ack_frame = 0x39;
      break;

    default:
      FURI_LOG_T(TAG, "tag type not recognized: 0x%04X", answer_frame);
      answer_frame = 0;
      break;
    }
    if (ack_frame)
      break;
  } while (timeout--);

  if (!ack_frame)
    return 0;

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

/*
 * Private Listener functions
 */
static int32_t p_listener_rx(uint8_t *len, int32_t *raw) {
  furi_check(legic_prime_signal);

  int32_t raw_frame = 0;
  int32_t xored_frame = 0;
  int prng_steps = 0;

  // configure miso gpio as interrupt pin and add callback to fill buffer
  furi_hal_gpio_init(&gpio_spi_r_miso, GpioModeInterruptRiseFall, GpioPullDown,
                     GpioSpeedVeryHigh);
  furi_hal_gpio_add_int_callback(&gpio_spi_r_miso, p_rx_cb, NULL);

  // Claim highest priority, so we don't get interrupted too often!
  // not sure if this is even necessary when the kernel is locked..
  // FuriThreadPriority prio = furi_thread_get_current_priority();
  // furi_thread_set_current_priority(FuriThreadPriorityHighest);

  // consider locking the kernel earlier and ditching the priority stuff
  furi_kernel_lock();

  int bit_errors[BIT_DEBUG_BUFFER_SIZE];
  int bit_debug[BIT_DEBUG_BUFFER_SIZE];
  int bit_err_idx = 0;
  int bit_dbg_idx = 0;
  if (bit_debugging_enabled) {
    memset(bit_errors, 0, sizeof(int) * BIT_DEBUG_BUFFER_SIZE);
    memset(bit_debug, 0, sizeof(int) * BIT_DEBUG_BUFFER_SIZE);
  }
  uint32_t loop_timeout = LISTENER_RX_FRAME_TIMEOUT_T;
  uint16_t loop_idx = 0;
  uint16_t bit_idx = 0;
  uint16_t last_bit_idx = 0;
  static uint16_t diff = 0;
  bool pause_detected = false;
  irq_bits = 0;

  // Everything from here on out is either a bit or an error!
  while (true) {
    furi_delay_us(LISTENER_LOOP_T);

    if (irq_bits) {
      diff = loop_idx - last_bit_idx;
      if (pause_detected) {
        if (is_within_tolerance(diff, LISTENER_ONE_T, LISTENER_BIT_TOL_T)) {
          // This is a ONE
          raw_frame |= 1 << bit_idx++;
        } else if (is_within_tolerance(diff, LISTENER_ZERO_T,
                                       LISTENER_BIT_TOL_T)) {
          // This is a ZERO
          raw_frame |= 0 << bit_idx++;
        } else if (diff > LISTENER_TIMEOUT_T) {
          prng_steps += diff;
        } else {
          bit_errors[bit_err_idx++] = diff;
        }
        pause_detected = false;
      } else if (is_within_tolerance(diff, LISTENER_PAUSE_T,
                                     LISTENER_BIT_TOL_T)) {
        pause_detected = true;
      } else if (bit_idx) {
        bit_errors[bit_err_idx++] = diff;
      }

      last_bit_idx = loop_idx;
      irq_bits = 0;
      bit_debug[bit_dbg_idx++] = diff;
    }
    loop_idx++;
    if (!loop_timeout-- && !bit_idx) {
      FURI_LOG_T(TAG, "Listener RX Timeout!");
      break;
    }

    if (bit_err_idx >= BIT_DEBUG_BUFFER_SIZE ||
        bit_dbg_idx >= BIT_DEBUG_BUFFER_SIZE)
      break;

    // Break if no more bits are received!
    // This will break if the loop has run for longer than it takes for a '1'
    // bit to be received, but only after at least one bit has been received.
    if ((bit_idx) && ((loop_idx - last_bit_idx) > LISTENER_TIMEOUT_T)) {
      break;
    }
  }

  // start the tx fwt as soon as possible!
  uint32_t tx_delay =
      bit_idx > 14 ? LISTENER_TX_ACK_DELAY_T : LISTENER_TX_DELAY_T;
  furi_hal_nfc_timer_fwt_start(tx_delay);

  // Unlock kernel, set prio back to normal, disable irq
  furi_kernel_unlock();
  // furi_thread_set_current_priority(prio);
  furi_hal_gpio_remove_int_callback(&gpio_spi_r_miso);

  if (bit_err_idx) {
    FURI_LOG_T(TAG, "raw_frame: 0x%08lX bit errors: %d", raw_frame,
               bit_err_idx);
    for (int i = 0; i < BIT_DEBUG_BUFFER_SIZE; i++) {
      FURI_LOG_T(TAG, "bit error: idx: %d, diff: %d", i, bit_errors[i]);
    }
  }
  if (bit_debugging_enabled && bit_idx == debug_bit_idx) {
    FURI_LOG_D(TAG,
               "raw_frame: 0x%08lX, xored_frame: 0%08lX, "
               "len: %d rng steps: %ld",
               raw_frame, xored_frame, bit_idx, legic_prng_get_count());
    for (int i = 0; i < BIT_DEBUG_BUFFER_SIZE; i++) {
      FURI_LOG_D(TAG, "bit idx: %d, diff: %d", i, bit_debug[i]);
    }
  }
  if (bit_idx && bit_idx != 1 && bit_idx != 6 && bit_idx != 7 && bit_idx != 9 &&
      bit_idx != 11 && bit_idx != 21 && bit_idx != 23) {
    FURI_LOG_E(TAG,
               "wrong bit length! raw_frame: 0x%08lX, xored_frame: 0%08lX, "
               "len: %d rng steps: %ld",
               raw_frame, xored_frame, bit_idx, legic_prng_get_count());
    for (int i = 0; i < BIT_DEBUG_BUFFER_SIZE; i++) {
      FURI_LOG_E(TAG, "bit idx: %d, diff: %d", i, bit_debug[i]);
    }
    memset(bit_errors, 0, sizeof(int) * BIT_DEBUG_BUFFER_SIZE);
  }

  legic_prng_forward(rng_steps_rx);
  xored_frame = raw_frame ^ legic_prng_get_bits(bit_idx);

  *raw = raw_frame;
  *len = bit_idx;
  return xored_frame;
}

static void p_listener_tx(uint32_t tx_data, size_t tx_bits) {

  // Wait for the fwt to expire, which has been set in p_listener_rx to ~330us
  // after the last bit has been received.
  furi_thread_flags_wait(FuriHalNfcEventInternalTypeTimerFwtExpired,
                         FuriFlagWaitAny, FuriWaitForever);
  furi_hal_nfc_timer_fwt_stop();

  legic_prng_forward(rng_steps_tx);
  legic_prime_signal_tx(legic_prime_signal,
                        tx_data ^ legic_prng_get_bits(tx_bits), tx_bits, false);
}

static void p_listener_tx_ack(void) {

  // Wait for the fwt to expire, which has been set in p_listener_rx to ~330us
  // after the last bit has been received.
  furi_thread_flags_wait(FuriHalNfcEventInternalTypeTimerFwtExpired,
                         FuriFlagWaitAny, FuriWaitForever);
  furi_hal_nfc_timer_fwt_stop();
  // furi_delay_us(LISTENER_ACK_DELAY_US);

  legic_prng_forward(rng_steps_write_ack);
  legic_prime_signal_tx(legic_prime_signal, 1, 1, false);
  legic_prng_forward(1);
}

// Setup reader to card connection
//
// The setup consists of a three way handshake:
//  - Receive initialisation vector 7 bits
//  - Transmit card type 6 bits
//  - Receive Acknowledge 6 bits
static FuriHalNfcError p_listener_setup_phase(LegicPrimeTag *p_card) {
  uint8_t len = 0;

  // reset prng
  legic_prng_init(0);

  // wait for iv
  int32_t raw = 0;
  int32_t iv = p_listener_rx(&len, &raw);
  if ((len != 7) || (iv < 0)) {
    FURI_LOG_E(TAG,
               "Listener setup phase: no/wrong IV received! iv: 0x%02lX, raw: "
               "0x%02lX, len: %d",
               iv, raw, len);
    return FuriHalNfcErrorCommunicationTimeout;
  }

  // configure prng
  legic_prng_init(iv);

  // reply with card type
  switch (p_card->tagtype) {
  case LegicPrimeTagTypeMim22:
    p_listener_tx(0x0D, 6);
    break;
  case LegicPrimeTagTypeMim256:
    p_listener_tx(0x1D, 6);
    break;
  case LegicPrimeTagTypeMim1024:
    p_listener_tx(0x3D, 6);
    break;
  }

  // wait for ack
  int32_t ack = p_listener_rx(&len, &raw);
  // stop the fwt, because the next rx command will try to start it, which wont
  // succeed, if already started/expired!
  furi_hal_nfc_timer_fwt_stop();
  if ((len != 6) || (ack < 0)) {
    FURI_LOG_E(
        TAG,
        "Listener setup phase: no/wrong ACK received! ack: 0x%02lX, len: %d",
        ack, len);
    return FuriHalNfcErrorCommunicationTimeout;
  }

  // validate data
  switch (p_card->tagtype) {
  case LegicPrimeTagTypeMim22:
    if (ack != 0x19) {
      FURI_LOG_E(
          TAG,
          "Listener setup phase: ACK mismatch! ack: 0x%02lX, prng count %ld",
          ack, legic_prng_get_count());
      return FuriHalNfcErrorCommunication;
    }
    break;
  case LegicPrimeTagTypeMim256:
    if (ack != 0x39) {
      FURI_LOG_E(
          TAG,
          "Listener setup phase: ACK mismatch! ack: 0x%02lX, prng count %ld",
          ack, legic_prng_get_count());
      return FuriHalNfcErrorCommunication;
    }
    break;
  case LegicPrimeTagTypeMim1024:
    if (ack != 0x39) {
      FURI_LOG_E(
          TAG,
          "Listener setup phase: ACK mismatch! ack: 0x%02lX, prng count %ld",
          ack, legic_prng_get_count());
      return FuriHalNfcErrorCommunication;
    }
    break;
  }
  legic_prng_backup(rng_steps_setup_back);
  return FuriHalNfcErrorNone;
}

static FuriHalNfcError p_listener_connected_phase(LegicPrimeTag *p_card,
                                                  uint16_t *bytes_read,
                                                  uint16_t *bytes_written) {
  uint8_t len = 0;
  int32_t raw = 0;

  // wait for command
  int32_t cmd = p_listener_rx(&len, &raw);
  if (cmd < 0) {
    return FuriHalNfcErrorCommunicationTimeout;
  }

  // check if command is LEGIC_READ
  if (len == p_card->cmdsize) {
    // prepare data
    uint8_t byte = sim_tag->data[cmd >> 1];
    uint8_t crc = calc_crc4(cmd, p_card->cmdsize, byte);

    // transmit data
    p_listener_tx((crc << 8) | byte, 12);
    (*bytes_read)++;
    return FuriHalNfcErrorNone;
  }

  // check if command is LEGIC_WRITE
  if (len == p_card->cmdsize + 8 + 4) {
    // decode data
    uint16_t mask = (1 << p_card->addrsize) - 1;
    uint16_t addr = (cmd >> 1) & mask;
    uint8_t byte = (cmd >> p_card->cmdsize) & 0xff;
    uint8_t crc = (cmd >> (p_card->cmdsize + 8)) & 0xf;

    // check received against calculated crc
    uint8_t calc_crc = calc_crc4(addr << 1, p_card->cmdsize, byte);
    if (calc_crc != crc) {
      FURI_LOG_E(
          TAG,
          "!!! crc mismatch: %x != %x @ addr: %02x, data: %02x, addr_sz: "
          "%d, cmd_sz: %d !!!",
          calc_crc, crc, addr, byte, p_card->addrsize, p_card->cmdsize);
      return FuriHalNfcErrorCommunication;
    }

    FURI_LOG_T(TAG, "Write successful @ addr: %02x, data: %02x", addr, byte);
    // store data
    sim_tag->data[addr] = byte;

    // transmit ack
    p_listener_tx_ack();
    (*bytes_written)++;
    return FuriHalNfcErrorNone;
  }
  if (len) {
    FURI_LOG_E(TAG,
               "Listener data error! rx data: 0x%08lX, raw: 0x%08lX, bits: %d "
               "rng steps: %ld",
               cmd, raw, len, legic_prng_get_count());
  }

  return FuriHalNfcErrorCommunicationTimeout;
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

  // TX driver full AM modulation (40%) This may be only for listener mode.
  // Also, this doesn't seem to make much of a difference.
  st25r3916_write_reg(handle, ST25R3916_REG_TX_DRIVER,
                      ST25R3916_REG_TX_DRIVER_am_mod_40percent);

  // Stream mode config.
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

  // Operation mode config.
  // mode.
  st25r3916_write_reg(handle, ST25R3916_REG_MODE,
                      ST25R3916_REG_MODE_om_subcarrier_stream |
                          ST25R3916_REG_MODE_tr_am_ook);

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

  st25r3916_write_reg(handle, ST25R3916_REG_MODE,
                      ST25R3916_REG_MODE_targ_targ |
                          ST25R3916_REG_MODE_om_subcarrier_stream |
                          ST25R3916_REG_MODE_tr_am_ook);
  // This is the same in inits and de-inits all over the place.
  st25r3916_write_reg(handle, ST25R3916_REG_OP_CONTROL,
                      ST25R3916_REG_OP_CONTROL_en |
                          ST25R3916_REG_OP_CONTROL_rx_en |
                          ST25R3916_REG_OP_CONTROL_en_fd_auto_efd);

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
  st25r3916_direct_cmd(handle, ST25R3916_CMD_GOTO_SENSE);

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

  FURI_LOG_D(TAG, "listener event: %x", event);

  if (event & FuriHalNfcEventFieldOn) {
    p_enter_transparent(handle);
    do {
      FuriHalNfcError error = p_listener_setup_phase(&sim_tag->tag);
      if (error != FuriHalNfcErrorNone) {
        FURI_LOG_E(TAG, "Listener setup phase failed: %d", error);
        event = FuriHalNfcEventTimeout;
        break;
      }
      uint16_t bytes_written = 0;
      uint16_t bytes_read = 0;
      while (true) {
        error = p_listener_connected_phase(&sim_tag->tag, &bytes_read,
                                           &bytes_written);
        if (error != FuriHalNfcErrorNone) {
          FURI_LOG_D(TAG, "Listener connected phase returned %d", error);
          event = FuriHalNfcEventTimeout;
          break;
        }
      }
      FURI_LOG_I(TAG, "bytes read/written: %d/%d", bytes_read, bytes_written);

    } while (false);
    p_exit_transparent(handle);
  }

  return event;
}

FuriHalNfcError
furi_hal_nfc_legic_prime_listener_tx(const FuriHalSpiBusHandle *handle,
                                     const uint8_t *tx_data, size_t tx_bits) {
  UNUSED(handle);
  UNUSED(tx_bits);
  furi_check(sim_tag == NULL);

  FuriHalNfcError error = FuriHalNfcErrorNone;

  sim_tag = malloc(sizeof(LegicPrimeData));
  memset(sim_tag, 0, sizeof(LegicPrimeData));

  LegicPrimeListenerTrxData *trx_data = (LegicPrimeListenerTrxData *)tx_data;
  memcpy(sim_tag, &trx_data->data, sizeof(LegicPrimeData));

  return error;
}

FuriHalNfcError
furi_hal_nfc_legic_prime_listener_rx(const FuriHalSpiBusHandle *handle,
                                     uint8_t *rx_data, size_t rx_data_size,
                                     size_t *rx_bits) {
  UNUSED(handle);
  UNUSED(rx_data_size);
  UNUSED(rx_bits);
  furi_check(sim_tag != NULL);

  LegicPrimeListenerTrxData *trx_data = (LegicPrimeListenerTrxData *)rx_data;
  memcpy(&trx_data->data, sim_tag, sizeof(LegicPrimeData));

  if (sim_tag) {
    free(sim_tag);
    sim_tag = NULL;
  }
  return FuriHalNfcErrorNone;
}

FuriHalNfcError
furi_hal_nfc_legic_prime_listener_sleep(const FuriHalSpiBusHandle *handle) {
  UNUSED(handle);
  return FuriHalNfcErrorNone;
}

FuriHalNfcError
furi_hal_nfc_legic_prime_listener_idle(const FuriHalSpiBusHandle *handle) {
  UNUSED(handle);
  return FuriHalNfcErrorNone;
}

FuriHalNfcError
furi_hal_nfc_legic_prime_poller_tx(const FuriHalSpiBusHandle *handle,
                                   const uint8_t *tx_data, size_t tx_bits) {

  furi_check(tx_data);
  furi_check(legic_prime_signal);
  UNUSED(tx_bits);
  furi_check(rx_buf == NULL);

  LegicPrimePollerTrxData *trx_data = (LegicPrimePollerTrxData *)tx_data;
  FuriHalNfcError error = FuriHalNfcErrorNone;

  // init crc calculator
  crc_init(&legic_crc, 4, 0x19 >> 1, 0x05, 0);

  rx_buf = malloc(sizeof(uint16_t) * 1024);
  memset(rx_buf, 0, 1024 * sizeof(uint16_t));

  ones_per_slot = malloc(sizeof(uint16_t) * MAX_ANSWER_BITS);
  memset(ones_per_slot, 0, MAX_ANSWER_BITS * sizeof(uint16_t));
  ones_before_start = 0;
  irq_bits = 0;

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

        if (rx_buf) {
          free(rx_buf);
          rx_buf = NULL;
          free(ones_per_slot);
          ones_per_slot = NULL;
        }
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

  if (rx_buf) {
    free(rx_buf);
    rx_buf = NULL;
    free(ones_per_slot);
    ones_per_slot = NULL;
  }

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
            .rx = furi_hal_nfc_legic_prime_listener_rx,
            .sleep = furi_hal_nfc_legic_prime_listener_sleep,
            .idle = furi_hal_nfc_legic_prime_listener_idle,
        },
};
