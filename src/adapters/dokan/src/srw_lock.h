#pragma once

#include <windows.h>

namespace aegra::adapters::dokan::detail {

class SharedSrwLock final {
  public:
    explicit SharedSrwLock(SRWLOCK& lock) : lock_(lock) {
        AcquireSRWLockShared(&lock_);
    }
    ~SharedSrwLock() { ReleaseSRWLockShared(&lock_); }

    SharedSrwLock(const SharedSrwLock&) = delete;
    SharedSrwLock& operator=(const SharedSrwLock&) = delete;

  private:
    SRWLOCK& lock_;
};

class ExclusiveSrwLock final {
  public:
    explicit ExclusiveSrwLock(SRWLOCK& lock) : lock_(lock) {
        AcquireSRWLockExclusive(&lock_);
    }
    ~ExclusiveSrwLock() { ReleaseSRWLockExclusive(&lock_); }

    ExclusiveSrwLock(const ExclusiveSrwLock&) = delete;
    ExclusiveSrwLock& operator=(const ExclusiveSrwLock&) = delete;

  private:
    SRWLOCK& lock_;
};

} // namespace aegra::adapters::dokan::detail
