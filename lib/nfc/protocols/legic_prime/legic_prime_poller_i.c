#include "legic_prime_poller_i.h"


#define TAG "LegicPrimePoller"

static LegicPrimeError legic_prime_poller_process_error(NfcError error) {
    switch(error) {
    case NfcErrorNone:
        return LegicPrimeErrorNone;
    case NfcErrorTimeout:
        return LegicPrimeErrorTimeout;
    default:
        return LegicPrimeErrorNotPresent;
    }
}

LegicPrimeError legic_prime_poller_frame_exchange(
    const LegicPrimePoller* instance,
    const BitBuffer* tx_buffer,
    BitBuffer* rx_buffer,
    uint32_t fwt) {
    furi_assert(instance);

    const size_t tx_bytes = bit_buffer_get_size_bytes(tx_buffer);
    furi_assert(tx_bytes <= bit_buffer_get_capacity_bytes(instance->tx_buffer));

    LegicPrimeError ret = LegicPrimeErrorNone;

    do {
        NfcError error =
            nfc_legic_prime_poller_trx(instance->nfc, instance->tx_buffer, instance->rx_buffer, fwt);
            //nfc_poller_trx(instance->nfc, instance->tx_buffer, instance->rx_buffer, fwt);
        if(error != NfcErrorNone) {
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
    } while(false);

    return ret;
}

LegicPrimeError legic_prime_poller_polling(
    LegicPrimePoller* instance,
    const LegicPrimePollerPollingCommand* cmd,
    LegicPrimePollerPollingResponse* resp) {
    furi_assert(instance);
    furi_assert(cmd);
    furi_assert(resp);

    LegicPrimeError error = LegicPrimeErrorNone;

    do {
        bit_buffer_reset(instance->tx_buffer);
        bit_buffer_reset(instance->rx_buffer);

        for (int i=0; i<cmd->num_bits; i++)
        {
            uint8_t bit = (cmd->data >> i) & 0x01;
            bit_buffer_append_bit(instance->tx_buffer, bit);
        }

        error = legic_prime_poller_frame_exchange(
            instance, instance->tx_buffer, instance->rx_buffer, LEGIC_PRIME_POLLER_POLLING_FWT);

        if(error != LegicPrimeErrorNone) break;
#if 1
        bit_buffer_write_bytes_mid(instance->rx_buffer, resp->data, 0, 2);
#else
        if(bit_buffer_get_byte(instance->rx_buffer, 1) != LEGIC_PRIME_POLLER_CMD_POLLING_RESP_CODE) {
            error = LegicPrimeErrorProtocol;
            break;
        }
        if(bit_buffer_get_size_bytes(instance->rx_buffer) <
           sizeof(LegicPrimeIDm) + sizeof(LegicPrimePMm) + 1) {
            error = LegicPrimeErrorProtocol;
            break;
        }

        bit_buffer_write_bytes_mid(instance->rx_buffer, resp->idm.data, 2, sizeof(LegicPrimeIDm));
        bit_buffer_write_bytes_mid(
            instance->rx_buffer, resp->pmm.data, sizeof(LegicPrimeIDm) + 2, sizeof(LegicPrimePMm));
#endif

    } while(false);

    return error;
}

#if 0
static void legic_prime_poller_prepare_tx_buffer(
    const LegicPrimePoller* instance,
    const uint8_t command,
    const uint16_t service_code,
    const uint8_t block_count,
    const uint8_t* const blocks,
    const uint8_t data_block_count,
    const uint8_t* data) {
#if 1
    UNUSED(instance);
    UNUSED(command);
    UNUSED(service_code);
    UNUSED(block_count);
    UNUSED(blocks);
    UNUSED(data_block_count);
    UNUSED(data);

#else
    LegicPrimeCommandHeader cmd = {
        .code = command,
        .idm = instance->data->idm,
        .service_num = 1,
        .service_code = service_code,
        .block_count = block_count,
    };

    LegicPrimeBlockListElement block_list[4] = {{0}, {0}, {0}, {0}};
    for(uint8_t i = 0; i < block_count; i++) {
        block_list[i].length = 1;
        block_list[i].block_number = blocks[i];
    }

    uint8_t block_list_count = block_count;
    uint8_t block_list_size = block_list_count * sizeof(LegicPrimeBlockListElement);
    uint8_t total_size = sizeof(LegicPrimeCommandHeader) + 1 + block_list_size +
                         data_block_count * LEGIC_PRIME_DATA_BLOCK_SIZE;
    bit_buffer_reset(instance->tx_buffer);
    bit_buffer_append_byte(instance->tx_buffer, total_size);
    bit_buffer_append_bytes(instance->tx_buffer, (uint8_t*)&cmd, sizeof(LegicPrimeCommandHeader));
    bit_buffer_append_bytes(instance->tx_buffer, (uint8_t*)&block_list, block_list_size);

    if(data_block_count != 0) {
        bit_buffer_append_bytes(
            instance->tx_buffer, data, data_block_count * LEGIC_PRIME_DATA_BLOCK_SIZE);
    }
#endif
}
#endif

LegicPrimeError legic_prime_poller_read_blocks(
    LegicPrimePoller* instance,
    const uint8_t block_count,
    const uint8_t* const block_numbers,
    LegicPrimePollerReadCommandResponse** const response_ptr) {

    furi_assert(instance);
    furi_assert(block_count <= 4);
    furi_assert(block_numbers);
    furi_assert(response_ptr);

    // Prepare bit buffer
    bit_buffer_reset(instance->rx_buffer);
    bit_buffer_reset(instance->tx_buffer);
    uint16_t tx_frame = block_numbers[0] << 1 | 1;

    // FIXME: 11 if card type mim1024
    for (int i=0; i<9; i++)
    {
        uint8_t bit = (tx_frame >> i) & 0x01;
        bit_buffer_append_bit(instance->tx_buffer, bit);
    }

    LegicPrimeError error = legic_prime_poller_frame_exchange(
        instance, instance->tx_buffer, instance->rx_buffer, LEGIC_PRIME_POLLER_POLLING_FWT);
    if(error == LegicPrimeErrorNone) {
        *response_ptr = (LegicPrimePollerReadCommandResponse*)bit_buffer_get_data(instance->rx_buffer);
    }
    return error;
}

LegicPrimeError legic_prime_poller_write_blocks(
    const LegicPrimePoller* instance,
    const uint8_t block_count,
    const uint8_t* const block_numbers,
    const uint8_t* data,
    LegicPrimePollerWriteCommandResponse** const response_ptr) {
    furi_assert(instance);
    furi_assert(block_count <= 2);
    furi_assert(block_numbers);
    furi_assert(data);
    furi_assert(response_ptr);

    return LegicPrimeErrorNone;
#if 0
    legic_prime_poller_prepare_tx_buffer(
        instance,
        LEGIC_PRIME_CMD_WRITE_WITHOUT_ENCRYPTION,
        LEGIC_PRIME_SERVICE_RW_ACCESS,
        block_count,
        block_numbers,
        block_count,
        data);
    bit_buffer_reset(instance->rx_buffer);

    LegicPrimeError error = legic_prime_poller_frame_exchange(
        instance, instance->tx_buffer, instance->rx_buffer, LEGIC_PRIME_POLLER_POLLING_FWT);
    if(error == LegicPrimeErrorNone) {
        *response_ptr =
            (LegicPrimePollerWriteCommandResponse*)bit_buffer_get_data(instance->rx_buffer);
    }
    return error;
#endif
}

LegicPrimeError legic_prime_poller_activate(LegicPrimePoller* instance, LegicPrimeData* data) {
    furi_assert(instance);

    LegicPrimeError ret;
#if 1
    UNUSED(data);
    instance->state = LegicPrimePollerStateActivated;
    ret = LegicPrimeErrorNone;
#else
    uint8_t iv = 0x01;

    do {
        bit_buffer_reset(instance->tx_buffer);
        bit_buffer_reset(instance->rx_buffer);

        // Send Polling command
        LegicPrimePollerPollingCommand polling_cmd = {
            .num_bits = 7,
            .data = iv,
        };

        LegicPrimePollerPollingResponse polling_resp = {};

        ret = legic_prime_poller_polling(instance, &polling_cmd, &polling_resp);

        if(ret != LegicPrimeErrorNone) {
            FURI_LOG_T(TAG, "Activation failed error: %d", ret);
            break;
        }
        else if (polling_resp.data[0] & 0x3f)
        {
            break;
        }
        else
        {
            ret = LegicPrimeErrorTimeout;
            break;
        }

        data->data[0] = polling_resp.data[0];
        instance->state = LegicPrimePollerStateActivated;
    } while(false);

#endif
    return ret;
}
