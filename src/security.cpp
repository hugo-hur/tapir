#include "security.hpp"
#include "raii.hpp"
#include <openssl/rand.h>

using namespace tapir::security;

tapir::Sha256::Sha256() : ctx_(EVP_MD_CTX_new())
{
    EVP_DigestInit_ex(ctx_.get(), EVP_sha256(), nullptr);
}

void tapir::Sha256::update(const void *p, std::size_t n)
{
    EVP_DigestUpdate(ctx_.get(), p, n);
}

std::string tapir::Sha256::hex() const
{
    EvpCtxPtr dup(EVP_MD_CTX_new()); // copy so the live context stays usable
    EVP_MD_CTX_copy_ex(dup.get(), ctx_.get());
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(dup.get(), md, &len);
    std::string out;
    out.reserve(static_cast<std::size_t>(len) * 2);
    for (unsigned int i = 0; i < len; ++i)
    {
        out += hex_digit(md[i] >> 4);
        out += hex_digit(md[i] & 0x0F);
    }
    return out;
}

std::array<std::byte, 32> tapir::Sha256::Oneshot(const void *data, std::size_t n)
{
    std::array<std::byte, 32> output;

    EvpCtxPtr _context = EVP_MD_CTX_new();
    EVP_DigestInit_ex(_context.get(), EVP_sha256(), nullptr);
    EVP_DigestUpdate(_context.get(), data, n);
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(_context.get(), reinterpret_cast<unsigned char *>(output.data()), &len) != 1)
    {
        throw std::exception(std::string("EVP_DigestFinal_ex exited with error"));
    }
    return output;
}

// Key derivation (software mode): derive a per-tape, per-write subkey from a high-entropy 256-bit master key:
//     K = SHA256(master_key(32B) || volume-uuid(16B) || write-generation(8B BE))
//     Plain SHA-256 is sufficient here — no HMAC/HKDF needed. The master key is already uniform random (OpenSSL
//     CSPRNG, so there is no weak input to "extract"/whiten), and K is used ONLY as internal AES-256-GCM key
//     material — it is never published as an authenticator, so the Merkle-Damgard length-extension weakness of
//     SHA256(secret||data) is not reachable.
//     The field BOUNDARIES must be unambiguous: two different (uuid, write-gen) tuples must never serialise to the
//     same bytes (the hash sees only the concatenation, not the fields). This holds for free here because each
//     field has a fixed, known size (32/16/8 B) — they need NOT be equal length. The only way to break it is to
//     make a field variable-length (e.g. generation as decimal ASCII); then you must delimit or length-prefix it.
//     TRIP-WIRE: if a value derived directly from SHA256(master_key||...) ever gets EXPOSED — a published
//     key-check value, a MAC, an integrity tag like SHA256(K||...) — switch this to HMAC, because length extension
//     becomes reachable at that point. Until then it is not.
//     volume-uuid: a random v4 UUID generated once per tape, stored (plaintext) in the index. Gives cross-tape key
//     separation — the same master key is safe across many tapes because K differs per uuid, so a position-derived
//     nonce + key pair never recurs on another tape.
//     write-generation: monotonic per volume, bumped every write session, covering same-tape rewrites.
//     Both uuid and write-generation are KDF context (not secret), so K can be derived before decrypting the index.
//     (If a passphrase is ever supported, run it through Argon2id to PRODUCE the master key — this construction is
//     only for deriving subkeys from an already-strong key.)
//
// Nonce: do NOT use the tape file number (it repeats across all the blocks of a file => catastrophic GCM nonce
//     reuse). Encrypt in fixed frames of N tape blocks (e.g. 1 MiB) and derive a unique 96-bit nonce per frame from
//     its tape location (absolute frame index). Deterministic => nonces need not be stored; only the 16-byte GCM tag
//     per frame is kept (in the index, or inline). Each frame is far under GCM's ~64 GiB/nonce size limit, and
//     per-frame tags allow incremental authentication for random-access reads. Uniqueness holds because positions are
//     written once per generation and the key changes per generation/volume.

std::array<std::byte, 32> SoftwareEncryption::derive_per_tape_key(std::array<std::byte, 32> master_key, std::array<std::byte, 16> volume_uuid, std::uint64_t write_generation)
{
    return Sha256::Oneshot(ByteConcat(master_key, volume_uuid, Uint64ToBigEndianBytes(write_generation)));
}
