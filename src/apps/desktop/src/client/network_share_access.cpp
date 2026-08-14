#include "client/network_share_access.h"

#include <QDir>

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <Windows.h>
#  include <winnetwk.h>
#endif

namespace aegra::desktop {
namespace {

[[nodiscard]] QString win32_error_text(const DWORD code) {
#if defined(Q_OS_WIN)
    wchar_t* buffer = nullptr;
    const auto length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString text;
    if (length > 0 && buffer != nullptr) {
        text = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
        LocalFree(buffer);
    }
    if (text.isEmpty()) {
        text = QStringLiteral("error %1").arg(code);
    }
    return text;
#else
    Q_UNUSED(code);
    return QStringLiteral("network share is not supported on this platform");
#endif
}

} // namespace

bool is_unc_path(const QString& path) {
    const auto trimmed = path.trimmed();
    return trimmed.startsWith(QStringLiteral("\\\\")) && trimmed.size() > 3;
}

QString extract_share_root(const QString& unc_path) {
    auto path = QDir::toNativeSeparators(unc_path.trimmed());
    if (!is_unc_path(path)) {
        return {};
    }
    // \\server\share[\rest...]
    const auto server_end = path.indexOf(QLatin1Char('\\'), 2);
    if (server_end < 0) {
        return {};
    }
    const auto share_end = path.indexOf(QLatin1Char('\\'), server_end + 1);
    if (share_end < 0) {
        return path;
    }
    return path.left(share_end);
}

QVariantMap connect_network_share(const QString& unc_path, const QString& username,
                                  const QString& password, const QString& domain) {
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    result.insert(QStringLiteral("errorCode"), 0);
    result.insert(QStringLiteral("errorText"), QString{});

    const auto share = extract_share_root(unc_path);
    if (share.isEmpty()) {
        result.insert(QStringLiteral("errorText"),
                      QStringLiteral("Invalid network path (expected \\\\server\\share)"));
        return result;
    }

#if defined(Q_OS_WIN)
    const auto w_share = share.toStdWString();
    // username: optional DOMAIN\user form when domain is set separately.
    QString account = username.trimmed();
    const auto domain_trimmed = domain.trimmed();
    if (!domain_trimmed.isEmpty() && !account.isEmpty() &&
        !account.contains(QLatin1Char('\\')) && !account.contains(QLatin1Char('@'))) {
        account = domain_trimmed + QLatin1Char('\\') + account;
    }
    const auto w_user = account.toStdWString();
    const auto w_pass = password.toStdWString();

    NETRESOURCEW resource{};
    resource.dwType = RESOURCETYPE_DISK;
    resource.lpRemoteName = const_cast<wchar_t*>(w_share.c_str());

    auto try_connect = [&]() -> DWORD {
        return WNetAddConnection2W(&resource, w_pass.empty() ? nullptr : w_pass.c_str(),
                                   w_user.empty() ? nullptr : w_user.c_str(), CONNECT_TEMPORARY);
    };

    DWORD status = try_connect();
    if (status == ERROR_SESSION_CREDENTIAL_CONFLICT) {
        // Drop stale credentials for this share and retry once (matches backup share connect).
        WNetCancelConnection2W(w_share.c_str(), 0, TRUE);
        status = try_connect();
    }
    if (status != NO_ERROR && status != ERROR_ALREADY_ASSIGNED &&
        status != ERROR_DEVICE_ALREADY_REMEMBERED) {
        result.insert(QStringLiteral("errorCode"), static_cast<qint64>(status));
        result.insert(QStringLiteral("errorText"), win32_error_text(status));
        return result;
    }
    result.insert(QStringLiteral("ok"), true);
    return result;
#else
    Q_UNUSED(username);
    Q_UNUSED(password);
    Q_UNUSED(domain);
    result.insert(QStringLiteral("errorText"),
                  QStringLiteral("network share is not supported on this platform"));
    return result;
#endif
}

QStringList list_network_share_folders(const QString& unc_path) {
    QStringList folders;
    const auto trimmed = QDir::toNativeSeparators(unc_path.trimmed());
    if (trimmed.isEmpty()) {
        return folders;
    }
    QDir dir(trimmed);
    if (!dir.exists()) {
        return folders;
    }
    const auto entries =
        dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    folders.reserve(entries.size());
    for (const auto& name : entries) {
        folders.push_back(name);
    }
    return folders;
}

} // namespace aegra::desktop
