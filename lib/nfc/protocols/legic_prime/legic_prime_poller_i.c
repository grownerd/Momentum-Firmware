#include "legic_prime_poller_i.h"
#include "protocols/legic_prime/legic_prime_poller.h"
#include <nfc/helpers/legic_prime_crc.h>

#define TAG "LegicPrimePoller"

static crc_t legic_crc;

static uint8_t calc_crc4(uint16_t cmd, uint8_t cmd_sz, uint8_t value) {
  crc_clear(&legic_crc);
  crc_update(&legic_crc, (value << cmd_sz) | cmd, 8 + cmd_sz);
  return crc_finish(&legic_crc);
}

static LegicPrimeError legic_prime_poller_process_error(NfcError error) {
  switch (error) {
  case NfcErrorNone:
    return LegicPrimeErrorNone;
  case NfcErrorTimeout:
    return LegicPrimeErrorTimeout;
  default:
    return LegicPrimeErrorNotPresent;
  }
}

static LegicPrimeError legic_prime_init_tag(LegicPrimeTagType cardtype,
                                            LegicPrimeTag *p_card) {
  p_card->tagtype = cardtype;

  switch (p_card->tagtype) {
  case 0x0d:
    p_card->cmdsize = 6;
    p_card->addrsize = 5;
    p_card->cardsize = 22;
    break;
  case 0x1d:
    p_card->cmdsize = 9;
    p_card->addrsize = 8;
    p_card->cardsize = 256;
    break;
  case 0x3d:
    p_card->cmdsize = 11;
    p_card->addrsize = 10;
    p_card->cardsize = 1024;
    break;
  default:
    p_card->cmdsize = 0;
    p_card->addrsize = 0;
    p_card->cardsize = 0;
    return LegicPrimeErrorNotPresent;
  }
  return LegicPrimeErrorNone;
}

LegicPrimeError
legic_prime_poller_frame_exchange(const LegicPrimePoller *instance,
                                  const BitBuffer *tx_buffer,
                                  BitBuffer *rx_buffer, uint32_t fwt) {
  furi_assert(instance);

  const size_t tx_bytes = bit_buffer_get_size_bytes(tx_buffer);
  furi_assert(tx_bytes <= bit_buffer_get_capacity_bytes(instance->tx_buffer));

  LegicPrimeError ret = LegicPrimeErrorNone;

  do {
    NfcError error = nfc_legic_prime_poller_trx(
        instance->nfc, instance->tx_buffer, instance->rx_buffer, fwt);
    // nfc_poller_trx(instance->nfc, instance->tx_buffer, instance->rx_buffer,
    // fwt);
    if (error != NfcErrorNone) {
      ret = legic_prime_poller_process_error(error);
      break;
    }

    bit_buffer_copy(rx_buffer, instance->rx_buffer);
#if 0
        if(!legic_prime_crc_check(instance->rx_buffer)) {
            ret = LegicPrimeErrorWrongCrc;
            break;
        }

        legic_prime_crc_trim(rx_buffer);
#endif
  } while (false);

  return ret;
}

LegicPrimeError
legic_prime_poller_polling(LegicPrimePoller *instance,
                           const LegicPrimePollerPollingCommand *cmd,
                           LegicPrimePollerPollingResponse *resp) {
  furi_assert(instance);
  furi_assert(cmd);
  furi_assert(resp);

  LegicPrimeError error = LegicPrimeErrorNone;

  do {
    bit_buffer_reset(instance->tx_buffer);
    bit_buffer_reset(instance->rx_buffer);

    for (int i = 0; i < cmd->num_bits; i++) {
      uint8_t bit = (cmd->data >> i) & 0x01;
      bit_buffer_append_bit(instance->tx_buffer, bit);
    }

    for (int i = 0; i < resp->num_bits; i++) {
      uint8_t bit = (resp->data >> i) & 0x01;
      bit_buffer_append_bit(instance->rx_buffer, bit);
    }

    error = legic_prime_poller_frame_exchange(instance, instance->tx_buffer,
                                              instance->rx_buffer,
                                              LEGIC_PRIME_POLLER_POLLING_FWT);

    if (error != LegicPrimeErrorNone)
      break;
    bit_buffer_write_bytes_mid(instance->rx_buffer, &(resp->data), 0, 1);

  } while (false);

  return error;
}

LegicPrimeError legic_prime_poller_read_byte(
    LegicPrimePoller *instance, uint16_t addr,
    LegicPrimePollerReadCommandResponse **const response_ptr) {

  furi_assert(instance);
  furi_assert(addr);
  furi_assert(response_ptr);

  // Prepare bit buffer
  bit_buffer_reset(instance->rx_buffer);
  bit_buffer_reset(instance->tx_buffer);

  uint16_t tx_frame = addr << 1 | LegicPrimeCmdRead;

  uint16_t cmdsize = instance->data->tag.cmdsize;

  for (int i = 0; i < cmdsize; i++) {
    uint8_t bit = (tx_frame >> i) & 0x01;
    bit_buffer_append_bit(instance->tx_buffer, bit);
  }

  for (int i = 0; i < 8; i++) {
    uint8_t bit = 0;
    bit_buffer_append_bit(instance->rx_buffer, bit);
  }

  LegicPrimeError error = legic_prime_poller_frame_exchange(
      instance, instance->tx_buffer, instance->rx_buffer,
      LEGIC_PRIME_POLLER_POLLING_FWT);
  if (error == LegicPrimeErrorNone) {
    *response_ptr = (LegicPrimePollerReadCommandResponse *)bit_buffer_get_data(
        instance->rx_buffer);
  }
  return error;
}

LegicPrimeError legic_prime_poller_write_byte(
    LegicPrimePoller *instance, uint16_t addr, uint8_t data,
    LegicPrimePollerReadCommandResponse **const response_ptr) {

  furi_assert(instance);
  furi_assert(addr);
  furi_assert(data);
  furi_assert(response_ptr);
  furi_assert(instance->data->tag.cardsize > 0);

  if (addr < 7)
    return LegicPrimeErrorNone;

  // Prepare bit buffer
  bit_buffer_reset(instance->rx_buffer);
  bit_buffer_reset(instance->tx_buffer);

  uint16_t addr_sz = instance->data->tag.addrsize;
  uint32_t tx_frame = addr << 1 | LegicPrimeCmdWrite; // prepare command

  // init crc calculator
  crc_init(&legic_crc, 4, 0x19 >> 1, 0x05, 0);
  uint8_t crc = calc_crc4(tx_frame, addr_sz + 1, data); // calculate crc
  tx_frame |= data << (addr_sz + 1);                    // append value
  tx_frame |= (crc & 0xF) << (addr_sz + 1 + 8);         // and crc

  uint16_t cmdsize = addr_sz + 1 + 8 + 4; // cmd_sz = addr_sz + cmd + data + crc

  for (int i = 0; i < cmdsize; i++) {
    uint8_t bit = (tx_frame >> i) & 0x01;
    bit_buffer_append_bit(instance->tx_buffer, bit);
  }

  for (int i = 0; i < 1; i++) {
    uint8_t bit = 0;
    bit_buffer_append_bit(instance->rx_buffer, bit);
  }

  LegicPrimeError error = legic_prime_poller_frame_exchange(
      instance, instance->tx_buffer, instance->rx_buffer,
      LEGIC_PRIME_POLLER_POLLING_FWT);
  if (error == LegicPrimeErrorNone) {
    *response_ptr = (LegicPrimePollerWriteCommandResponse *)bit_buffer_get_data(
        instance->rx_buffer);
  }
  return error;
}

LegicPrimeError legic_prime_poller_activate(LegicPrimePoller *instance,
                                            LegicPrimeData *data) {
  furi_assert(instance);

  LegicPrimeError ret;
  uint32_t timeout = 100;

  do {
    bit_buffer_reset(instance->tx_buffer);
    bit_buffer_reset(instance->rx_buffer);

    // Send Polling command
    LegicPrimePollerPollingCommand polling_cmd = {
        .num_bits = 7,
        .data = timeout,
    };

    LegicPrimePollerPollingResponse polling_resp = {
        .num_bits = 6,
        .data = 0,
    };

    ret = legic_prime_poller_polling(instance, &polling_cmd, &polling_resp);
    if (ret != LegicPrimeErrorNone) {
      FURI_LOG_T(TAG, "Activation failed error: %d", ret);
      instance->legic_prime_event.type = LegicPrimePollerEventTypeFail;
      break;
    }

    ret = legic_prime_init_tag(polling_resp.data, &data->tag);
    if (ret != LegicPrimeErrorNone) {
      FURI_LOG_T(TAG, "Tag init failed error: %d", ret);
      instance->legic_prime_event.type = LegicPrimePollerEventTypeFail;
      break;
    }
    data->blocks_total = data->tag.cardsize;

    instance->legic_prime_event.type = LegicPrimePollerEventTypeRequestMode;
    // instance->state = LegicPrimePollerStateActivated;
  } while (false);

  return ret;
}
