#include "legic_prime.h"
#include "legic_prime_render.h"

#include <nfc/protocols/legic_prime/legic_prime_poller.h>

#include "nfc/nfc_app_i.h"

#include "../nfc_protocol_support_common.h"
#include "../nfc_protocol_support_gui_common.h"

#define TAG "LegicPrimeApp"

enum {
  SubmenuIndexWrite = SubmenuIndexCommonMax,
  SubmenuIndexWrite43,
  SubmenuIndexWrite44,
};

static void nfc_scene_info_on_enter_legic_prime(NfcApp *instance) {
  const NfcDevice *device = instance->nfc_device;
  const LegicPrimeData *data =
      nfc_device_get_data(device, NfcProtocolLegicPrime);

  FuriString *temp_str = furi_string_alloc();
  nfc_append_filename_string_when_present(instance, temp_str);

  furi_string_cat_printf(temp_str, "\e#%s\n",
                         nfc_device_get_name(device, NfcDeviceNameTypeFull));
  nfc_render_legic_prime_info(data, NfcProtocolFormatTypeFull, temp_str);

  widget_add_text_scroll_element(instance->widget, 0, 0, 128, 64,
                                 furi_string_get_cstr(temp_str));

  furi_string_free(temp_str);
}

static void nfc_scene_more_info_on_enter_legic_prime(NfcApp *instance) {
  const NfcDevice *device = instance->nfc_device;
  const LegicPrimeData *data =
      nfc_device_get_data(device, NfcProtocolLegicPrime);

  furi_string_reset(instance->text_box_store);
  nfc_render_legic_prime_dump(data, instance->text_box_store);

  text_box_set_font(instance->text_box, TextBoxFontHex);
  text_box_set_text(instance->text_box,
                    furi_string_get_cstr(instance->text_box_store));
  view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewTextBox);
}

static NfcCommand
nfc_scene_read_poller_callback_legic_prime(NfcGenericEvent event,
                                           void *context) {
  furi_assert(event.protocol == NfcProtocolLegicPrime);

  NfcApp *instance = context;
  const LegicPrimePollerEvent *legic_prime_event = event.event_data;

  if (legic_prime_event->type == LegicPrimePollerEventTypeRequestMode) {
    legic_prime_event->data->poller_mode.mode = LegicPrimePollerModeRead;
  } else if (legic_prime_event->type == LegicPrimePollerEventTypeSuccess) {
    nfc_device_set_data(instance->nfc_device, NfcProtocolLegicPrime,
                        nfc_poller_get_data(instance->poller));
    view_dispatcher_send_custom_event(instance->view_dispatcher,
                                      NfcCustomEventPollerSuccess);
    return NfcCommandStop;

  } else if (legic_prime_event->type == LegicPrimePollerEventTypeFail) {
    nfc_device_set_data(instance->nfc_device, NfcProtocolLegicPrime,
                        nfc_poller_get_data(instance->poller));
    view_dispatcher_send_custom_event(instance->view_dispatcher,
                                      NfcCustomEventPollerFailure);
    return NfcCommandStop;
  }

  return NfcCommandContinue;
}

static void nfc_scene_read_on_enter_legic_prime(NfcApp *instance) {
  nfc_poller_start(instance->poller, nfc_scene_read_poller_callback_legic_prime,
                   instance);
}

static void nfc_scene_read_success_on_enter_legic_prime(NfcApp *instance) {
  const NfcDevice *device = instance->nfc_device;
  const LegicPrimeData *data =
      nfc_device_get_data(device, NfcProtocolLegicPrime);

  FuriString *temp_str = furi_string_alloc();
  furi_string_cat_printf(temp_str, "\e#%s\n",
                         nfc_device_get_name(device, NfcDeviceNameTypeFull));
  nfc_render_legic_prime_info(data, NfcProtocolFormatTypeShort, temp_str);

  widget_add_text_scroll_element(instance->widget, 0, 0, 128, 52,
                                 furi_string_get_cstr(temp_str));

  furi_string_free(temp_str);
}

static void nfc_scene_read_menu_on_enter_legic_prime(NfcApp *instance) {
  Submenu *submenu = instance->submenu;
  // const LegicPrimeData *data =
  //     nfc_device_get_data(instance->nfc_device, NfcProtocolLegicPrime);

  // if ((data->data[0x43] ^ data->data[4]) == 0x04) {
  if (1) {
    submenu_add_item(submenu, "Set 0x43", SubmenuIndexWrite43,
                     nfc_protocol_support_common_submenu_callback, instance);
    submenu_add_item(submenu, "Set 0x44", SubmenuIndexWrite44,
                     nfc_protocol_support_common_submenu_callback, instance);
  }
}

static void nfc_scene_saved_menu_on_enter_legic_prime(NfcApp *instance) {
  Submenu *submenu = instance->submenu;

  submenu_add_item(submenu, "Write to tag", SubmenuIndexWrite,
                   nfc_protocol_support_common_submenu_callback, instance);
}

static bool nfc_scene_read_menu_on_event_legic_prime(NfcApp *instance,
                                                     SceneManagerEvent event) {
  bool consumed = false;

  if (event.type == SceneManagerEventTypeCustom) {
    if (event.event == SubmenuIndexWrite43) {
      scene_manager_next_scene(instance->scene_manager,
                               NfcSceneLegicPrimeSet43);
      consumed = true;
    }
    if (event.event == SubmenuIndexWrite44) {
      scene_manager_next_scene(instance->scene_manager,
                               NfcSceneLegicPrimeSet44);
      consumed = true;
    }
  }
  return consumed;
}

static bool nfc_scene_saved_menu_on_event_legic_prime(NfcApp *instance,
                                                      SceneManagerEvent event) {
  bool consumed = false;

  if (event.type == SceneManagerEventTypeCustom) {
    if (event.event == SubmenuIndexWrite) {
      scene_manager_next_scene(instance->scene_manager,
                               NfcSceneLegicPrimeWrite);
      consumed = true;
    }
  }
  return consumed;
}

static NfcCommand
nfc_scene_emulate_listener_callback_legic_prime(NfcGenericEvent event,
                                                void *context) {
  furi_assert(context);
  furi_assert(event.protocol == NfcProtocolLegicPrime);
  furi_assert(event.event_data);

#if 0
    NfcApp* nfc = context;
    LegicPrimeListenerEvent* legic_prime_event = event.event_data;

    if(legic_prime_event->type == LegicPrimeListenerEventTypeReceivedStandardFrame) {
        if(furi_string_size(nfc->text_box_store) < NFC_LOG_SIZE_MAX) {
            furi_string_cat_printf(nfc->text_box_store, "R:");
            for(size_t i = 0; i < bit_buffer_get_size_bytes(legic_prime_event->data->buffer);
                i++) {
                furi_string_cat_printf(
                    nfc->text_box_store,
                    " %02X",
                    bit_buffer_get_byte(legic_prime_event->data->buffer, i));
            }
            furi_string_push_back(nfc->text_box_store, '\n');
            view_dispatcher_send_custom_event(nfc->view_dispatcher, NfcCustomEventListenerUpdate);
        }
    }
#endif

  return NfcCommandContinue;
}

static void nfc_scene_emulate_on_enter_legic_prime(NfcApp *instance) {
  const LegicPrimeData *data =
      nfc_device_get_data(instance->nfc_device, NfcProtocolLegicPrime);

  instance->listener =
      nfc_listener_alloc(instance->nfc, NfcProtocolLegicPrime, data);
  nfc_listener_start(instance->listener,
                     nfc_scene_emulate_listener_callback_legic_prime, instance);
}

const NfcProtocolSupportBase nfc_protocol_support_legic_prime = {
    .features = NfcProtocolFeatureEmulateFull | NfcProtocolFeatureMoreInfo,

    .scene_info =
        {
            .on_enter = nfc_scene_info_on_enter_legic_prime,
            .on_event = nfc_protocol_support_common_on_event_empty,
        },
    .scene_more_info =
        {
            .on_enter = nfc_scene_more_info_on_enter_legic_prime,
            .on_event = nfc_protocol_support_common_on_event_empty,
        },
    .scene_read =
        {
            .on_enter = nfc_scene_read_on_enter_legic_prime,
            .on_event = nfc_protocol_support_common_on_event_empty,
        },
    .scene_read_menu =
        {
            .on_enter = nfc_scene_read_menu_on_enter_legic_prime,
            .on_event = nfc_scene_read_menu_on_event_legic_prime,
        },
    .scene_read_success =
        {
            .on_enter = nfc_scene_read_success_on_enter_legic_prime,
            .on_event = nfc_protocol_support_common_on_event_empty,
        },
    .scene_saved_menu =
        {
            .on_enter = nfc_scene_saved_menu_on_enter_legic_prime,
            .on_event = nfc_scene_saved_menu_on_event_legic_prime,
        },
    .scene_save_name =
        {
            .on_enter = nfc_protocol_support_common_on_enter_empty,
            .on_event = nfc_protocol_support_common_on_event_empty,
        },
    .scene_emulate =
        {
            .on_enter = nfc_scene_emulate_on_enter_legic_prime,
            .on_event = nfc_protocol_support_common_on_event_empty,
        },
};

NFC_PROTOCOL_SUPPORT_PLUGIN(legic_prime, NfcProtocolLegicPrime);
