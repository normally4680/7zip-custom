// Custom/Core/VaultCrypto.h
//
// 密码库的加密原语。只依赖 Windows 系统库（BCrypt + DPAPI），
// 不引入 OpenSSL / libsodium 等第三方依赖，保持 7-Zip 的"零外部依赖"风格。

#ifndef CUSTOM_VAULT_CRYPTO_H
#define CUSTOM_VAULT_CRYPTO_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace custom {

constexpr size_t kAesKeySize = 32;   // AES-256
constexpr size_t kGcmIvSize  = 12;   // GCM 标准 96-bit nonce
constexpr size_t kGcmTagSize = 16;   // 128-bit tag

bool CryptoRandom(void *buf, size_t size);

// AES-256-GCM。out 允许与 in 指向同一块内存（原地加密），
// 但 tag 必须独立存放。
bool AesGcmEncrypt(const uint8_t *key,
                   const uint8_t *iv,
                   const void *aad, size_t aadSize,
                   const void *in, size_t inSize,
                   void *out,
                   uint8_t *tag);

bool AesGcmDecrypt(const uint8_t *key,
                   const uint8_t *iv,
                   const void *aad, size_t aadSize,
                   const void *in, size_t inSize,
                   void *out,
                   const uint8_t *tag);

// DPAPI，当前用户作用域：只有同一 Windows 账户能解开，
// 把文件拷走也无法在别的机器/账户上解密。
bool DpapiProtect(const void *in, size_t inSize, std::vector<uint8_t> &out);
bool DpapiUnprotect(const void *in, size_t inSize, std::vector<uint8_t> &out);

} // namespace custom

#endif
