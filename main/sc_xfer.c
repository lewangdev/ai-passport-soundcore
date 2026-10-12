// main/sc_xfer.c —— 文件传输帧构造。
#include "sc_xfer.h"

#include "sc_crypto.h"

#include <string.h>

size_t sc_xfer_head_frame(uint8_t *out, size_t cap, const sc_session_t *session, sc_xfer_file_t *file,
                          uint8_t error_code)
{
    uint8_t payload[SC_FILE_HEAD_LEN];
    sc_file_keys_t keys;
    bool with_keys = session && session->active && error_code == 0;
    file->encrypt = false;
    if (with_keys) {
        memcpy(keys.nonce, file->nonce, sizeof(keys.nonce));
        sc_crypto_random_iv(keys.session_nonce);
        if (sc_crypto_wrap_file_key(session->key, keys.session_nonce, file->file_key, keys.wrapped_key) != 0) {
            with_keys = false;
        } else {
            file->encrypt = true;
        }
    }
    if (!with_keys && error_code != 0) {
        memset(&keys, 0, sizeof(keys));
        with_keys = true;                       // 错误码位于长头的固定偏移
    }
    size_t plen = sc_build_file_head(payload, sizeof(payload), file->file_id, file->size,
                                     with_keys ? &keys : NULL, error_code);
    return sc_frame_build(out, cap, 1, SC_T_FILE, SC_ID_FILE_HEAD, payload, plen);
}

size_t sc_xfer_slice_frame(uint8_t *out, size_t cap, const sc_xfer_file_t *file, uint32_t first_seq,
                           const uint8_t (*bufs)[SC_UNIT_DATA], const uint8_t *lens, size_t n)
{
    size_t total = sc_xfer_slice_frame_size(n);
    if (n == 0 || n > SC_XFER_MAX_UNITS || total > cap) return 0;
    const uint8_t *frames[SC_XFER_MAX_UNITS];
    for (size_t i = 0; i < n; i++) frames[i] = bufs[i];
    uint8_t *payload = out + SC_HDR_LEN;
    size_t plen = sc_build_slices(payload, cap - SC_FRAME_OVERHEAD, first_seq, frames, lens, n);
    if (plen == 0) return 0;
    if (file->encrypt) {
        for (size_t i = 0; i < n; i++) {
            uint8_t *data = payload + i * SC_UNIT_SIZE + 5;
            sc_crypto_encrypt_unit(file->file_key, file->nonce, first_seq + (uint32_t)i, data);
        }
    }
    return sc_frame_build(out, cap, 1, SC_T_FILE, SC_ID_FILE_SLICE, payload, plen);
}

size_t sc_xfer_done_frame(uint8_t *out, size_t cap, uint32_t file_id)
{
    uint8_t payload[4];
    sc_wr32(payload, file_id);
    return sc_frame_build(out, cap, 1, SC_T_FILE, SC_ID_FILE_DONE, payload, sizeof(payload));
}
