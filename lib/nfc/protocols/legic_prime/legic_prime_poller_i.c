#include "legic_prime_poller_i.h"
#include "nfc.h"
#include "protocols/legic_prime/legic_prime.h"
#include "protocols/legic_prime/legic_prime_poller.h"
// #include <nfc/helpers/legic_prime_crc.h>

#define TAG "LegicPrimePoller"

#if 0
static crc_t legic_crc;

static uint8_t calc_crc4(uint16_t cmd, uint8_t cmd_sz, uint8_t value) {
  crc_clear(&legic_crc);
  crc_update(&legic_crc, (value << cmd_sz) | cmd, 8 + cmd_sz);
  return crc_finish(&legic_crc);
}
#endif

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

LegicPrimeError legic_prime_init_tag(LegicPrimeTagType cardtype,
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

LegicPrimeError legic_prime_poller_trx(LegicPrimePoller *instance,
                                       LegicPrimePollerTrxData *trx_data) {

  furi_assert(instance);
  furi_assert(trx_data);

  LegicPrimeError ret = LegicPrimeErrorNone;
  NfcError error =
      nfc_legic_prime_poller_trx(instance->nfc, (uint8_t *)trx_data);
  if (error != NfcErrorNone) {
    ret = legic_prime_poller_process_error(error);
  }
  return ret;
}

LegicPrimeError legic_prime_poller_activate(LegicPrimePoller *instance,
                                            LegicPrimeData *data) {
  furi_assert(instance);

  LegicPrimeError ret;

  LegicPrimePollerTrxData *trx_data = malloc(sizeof(LegicPrimePollerTrxData));
  memset(trx_data, 0, sizeof(LegicPrimePollerTrxData));

  do {

    ret = legic_prime_poller_trx(instance, trx_data);

    if (ret != LegicPrimeErrorNone) {
      FURI_LOG_T(TAG, "Activation failed error: %d", ret);
      instance->legic_prime_event.type = LegicPrimePollerEventTypeFail;
      break;
    }

    ret = legic_prime_init_tag(trx_data->tag.tagtype, &data->tag);
    if (ret != LegicPrimeErrorNone) {
      FURI_LOG_T(TAG, "Tag init failed error: %d", ret);
      instance->legic_prime_event.type = LegicPrimePollerEventTypeFail;
      break;
    }
    // data->blocks_total = data->tag.cardsize;
    instance->legic_prime_event.type = LegicPrimePollerEventTypeRequestMode;

  } while (false);

  free(trx_data);
  return ret;
}
