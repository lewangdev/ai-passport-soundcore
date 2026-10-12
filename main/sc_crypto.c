// main/sc_crypto.c —— D3200 文件加密实现(mbedtls)。
#include "sc_crypto.h"
#include "sc_proto.h"

#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/md.h"

#ifdef ESP_PLATFORM
#include "esp_random.h"
#else
#include <stdio.h>
#include <stdlib.h>
#endif

void sc_crypto_random(uint8_t *buf, size_t len)
{
#ifdef ESP_PLATFORM
    // RF 子系统(BLE/Wi-Fi)开启时为真随机;本固件在任何握手前都已启动 BLE。
    esp_fill_random(buf, len);
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(buf, 1, len, f) != len) abort();
    fclose(f);
#endif
}

static int rng_cb(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    sc_crypto_random(out, len);
    return 0;
}

int sc_crypto_handshake(const uint8_t app_pub[SC_PUBKEY_LEN], uint8_t dev_pub[SC_PUBKEY_LEN],
                        uint8_t shared[SC_KEY_LEN], uint8_t session_key[SC_KEY_LEN])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d, z;
    mbedtls_ecp_point q, peer;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&q);
    mbedtls_ecp_point_init(&peer);

    size_t olen = 0;
    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc == 0) rc = mbedtls_ecp_point_read_binary(&grp, &peer, app_pub, SC_PUBKEY_LEN);
    if (rc == 0) rc = mbedtls_ecp_check_pubkey(&grp, &peer);
    if (rc == 0) rc = mbedtls_ecp_gen_keypair(&grp, &d, &q, rng_cb, NULL);
    if (rc == 0) rc = mbedtls_ecdh_compute_shared(&grp, &z, &peer, &d, rng_cb, NULL);
    if (rc == 0) rc = mbedtls_mpi_write_binary(&z, shared, SC_KEY_LEN);
    if (rc == 0) {
        rc = mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen,
                                            dev_pub, SC_PUBKEY_LEN);
    }
    if (rc == 0 && olen != SC_PUBKEY_LEN) rc = -1;
    if (rc == 0) {
        static const uint8_t k123[3] = { 1, 2, 3 };
        rc = sc_crypto_hkdf_sha256(shared, SC_KEY_LEN, k123, sizeof(k123), k123, sizeof(k123),
                                   session_key, SC_KEY_LEN);
    }

    mbedtls_ecp_point_free(&peer);
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

static int hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *a, size_t a_len,
                       const uint8_t *b, size_t b_len, const uint8_t *c, size_t c_len, uint8_t out[32])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, md, 1);
    if (rc == 0) rc = mbedtls_md_hmac_starts(&ctx, key, key_len);
    if (rc == 0 && a_len) rc = mbedtls_md_hmac_update(&ctx, a, a_len);
    if (rc == 0 && b_len) rc = mbedtls_md_hmac_update(&ctx, b, b_len);
    if (rc == 0 && c_len) rc = mbedtls_md_hmac_update(&ctx, c, c_len);
    if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
    return rc;
}

int sc_crypto_hkdf_sha256(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len,
                          const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len)
{
    if (out_len > 255u * 32u) return -1;
    uint8_t prk[32];
    int rc = hmac_sha256(salt, salt_len, ikm, ikm_len, NULL, 0, NULL, 0, prk);
    uint8_t t[32];
    size_t t_len = 0;
    size_t done = 0;
    for (uint8_t counter = 1; rc == 0 && done < out_len; counter++) {
        rc = hmac_sha256(prk, sizeof(prk), t, t_len, info, info_len, &counter, 1, t);
        t_len = sizeof(t);
        size_t n = out_len - done < sizeof(t) ? out_len - done : sizeof(t);
        memcpy(out + done, t, n);
        done += n;
    }
    memset(prk, 0, sizeof(prk));
    memset(t, 0, sizeof(t));
    return rc;
}

int sc_crypto_aes_ctr(const uint8_t key[SC_KEY_LEN], const uint8_t iv[16], const uint8_t *in,
                      uint8_t *out, size_t len)
{
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    uint8_t counter[16];
    uint8_t stream[16];
    size_t off = 0;
    memcpy(counter, iv, sizeof(counter));
    int rc = mbedtls_aes_setkey_enc(&aes, key, 256);
    if (rc == 0) rc = mbedtls_aes_crypt_ctr(&aes, len, &off, counter, stream, in, out);
    mbedtls_aes_free(&aes);
    return rc;
}

void sc_crypto_random_iv(uint8_t iv[16])
{
    sc_crypto_random(iv, 16);
    if (iv[15] > 0xF0) iv[15] &= 0x7F;
}

int sc_crypto_wrap_file_key(const uint8_t session_key[SC_KEY_LEN], const uint8_t session_nonce[16],
                            const uint8_t file_key[SC_KEY_LEN], uint8_t out[46])
{
    uint8_t plain[SC_WRAPPED_KEY_LEN];
    memcpy(plain, SC_FILE_KEY_MAGIC, SC_FILE_KEY_MAGIC_LEN);
    memcpy(plain + SC_FILE_KEY_MAGIC_LEN, file_key, SC_KEY_LEN);
    int rc = sc_crypto_aes_ctr(session_key, session_nonce, plain, out, sizeof(plain));
    memset(plain, 0, sizeof(plain));
    return rc;
}

int sc_crypto_encrypt_unit(const uint8_t file_key[SC_KEY_LEN], const uint8_t nonce[16], uint32_t seq,
                           uint8_t data[160])
{
    uint8_t iv[16];
    memcpy(iv, nonce, 12);
    uint32_t c = seq * 10u;                     // 每单元 10 个 AES 块
    iv[12] = (uint8_t)(c >> 24);
    iv[13] = (uint8_t)(c >> 16);
    iv[14] = (uint8_t)(c >> 8);
    iv[15] = (uint8_t)c;
    return sc_crypto_aes_ctr(file_key, iv, data, data, 160);
}
