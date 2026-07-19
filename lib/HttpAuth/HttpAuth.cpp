#include "hydracup_auth.h"

#include <cstdlib>
#include <cstring>

#include "esp_random.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

namespace {
constexpr size_t SALT_BYTES = 16;
constexpr size_t HASH_BYTES = 32;
constexpr uint32_t DEFAULT_ITERATIONS = 12000;

bool equalBytes(const uint8_t* left, const uint8_t* right, size_t length) {
    uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i) difference |= static_cast<uint8_t>(left[i] ^ right[i]);
    return difference == 0;
}

std::string hexEncode(const uint8_t* input, size_t length) {
    static const char* HEX = "0123456789abcdef";
    std::string value; value.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        value.push_back(HEX[input[i] >> 4]); value.push_back(HEX[input[i] & 0xf]);
    }
    return value;
}

int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool hexDecode(const std::string& input, uint8_t* output, size_t length) {
    if (input.size() != length * 2) return false;
    for (size_t i = 0; i < length; ++i) {
        const int high = hexValue(input[i * 2]); const int low = hexValue(input[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

bool hmacSha256(const uint8_t* key, size_t keyLength, const uint8_t* input,
                size_t inputLength, uint8_t output[HASH_BYTES]) {
    const auto* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info && mbedtls_md_hmac(info, key, keyLength, input, inputLength, output) == 0;
}

bool pbkdf2(const std::string& password, const uint8_t* salt, size_t saltLength,
            uint32_t iterations, uint8_t output[HASH_BYTES]) {
    if (password.empty() || saltLength > 60 || iterations == 0) return false;
    uint8_t block[64] = {}, u[HASH_BYTES] = {}, next[HASH_BYTES] = {}, total[HASH_BYTES] = {};
    std::memcpy(block, salt, saltLength);
    block[saltLength + 3] = 1;
    const auto* passwordBytes = reinterpret_cast<const uint8_t*>(password.data());
    if (!hmacSha256(passwordBytes, password.size(), block, saltLength + 4, u)) return false;
    std::memcpy(total, u, sizeof(total));
    for (uint32_t round = 1; round < iterations; ++round) {
        if (!hmacSha256(passwordBytes, password.size(), u, sizeof(u), next)) return false;
        for (size_t i = 0; i < sizeof(total); ++i) total[i] ^= next[i];
        std::memcpy(u, next, sizeof(u));
    }
    std::memcpy(output, total, sizeof(total));
    mbedtls_platform_zeroize(block, sizeof(block)); mbedtls_platform_zeroize(u, sizeof(u));
    mbedtls_platform_zeroize(next, sizeof(next)); mbedtls_platform_zeroize(total, sizeof(total));
    return true;
}
}

std::string http_random_token(size_t byteCount) {
    byteCount = byteCount > 32 ? 32 : byteCount;
    uint8_t bytes[32] = {};
    esp_fill_random(bytes, byteCount);
    const std::string result = hexEncode(bytes, byteCount);
    mbedtls_platform_zeroize(bytes, sizeof(bytes));
    return result;
}

bool http_constant_time_equal(const std::string& left, const std::string& right) {
    const size_t length = left.size() > right.size() ? left.size() : right.size();
    size_t difference = left.size() ^ right.size();
    for (size_t i = 0; i < length; ++i) {
        const uint8_t a = i < left.size() ? static_cast<uint8_t>(left[i]) : 0;
        const uint8_t b = i < right.size() ? static_cast<uint8_t>(right[i]) : 0;
        difference |= static_cast<size_t>(a ^ b);
    }
    return difference == 0;
}

std::string http_cookie_value(const std::string& header, const char* name) {
    const std::string prefix = std::string(name) + "=";
    size_t start = 0;
    while (start < header.size()) {
        while (start < header.size() && (header[start] == ' ' || header[start] == ';')) ++start;
        const size_t separator = header.find(';', start);
        const size_t end = separator == std::string::npos ? header.size() : separator;
        if (header.compare(start, prefix.size(), prefix) == 0)
            return header.substr(start + prefix.size(), end - start - prefix.size());
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    return {};
}

std::string http_create_password_hash(const std::string& password) {
    uint8_t salt[SALT_BYTES] = {}, derived[HASH_BYTES] = {};
    esp_fill_random(salt, sizeof(salt));
    if (!pbkdf2(password, salt, sizeof(salt), DEFAULT_ITERATIONS, derived)) return {};
    std::string result = "pbkdf2-sha256$" + std::to_string(DEFAULT_ITERATIONS) + "$" +
                         hexEncode(salt, sizeof(salt)) + "$" + hexEncode(derived, sizeof(derived));
    mbedtls_platform_zeroize(salt, sizeof(salt)); mbedtls_platform_zeroize(derived, sizeof(derived));
    return result;
}

bool http_verify_password_hash(const std::string& password, const std::string& stored) {
    const size_t first = stored.find('$');
    const size_t second = first == std::string::npos ? first : stored.find('$', first + 1);
    const size_t third = second == std::string::npos ? second : stored.find('$', second + 1);
    if (first == std::string::npos || second == std::string::npos || third == std::string::npos) return false;
    if (!http_constant_time_equal(stored.substr(0, first), "pbkdf2-sha256")) return false;
    char* end = nullptr;
    const std::string iterationText = stored.substr(first + 1, second - first - 1);
    const unsigned long iterations = std::strtoul(iterationText.c_str(), &end, 10);
    if (!end || end == iterationText.c_str() || *end != '\0' || iterations == 0 || iterations > 1000000UL) return false;
    uint8_t salt[SALT_BYTES] = {}, expected[HASH_BYTES] = {}, actual[HASH_BYTES] = {};
    const bool decoded = hexDecode(stored.substr(second + 1, third - second - 1), salt, sizeof(salt)) &&
                         hexDecode(stored.substr(third + 1), expected, sizeof(expected));
    const bool derived = decoded && pbkdf2(password, salt, sizeof(salt), static_cast<uint32_t>(iterations), actual);
    const bool result = derived && equalBytes(actual, expected, sizeof(actual));
    mbedtls_platform_zeroize(salt, sizeof(salt)); mbedtls_platform_zeroize(expected, sizeof(expected));
    mbedtls_platform_zeroize(actual, sizeof(actual));
    return result;
}
