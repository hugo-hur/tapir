#ifndef TAPIR_SECURITY_HPP
#define TAPIR_SECURITY_HPP

#include "raii.hpp" // EvpCtxDeleter, hex_digit

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace tapir::security
{
    // Uint64 to big-endian bytes.
    inline std::array<std::byte, 8> Uint64ToBigEndianBytes(std::uint64_t v)
    {
        std::array<std::byte, 8> out;

        out[0] = std::byte((v >> 56) & 0xFF);
        out[1] = std::byte((v >> 48) & 0xFF);
        out[2] = std::byte((v >> 40) & 0xFF);
        out[3] = std::byte((v >> 32) & 0xFF);
        out[4] = std::byte((v >> 24) & 0xFF);
        out[5] = std::byte((v >> 16) & 0xFF);
        out[6] = std::byte((v >> 8) & 0xFF);
        out[7] = std::byte((v >> 0) & 0xFF);

        return out;
    }

    // Byte-array concatenation helper. Field boundaries are fixed at compile time
    // by each array's size, so distinct inputs never collide (see the KDF note in
    // security.cpp).
    template <class T, std::size_t... Ns>
    constexpr std::array<T, (Ns + ...)> ByteConcat(const std::array<T, Ns> &...arrays)
    {
        std::array<T, (Ns + ...)> out{};
        std::size_t offset = 0;
        ((std::copy(arrays.begin(), arrays.end(), out.begin() + offset), offset += Ns), ...);
        return out;
    }

    // CSPRNG bytes.
    template <std::size_t N>
    std::array<std::byte, N> CsprngBytes()
    {
        // Enforce N conforming to limits at compile time.
        static_assert(N > 0 && N <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
                      "N too large for RAND_bytes");
        std::array<std::byte, N> b;
        RAND_bytes(reinterpret_cast<unsigned char *>(b.data()), static_cast<int>(N));
        return b;
    }

    // OpenSSL EVP digest-context deleter (the only EVP_MD_CTX_free in the codebase).
    struct EvpCtxDeleter
    {
        void operator()(EVP_MD_CTX *c) const noexcept
        {
            if (c)
                EVP_MD_CTX_free(c);
        }
    };
    using EvpCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpCtxDeleter>;

    // ── SHA-256 helper class ───────────────────────────────────────────────────────
    class Sha256
    {
    private:
        EvpCtxPtr ctx_;

    public:
        Sha256();
        void update(const void *p, std::size_t n);
        std::string hex() const;

        static std::array<std::byte, 32> Oneshot(const void *data, std::size_t n);
        template <class T, std::size_t N>
        static std::array<std::byte, 32> Oneshot(const std::array<T, N> &a)
        {
            return Oneshot(a.data(), N * sizeof(T));
        }
    };

    class SoftwareEncryption
    {
    private:
        std::array<std::byte, 32> derive_per_tape_key(std::array<std::byte, 32> master_key,
                                                      std::array<std::byte, 16> volume_uuid,
                                                      std::uint64_t write_generation);
    };
}

#endif // TAPIR_SECURITY_HPP
