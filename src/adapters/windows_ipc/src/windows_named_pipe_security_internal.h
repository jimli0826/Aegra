#pragma once

#include "aegra/adapters/windows_ipc/windows_named_pipe_security.h"
#include "aegra/base/result.h"

#include <Windows.h>

#include <utility>

namespace aegra::adapters::windows_ipc {
namespace detail {

class UniqueLocal final {
  public:
    explicit UniqueLocal(void* pointer = nullptr) noexcept : pointer_(pointer) {}
    ~UniqueLocal() {
        if (pointer_ != nullptr) {
            LocalFree(pointer_);
        }
    }

    UniqueLocal(const UniqueLocal&) = delete;
    UniqueLocal& operator=(const UniqueLocal&) = delete;
    UniqueLocal(UniqueLocal&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}
    UniqueLocal& operator=(UniqueLocal&& other) noexcept {
        if (this != &other) {
            if (pointer_ != nullptr) {
                LocalFree(pointer_);
            }
            pointer_ = std::exchange(other.pointer_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] void* get() const noexcept { return pointer_; }

  private:
    void* pointer_{nullptr};
};

[[nodiscard]] base::Result<SECURITY_ATTRIBUTES*>
create_local_everyone_security_attributes(SECURITY_ATTRIBUTES& attributes,
                                          UniqueLocal& descriptor_owner);

[[nodiscard]] base::Result<WindowsNamedPipePeerIdentity> query_pipe_peer(HANDLE pipe);

} // namespace detail
} // namespace aegra::adapters::windows_ipc
