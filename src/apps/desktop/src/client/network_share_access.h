#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace aegra::desktop {

/// Connect to an SMB/UNC share (WNetAddConnection2). Temporary connection for this process.
/// Returns { "ok": bool, "errorCode": int, "errorText": QString }.
[[nodiscard]] QVariantMap connect_network_share(const QString& unc_path, const QString& username,
                                                const QString& password, const QString& domain);

/// List immediate subdirectories under a connected UNC path (or share root).
[[nodiscard]] QStringList list_network_share_folders(const QString& unc_path);

/// True when path looks like \\server\... (UNC).
[[nodiscard]] bool is_unc_path(const QString& path);

/// Extract \\server\share from a full UNC path (empty if invalid).
[[nodiscard]] QString extract_share_root(const QString& unc_path);

} // namespace aegra::desktop
