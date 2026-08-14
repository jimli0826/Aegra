#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/job.h"

#include <string>
#include <string_view>

namespace aegra::apps::service {

/// True for \\server\... UNC paths.
[[nodiscard]] bool is_unc_locator(std::string_view locator) noexcept;

/// \\server\share prefix of a UNC locator (empty if invalid).
[[nodiscard]] std::string extract_share_root(std::string_view locator);

/// Temporary WNet connect (matches backup NetworkStorageBackend::ConnectToShare).
[[nodiscard]] base::Result<void> connect_network_share(std::string_view locator,
                                                       std::string_view username,
                                                       std::string_view password,
                                                       std::string_view domain);

/// Pack network auth for DPAPI blob (magic + user + domain + password).
[[nodiscard]] std::string pack_network_auth_material(std::string_view username,
                                                     std::string_view domain,
                                                     std::string_view password);

/// Unpack pack_network_auth_material; returns false if not network pack format.
[[nodiscard]] bool unpack_network_auth_material(std::string_view material, std::string& username,
                                                std::string& domain, std::string& password);

/// Connect using resolved SecretRef material when it is a network auth pack.
[[nodiscard]] base::Result<void>
connect_network_share_from_secret(std::string_view locator, const contracts::SecretRef& secret_ref);

} // namespace aegra::apps::service
