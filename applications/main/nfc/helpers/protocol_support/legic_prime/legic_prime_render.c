#include "legic_prime_render.h"

void nfc_render_legic_prime_format_bytes(FuriString *str, const uint8_t *data,
                                         size_t size) {
  for (size_t i = 0; i < size; i++) {
    if (!i % 8)
      furi_string_cat_printf(str, "\n");
    furi_string_cat_printf(str, "%02X ", data[i]);
  }
}

void nfc_render_legic_prime_tech_type(const LegicPrimeData *data,
                                      FuriString *str) {
  furi_string_cat_printf(str, "Tech: LegicPrime MIM %d\n", data->tag.cardsize);
}

void nfc_render_legic_prime_brief(const LegicPrimeData *data, FuriString *str) {
  furi_string_cat_printf(str, "UID: %02X %02X %02X %02X\nCRC: %02X\n",
                         data->data[0], data->data[1], data->data[2],
                         data->data[3], data->data[4]);
}

void nfc_render_legic_prime_info(const LegicPrimeData *data,
                                 NfcProtocolFormatType format_type,
                                 FuriString *str) {
  if (format_type == NfcProtocolFormatTypeFull) {
    nfc_render_legic_prime_tech_type(data, str);
  }

  nfc_render_legic_prime_brief(data, str);
}

static void legic_prime_render_raw_data(const uint8_t *data, size_t size,
                                        FuriString *str) {
  furi_assert((size % 2) == 0);

  for (size_t i = 0; i < size; i += 2) {
    furi_string_cat_printf(str, "%02X%02X ", data[i], data[i + 1]);
  }
}

void nfc_render_legic_prime_dump(const LegicPrimeData *data, FuriString *str) {
  furi_assert((data->tag.cardsize % 8) == 0);

  uint8_t clear_data[data->tag.cardsize];

  for (size_t i = 0; i < data->tag.cardsize; i++) {
    clear_data[i] = i < 22 ? data->data[i] : data->data[i] ^ data->data[4];
  }

  for (size_t i = 0; i < data->tag.cardsize; i += 8) {
    legic_prime_render_raw_data(clear_data + i, 8, str);
  }
}
