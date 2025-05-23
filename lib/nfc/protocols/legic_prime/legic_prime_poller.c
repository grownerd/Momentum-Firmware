#include "protocols/legic_prime/legic_prime_poller.h"
#include "legic_prime_poller_i.h"
#include "protocols/legic_prime/legic_prime.h"

#include <nfc/protocols/nfc_poller_base.h>

#include <furi.h>
#include <furi_hal.h>

#define TAG "LegicPrimePoller"

typedef NfcCommand (*LegicPrimePollerReadHandler)(LegicPrimePoller *instance);

const LegicPrimeData *legic_prime_poller_get_data(LegicPrimePoller *instance) {
  furi_assert(instance);
  furi_assert(instance->data);

  return instance->data;
}

static LegicPrimePoller *legic_prime_poller_alloc(Nfc *nfc) {
  furi_assert(nfc);
  LegicPrimePoller *instance = malloc(sizeof(LegicPrimePoller));
  instance->nfc = nfc;

  nfc_config(instance->nfc, NfcModePoller, NfcTechLegicPrime);
#if 0
  instance->tx_buffer = bit_buffer_alloc(LEGIC_PRIME_POLLER_MAX_BUFFER_SIZE);
  instance->rx_buffer = bit_buffer_alloc(LEGIC_PRIME_POLLER_MAX_BUFFER_SIZE);
    nfc_set_guard_time_us(instance->nfc, LEGIC_PRIME_GUARD_TIME_US);
    nfc_set_fdt_poll_fc(instance->nfc, LEGIC_PRIME_FDT_POLL_FC);
    nfc_set_fdt_poll_poll_us(instance->nfc, LEGIC_PRIME_POLL_POLL_MIN_US);
#else
  LegicPrimePollerTrxData *trx_data = malloc(sizeof(LegicPrimePollerTrxData));
  instance->trx_data = trx_data;

  nfc_set_guard_time_us(instance->nfc, 0);
  nfc_set_fdt_poll_fc(instance->nfc, 0);
  nfc_set_fdt_poll_poll_us(instance->nfc, 0);
#endif

  instance->data = legic_prime_alloc();

  instance->legic_prime_event.data = &instance->legic_prime_event_data;
  instance->general_event.protocol = NfcProtocolLegicPrime;
  instance->general_event.event_data = &instance->legic_prime_event;
  instance->general_event.instance = instance;

  return instance;
}

static void legic_prime_poller_free(LegicPrimePoller *instance) {
  furi_assert(instance);

  furi_assert(instance->data);
  furi_assert(instance->trx_data);

#if 0
  bit_buffer_free(instance->tx_buffer);
  bit_buffer_free(instance->rx_buffer);
#endif
  free(instance->trx_data);
  legic_prime_free(instance->data);
  free(instance);
}

static void legic_prime_poller_set_callback(LegicPrimePoller *instance,
                                            NfcGenericCallback callback,
                                            void *context) {
  furi_assert(instance);
  furi_assert(callback);

  instance->callback = callback;
  instance->context = context;
}

NfcCommand legic_prime_poller_state_handler_idle(LegicPrimePoller *instance) {
  FURI_LOG_D(TAG, "Idle");
  NfcCommand command = NfcCommandContinue;

  LegicPrimeError error = legic_prime_poller_activate(instance, instance->data);
  instance->legic_prime_event_data.error = error;

  if (error == LegicPrimeErrorNone) {

    instance->callback(instance->general_event, instance->context);

    instance->legic_prime_event.type = LegicPrimePollerEventTypeRequestMode;
    instance->state = LegicPrimePollerStateRequestMode;

  } else {
    instance->legic_prime_event.type = LegicPrimePollerEventTypeFail;
    instance->state = LegicPrimePollerStateReadFailed;
    command = NfcCommandStop;
  }
  return command;
}

NfcCommand
legic_prime_poller_state_handler_request_mode(LegicPrimePoller *instance) {
  FURI_LOG_D(TAG, "RequestMode");

  if (instance->state == LegicPrimePollerStateRequestMode) {
    if (instance->legic_prime_event_data.poller_mode.mode ==
        LegicPrimePollerModeRead) {
      instance->state = LegicPrimePollerStateReadBlocks;
    } else if (instance->legic_prime_event_data.poller_mode.mode ==
               LegicPrimePollerModeWrite) {
      instance->state = LegicPrimePollerStateWriteBlocks;
    }
  }

  return NfcCommandContinue;
}

NfcCommand
legic_prime_poller_state_handler_read_blocks(LegicPrimePoller *instance) {
  FURI_LOG_D(TAG, "Read Blocks");

  LegicPrimePollerTrxData *trx_data = instance->trx_data;
  size_t tag_sz = instance->data->tag.cardsize;
  // instance->data->blocks_read = 0;

  memcpy(&trx_data->tag, &instance->data->tag, sizeof(LegicPrimeTag));
  trx_data->cmd = LegicPrimeCmdRead;
  trx_data->num_addrs = trx_data->tag.cardsize;
  trx_data->bytes_processed = 0;

  for (size_t i = 0; i < tag_sz; i++) {
    trx_data->addrs[i] = i;
  }

  int retries = 100;

  do {
    LegicPrimeError error = legic_prime_poller_trx(instance, trx_data);

    if (error == LegicPrimeErrorNone) {
      uint8_t *data_ptr = instance->data->data;
      uint8_t *response_data_ptr = trx_data->response_data;

      memcpy(data_ptr, response_data_ptr, LEGIC_PRIME_DATA_BLOCK_SIZE);

      instance->state = LegicPrimePollerStateReadSuccess;
      break;
    } else {
      instance->legic_prime_event.type = LegicPrimePollerEventTypeFail;
      instance->legic_prime_event_data.error = error;
      instance->state = LegicPrimePollerStateReadFailed;
    }
  } while (retries--);
  FURI_LOG_I(TAG, "Read %d bytes with %d retries", trx_data->bytes_processed,
             trx_data->total_errors);

  return NfcCommandContinue;
}

NfcCommand
legic_prime_poller_state_handler_write_blocks(LegicPrimePoller *instance) {
  FURI_LOG_D(TAG, "Write Blocks");

  LegicPrimePollerTrxData *trx_data = instance->trx_data;

  size_t tag_sz = instance->data->tag.cardsize;

  memcpy(&trx_data->tag, &instance->data->tag, sizeof(LegicPrimeTag));
  trx_data->cmd = LegicPrimeCmdWrite;
  trx_data->bytes_processed = 0;

  size_t idx = 0;

  // Go backwards, because bytes 5 and 6 can only be written in reverse order.
  for (int i = tag_sz; i > 6; i--) {

    LegicPrimePollerEvent event = instance->legic_prime_event;
    uint8_t *write_mask = event.write_mask;

    if (write_mask[i]) {
      trx_data->addrs[idx] = i;
      trx_data->write_data[idx] = event.write_data->data[i];
      idx++;
    }
  }
  trx_data->num_addrs = idx;

  int retries = 100;

  do {
    LegicPrimeError error = legic_prime_poller_trx(instance, trx_data);

    if (error == LegicPrimeErrorNone) {
      instance->state = LegicPrimePollerStateWriteSuccess;
      break;
    } else {
      instance->legic_prime_event.type = LegicPrimePollerEventTypeFail;
      instance->legic_prime_event_data.error = error;
      instance->state = LegicPrimePollerStateWriteFailed;
    }
  } while (retries--);
  FURI_LOG_I(TAG, "Wrote %d bytes with %d retries", trx_data->bytes_processed,
             trx_data->total_errors);

  return NfcCommandContinue;
}

NfcCommand
legic_prime_poller_state_handler_read_success(LegicPrimePoller *instance) {
  FURI_LOG_D(TAG, "Read Success");
  instance->legic_prime_event.type = LegicPrimePollerEventTypeSuccess;
  instance->legic_prime_event_data.error = LegicPrimeErrorNone;
  return instance->callback(instance->general_event, instance->context);
}

NfcCommand
legic_prime_poller_state_handler_read_failed(LegicPrimePoller *instance) {
  FURI_LOG_D(TAG, "Read Fail");
  instance->legic_prime_event.type = LegicPrimePollerEventTypeFail;
  instance->legic_prime_event_data.error = LegicPrimeErrorTimeout;
  instance->callback(instance->general_event, instance->context);
  return NfcCommandStop;
}

NfcCommand
legic_prime_poller_state_handler_write_success(LegicPrimePoller *instance) {
  FURI_LOG_D(TAG, "Write Success");
  instance->legic_prime_event.type = LegicPrimePollerEventTypeSuccess;
  instance->legic_prime_event_data.error = LegicPrimeErrorNone;
  return instance->callback(instance->general_event, instance->context);
}

NfcCommand
legic_prime_poller_state_handler_write_failed(LegicPrimePoller *instance) {
  FURI_LOG_D(TAG, "Write Fail");
  instance->callback(instance->general_event, instance->context);
  return NfcCommandStop;
}

static const LegicPrimePollerReadHandler
    legic_prime_poller_handler[LegicPrimePollerStateNum] = {
        [LegicPrimePollerStateIdle] = legic_prime_poller_state_handler_idle,
        [LegicPrimePollerStateRequestMode] =
            legic_prime_poller_state_handler_request_mode,
        [LegicPrimePollerStateReadBlocks] =
            legic_prime_poller_state_handler_read_blocks,
        [LegicPrimePollerStateReadSuccess] =
            legic_prime_poller_state_handler_read_success,
        [LegicPrimePollerStateReadFailed] =
            legic_prime_poller_state_handler_read_failed,
        [LegicPrimePollerStateWriteBlocks] =
            legic_prime_poller_state_handler_write_blocks,
        [LegicPrimePollerStateWriteSuccess] =
            legic_prime_poller_state_handler_write_success,
        [LegicPrimePollerStateWriteFailed] =
            legic_prime_poller_state_handler_write_failed,
};

static NfcCommand legic_prime_poller_run(NfcGenericEvent event, void *context) {
  furi_assert(context);
  furi_assert(event.protocol == NfcProtocolInvalid);
  furi_assert(event.event_data);

  LegicPrimePoller *instance = context;
  NfcEvent *nfc_event = event.event_data;
  NfcCommand command = NfcCommandContinue;
  FURI_LOG_D(TAG, "Poller Run state %d", instance->state);

  if (nfc_event->type == NfcEventTypePollerReady) {
    command = legic_prime_poller_handler[instance->state](instance);
  } else {
    command = NfcCommandReset;
  }

  return command;
}

static bool legic_prime_poller_detect(NfcGenericEvent event, void *context) {
  furi_assert(context);
  furi_assert(event.event_data);
  furi_assert(event.instance);
  furi_assert(event.protocol == NfcProtocolInvalid);

  FURI_LOG_D(TAG, "Detect");

  bool protocol_detected = false;
  LegicPrimePoller *instance = context;
  NfcEvent *nfc_event = event.event_data;
  furi_assert(instance->state == LegicPrimePollerStateIdle);

  // legic_prime_reset(instance->data);

  if (nfc_event->type == NfcEventTypePollerReady) {
    LegicPrimeError error =
        legic_prime_poller_activate(instance, instance->data);
    protocol_detected = (error == LegicPrimeErrorNone);
  }

  return protocol_detected;
}

const NfcPollerBase nfc_poller_legic_prime = {
    .alloc = (NfcPollerAlloc)legic_prime_poller_alloc,
    .free = (NfcPollerFree)legic_prime_poller_free,
    .set_callback = (NfcPollerSetCallback)legic_prime_poller_set_callback,
    .run = (NfcPollerRun)legic_prime_poller_run,
    .detect = (NfcPollerDetect)legic_prime_poller_detect,
    .get_data = (NfcPollerGetData)legic_prime_poller_get_data,
};
