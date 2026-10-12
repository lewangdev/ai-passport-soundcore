// main/sc_crypto.h —— D3200 文件加密:ECDH(P-256) 会话 + 每文件 AES-256-CTR。
//
// 与客户端(Feishu KeyManager / Soundcore DeviceCrypto)对应:
//   1. 手机经 0x2E/0x01 发来 65 字节未压缩公钥;
//   2. 设备生成临时密钥对,shared = ECDH 的 X 坐标(32 字节);
//      应答 设备公钥(65) + shared(32),手机自己算一遍比对;
//   3. sessionKey = HKDF-SHA256(shared, salt={1,2,3}, info={1,2,3}, 32);
//   4. 每个文件头携带 AES-CTR(sessionKey, sessionNonce, "soundcored3200" ‖ fileKey);
//   5. 每个 160 字节单元:AES-CTR(fileKey, IV = nonce[0..12) ‖ BE32(seq*10))。
//
// 只依赖 mbedtls(ESP-IDF 自带;host 测试用系统 mbedtls 3.x)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SC_PUBKEY_LEN 65u
#define SC_KEY_LEN    32u

// 填充随机字节(目标板:硬件 RNG;host:操作系统随机源)。
void sc_crypto_random(uint8_t *buf, size_t len);

// 完成一次握手。成功返回 0,并写出设备公钥、shared 与派生的会话密钥。
int sc_crypto_handshake(const uint8_t app_pub[SC_PUBKEY_LEN], uint8_t dev_pub[SC_PUBKEY_LEN],
                        uint8_t shared[SC_KEY_LEN], uint8_t session_key[SC_KEY_LEN]);

// RFC 5869 HKDF-SHA256。成功返回 0。
int sc_crypto_hkdf_sha256(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len,
                          const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len);

// AES-256-CTR(IV 为完整 16 字节初始计数块,按 128 位大端递增)。成功返回 0。
int sc_crypto_aes_ctr(const uint8_t key[SC_KEY_LEN], const uint8_t iv[16], const uint8_t *in,
                      uint8_t *out, size_t len);

// 生成 CTR 用的随机 16 字节初值,末字节压到 <= 0xF0:任何实现都只在最低字节内递增,
// 不必依赖各家对 128 位进位的处理是否一致。
void sc_crypto_random_iv(uint8_t iv[16]);

// 用会话密钥包裹文件密钥:out = AES-CTR(session_key, session_nonce, magic ‖ file_key)。
int sc_crypto_wrap_file_key(const uint8_t session_key[SC_KEY_LEN], const uint8_t session_nonce[16],
                            const uint8_t file_key[SC_KEY_LEN], uint8_t out[46]);

// 原地加密一个 160 字节音频单元。
int sc_crypto_encrypt_unit(const uint8_t file_key[SC_KEY_LEN], const uint8_t nonce[16], uint32_t seq,
                           uint8_t data[160]);
