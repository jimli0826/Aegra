#pragma once

#include <QString>

namespace aegra::desktop {

// Maps stable Service message_code values to qsTrId translation IDs.
// Unknown codes fall back to a generic safe ID; callers must not display Service text.
[[nodiscard]] QString translation_id_for_message_code(const QString& message_code);

// Returns a localized user-facing string for a Service message_code.
[[nodiscard]] QString localize_message_code(const QString& message_code);

} // namespace aegra::desktop
