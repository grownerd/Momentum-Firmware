#include "protocols/legic_prime/legic_prime_poller.h"
#include "legic_prime_poller_i.h"

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
  instance->tx_buffer = bit_buffer_alloc(LEGIC_PRIME_POLLER_MAX_BUFFER_SIZE);
  instance->rx_buffer = bit_buffer_alloc(LEGIC_PRIME_POLLER_MAX_BUFFER_SIZE);

  nfc_config(instance->nfc, NfcModePoller, NfcTechLegicPrime);
#if 0
    nfc_set_guard_time_us(instance->nfc, LEGIC_PRIME_GUARD_TIME_US);
    nfc_set_fdt_poll_fc(instance->nfc, LEGIC_PRIME_FDT_POLL_FC);
    nfc_set_fdt_poll_poll_us(instance->nfc, LEGIC_PRIME_POLL_POLL_MIN_US);
#else
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

  furi_assert(instance->tx_buffer);
  furi_assert(instance->rx_buffer);
  furi_assert(instance->data);

#if 1
  bit_buffer_free(instance->tx_buffer);
  bit_buffer_free(instance->rx_buffer);
  legic_prime_free(instance->data);
  free(instance);
#endif
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

  LegicPrimeError error = legic_prime_poller_activate(instance, instance->data);
  instance->legic_prime_event_data.error = error;

  if (error == LegicPrimeErrorNone) {

    instance->callback(instance->general_event, instance->context);

    instance->legic_prime_event.type = LegicPrimePollerEventTypeRequestMode;
    instance->state = LegicPrimePollerStateRequestMode;

  } else {
    instance->legic_prime_event.type = LegicPrimePollerEventTypeFail;
    instance->state = LegicPrimePollerStateReadFailed;
    // command = NfcCommandStop;
  }
  return NfcCommandContinue;
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

  LegicPrimePollerReadCommandResponse *response;

  instance->data->blocks_read = 0;
  for (int i = 0; i < instance->data->tag.cardsize; i++) {
    LegicPrimeError error =
        legic_prime_poller_read_byte(instance, i, &response);
    if (error == LegicPrimeErrorNone) {
      uint8_t *data_ptr = instance->data->data;

      uint8_t *response_data_ptr = response->foo;
      instance->data->blocks_read++;
      memcpy(data_ptr + i, response_data_ptr, LEGIC_PRIME_DATA_BLOCK_SIZE);
      instance->state = LegicPrimePollerStateReadSuccess;

      FURI_LOG_I(TAG, "Read byte %3d: 0x%02X", i, response_data_ptr[0]);
    } else {
      instance->legic_prime_event.type = LegicPrimePollerEventTypeFail;
      instance->legic_prime_event_data.error = error;
      instance->state = LegicPrimePollerStateReadFailed;
      i--;
    }
  }
  return NfcCommandContinue;
}

NfcCommand
legic_prime_poller_state_handler_write_blocks(LegicPrimePoller *instance) {
  FURI_LOG_D(TAG, "Write Blocks");

  LegicPrimePollerReadCommandResponse *response;

  uint16_t blocks_written = 0;
  for (int i = 7; i < instance->data->tag.cardsize; i++) {
    // FIXME: why is data empty?
    LegicPrimePollerEvent event = instance->legic_prime_event;
    // uint8_t byte = instance->data->data[i];
    uint8_t byte = event.write_data->data[i];
    LegicPrimeError error =
        legic_prime_poller_write_byte(instance, i, byte, &response);

    if (error == LegicPrimeErrorNone) {

      uint8_t *response_data_ptr = response->foo;
      blocks_written++;
      instance->state = LegicPrimePollerStateWriteSuccess;

      FURI_LOG_I(TAG, "Wrote byte %3d: 0x%02X, response: 0x%03X", i, byte,
                 response_data_ptr[0]);
    } else {
      instance->legic_prime_event.type = LegicPrimePollerEventTypeFail;
      instance->legic_prime_event_data.error = error;
      instance->state = LegicPrimePollerStateWriteFailed;
      i--;
    }
  }
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
