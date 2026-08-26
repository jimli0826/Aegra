#pragma once

#include "aegra/application/pe_restore_prepare_service.h"

namespace aegra::apps::service {

/// Composition-root implementation of the application's secret sealer over
/// crypto_sodium::pe_envelope (ADR-0026 B). Generates a fresh 256-bit job key,
/// seals the password bound to the job binding, and fills the informational
/// binding digest. Keeps the application layer free of the crypto adapter.
class PeSecretSealer final : public application::IPeSecretSealer {
  public:
    [[nodiscard]] base::Result<Sealed> seal(std::string_view password,
                                            std::string_view binding) override;
};

} // namespace aegra::apps::service
