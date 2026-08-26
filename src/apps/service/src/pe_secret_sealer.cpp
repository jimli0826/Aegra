#include "pe_secret_sealer.h"

#include "aegra/adapters/crypto_sodium/pe_envelope.h"

#include <sodium.h>

#include <utility>
#include <vector>

namespace aegra::apps::service {

base::Result<application::IPeSecretSealer::Sealed>
PeSecretSealer::seal(const std::string_view password, const std::string_view binding) {
    auto key = adapters::crypto_sodium::generate_pe_job_key();
    if (!key) {
        return base::Result<Sealed>::failure(key.error());
    }
    auto envelope = adapters::crypto_sodium::seal_pe_password(password, key.value(), binding);
    if (!envelope) {
        sodium_memzero(key.value().data(), key.value().size());
        return base::Result<Sealed>::failure(envelope.error());
    }
    auto digest = adapters::crypto_sodium::pe_binding_digest_hex(binding);
    if (!digest) {
        sodium_memzero(key.value().data(), key.value().size());
        return base::Result<Sealed>::failure(digest.error());
    }
    Sealed sealed;
    sealed.envelope.mode = contracts::PeEnvelopeMode::kSealed;
    sealed.envelope.nonce.assign(envelope.value().nonce.begin(), envelope.value().nonce.end());
    sealed.envelope.ciphertext_with_tag = std::move(envelope).value().ciphertext_with_tag;
    sealed.envelope.binding_digest_hex = std::move(digest).value();
    sealed.job_key = std::move(key).value();
    return base::Result<Sealed>::success(std::move(sealed));
}

} // namespace aegra::apps::service
