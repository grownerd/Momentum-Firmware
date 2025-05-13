#include "../nfc_app_i.h"

#include <nfc/protocols/legic_prime/legic_prime_poller.h>

NfcCommand
nfc_scene_legic_prime_write_initial_worker_callback(NfcGenericEvent event,
                                                    void *context) {
  furi_assert(context);
  furi_assert(event.event_data);
  furi_assert(event.protocol == NfcProtocolLegicPrime);

  NfcCommand command = NfcCommandContinue;
  NfcApp *instance = context;
  LegicPrimePollerEvent *legic_prime_event = event.event_data;

  if (legic_prime_event->type == LegicPrimePollerEventTypeRequestMode) {
    // how do I get write data to the poller?
    const LegicPrimeData *write_data =
        nfc_device_get_data(instance->nfc_device, NfcProtocolLegicPrime);
    legic_prime_event->write_data = write_data;
    legic_prime_event->data->poller_mode.mode = LegicPrimePollerModeWrite;
  } else if (legic_prime_event->type == LegicPrimePollerEventTypeSuccess) {
    view_dispatcher_send_custom_event(instance->view_dispatcher,
                                      NfcCustomEventPollerSuccess);
    command = NfcCommandStop;
  } else if (legic_prime_event->type == LegicPrimePollerEventTypeFail) {
    view_dispatcher_send_custom_event(instance->view_dispatcher,
                                      NfcCustomEventPollerFailure);
    command = NfcCommandStop;
  }
  return command;
}

static void nfc_scene_legic_prime_write_initial_setup_view(NfcApp *instance) {
  Popup *popup = instance->popup;
  popup_reset(popup);

  popup_set_header(popup, "Writing\nDon't move...", 52, 32, AlignLeft,
                   AlignCenter);
  popup_set_icon(popup, 12, 23, &A_Loading_24);

  view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewPopup);
}

void nfc_scene_legic_prime_write_initial_on_enter(void *context) {
  NfcApp *instance = context;
  dolphin_deed(DolphinDeedNfcEmulate);

  nfc_scene_legic_prime_write_initial_setup_view(instance);

  // Setup and start worker
  instance->poller = nfc_poller_alloc(instance->nfc, NfcProtocolLegicPrime);
  nfc_poller_start(instance->poller,
                   nfc_scene_legic_prime_write_initial_worker_callback,
                   instance);

  nfc_blink_emulate_start(instance);
}

bool nfc_scene_legic_prime_write_initial_on_event(void *context,
                                                  SceneManagerEvent event) {
  NfcApp *instance = context;
  bool consumed = false;

  if (event.type == SceneManagerEventTypeCustom) {
    if (event.event == NfcCustomEventPollerSuccess) {
      scene_manager_next_scene(instance->scene_manager,
                               NfcSceneLegicPrimeWriteInitialSuccess);
      consumed = true;
    } else if (event.event == NfcCustomEventPollerFailure) {
      scene_manager_next_scene(instance->scene_manager,
                               NfcSceneLegicPrimeWriteInitialFail);
      consumed = true;
    }
  }

  return consumed;
}

void nfc_scene_legic_prime_write_initial_on_exit(void *context) {
  NfcApp *instance = context;

  nfc_poller_stop(instance->poller);
  nfc_poller_free(instance->poller);
  // Clear view
  popup_reset(instance->popup);

  nfc_blink_stop(instance);
}
