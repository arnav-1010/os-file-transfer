#pragma once
// ============================================================
//  crypto.h  —  Week 5: AES-128-CBC encryption + SHA-256 integrity
// ============================================================
//  Design:
//    Client: SHA256(plaintext) → AES_encrypt(plaintext) → send [iv|hash|ciphertext]
//    Server: recv [iv|hash|ciphertext] → AES_decrypt → SHA256(plaintext) → verify
//
//  Wire format per encrypted chunk:
//    [ 16 bytes IV ] [ 32 bytes SHA-256 hash of plaintext ] [ N bytes ciphertext ]
//
//  Key: 16-byte pre-shared key defined in TRANSFER_KEY below.
//  Both client and server #include this header — key is shared at compile time.
// ============================================================

#ifndef CRYPTO_H
#define CRYPTO_H

#include <openssl/aes.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <cstdint>
#include <cstring>
#include <string>

// ── Pre-shared AES-128 key (16 bytes) ─────────────────────────────────────────
static const uint8_t TRANSFER_KEY[16] = {
    0x4F, 0x53, 0x50, 0x72, 0x6F, 0x6A, 0x65, 0x63,   // "OSProjec"
    0x74, 0x32, 0x30, 0x32, 0x35, 0x49, 0x54, 0x42    // "t2025ITB"
};

// ── Constants ─────────────────────────────────────────────────────────────────
static constexpr int AES_BLOCK    = 16;   // AES block size
static constexpr int IV_SIZE      = 16;   // random IV per chunk
static constexpr int HASH_SIZE    = 32;   // SHA-256 digest size
static constexpr int CRYPTO_HDR   = IV_SIZE + HASH_SIZE;  // 48-byte header

// ── Encrypt one plaintext chunk ───────────────────────────────────────────────
// out_buf must be at least CRYPTO_HDR + plaintext_len + AES_BLOCK bytes.
// Returns total bytes written (header + ciphertext), or -1 on error.
inline int crypto_encrypt(const uint8_t* plaintext, int plaintext_len,
                           uint8_t* out_buf, int out_buf_size) {
    if (plaintext_len <= 0) return -1;

    // Pad plaintext to AES block boundary (PKCS#7 style — manual)
    int padded_len = ((plaintext_len / AES_BLOCK) + 1) * AES_BLOCK;
    if (out_buf_size < CRYPTO_HDR + padded_len) return -1;

    // 1. Random IV
    uint8_t iv[IV_SIZE];
    if (RAND_bytes(iv, IV_SIZE) != 1) return -1;
    memcpy(out_buf, iv, IV_SIZE);

    // 2. SHA-256 of original plaintext → stored in header so receiver can verify
    uint8_t hash[HASH_SIZE];
    SHA256(plaintext, static_cast<size_t>(plaintext_len), hash);
    memcpy(out_buf + IV_SIZE, hash, HASH_SIZE);

    // 3. Pad plaintext into a temp buffer
    uint8_t padded[65536 + AES_BLOCK];
    memcpy(padded, plaintext, plaintext_len);
    uint8_t pad_byte = static_cast<uint8_t>(padded_len - plaintext_len);
    memset(padded + plaintext_len, pad_byte, padded_len - plaintext_len);

    // 4. AES-128-CBC encrypt
    AES_KEY aes_key;
    AES_set_encrypt_key(TRANSFER_KEY, 128, &aes_key);
    uint8_t iv_copy[IV_SIZE];
    memcpy(iv_copy, iv, IV_SIZE);   // AES_cbc_encrypt modifies iv in place
    AES_cbc_encrypt(padded, out_buf + CRYPTO_HDR, padded_len, &aes_key, iv_copy, AES_ENCRYPT);

    return CRYPTO_HDR + padded_len;
}

// ── Decrypt one received chunk ─────────────────────────────────────────────────
// in_buf layout: [16 IV][32 hash][ciphertext]
// out_buf must be >= in_len - CRYPTO_HDR bytes.
// Returns plaintext length on success, -1 on decrypt error, -2 on hash mismatch.
inline int crypto_decrypt(const uint8_t* in_buf, int in_len,
                           uint8_t* out_buf, int out_buf_size) {
    if (in_len <= CRYPTO_HDR) return -1;

    int cipher_len = in_len - CRYPTO_HDR;
    if (cipher_len % AES_BLOCK != 0) return -1;
    if (out_buf_size < cipher_len) return -1;

    // 1. Extract IV and expected hash
    uint8_t iv[IV_SIZE];
    uint8_t expected_hash[HASH_SIZE];
    memcpy(iv,            in_buf,           IV_SIZE);
    memcpy(expected_hash, in_buf + IV_SIZE,  HASH_SIZE);

    // 2. AES-128-CBC decrypt
    AES_KEY aes_key;
    AES_set_decrypt_key(TRANSFER_KEY, 128, &aes_key);
    uint8_t iv_copy[IV_SIZE];
    memcpy(iv_copy, iv, IV_SIZE);
    AES_cbc_encrypt(in_buf + CRYPTO_HDR, out_buf, cipher_len,
                    &aes_key, iv_copy, AES_DECRYPT);

    // 3. Remove PKCS#7 padding
    uint8_t pad_byte = out_buf[cipher_len - 1];
    if (pad_byte == 0 || pad_byte > AES_BLOCK) return -1;
    int plaintext_len = cipher_len - pad_byte;

    // 4. SHA-256 verify
    uint8_t actual_hash[HASH_SIZE];
    SHA256(out_buf, static_cast<size_t>(plaintext_len), actual_hash);
    if (memcmp(actual_hash, expected_hash, HASH_SIZE) != 0) return -2;

    return plaintext_len;
}

// ── Pretty-print a SHA-256 hash (for logging) ─────────────────────────────────
inline std::string hash_to_hex(const uint8_t* hash, int len = HASH_SIZE) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (int i = 0; i < len; i++) {
        s += hex[(hash[i] >> 4) & 0xF];
        s += hex[hash[i] & 0xF];
    }
    return s;
}

#endif // CRYPTO_H
