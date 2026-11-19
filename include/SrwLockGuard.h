#pragma once

#include <Windows.h>

struct SrwExclusiveGuard {
    explicit SrwExclusiveGuard(SRWLOCK* lock) noexcept : m_lock(lock) {
        if (m_lock) AcquireSRWLockExclusive(m_lock);
    }
    ~SrwExclusiveGuard() {
        if (m_lock) ReleaseSRWLockExclusive(m_lock);
    }
    SrwExclusiveGuard(const SrwExclusiveGuard&) = delete;
    SrwExclusiveGuard& operator=(const SrwExclusiveGuard&) = delete;

private:
    SRWLOCK* m_lock;
};

struct SrwSharedGuard {
    explicit SrwSharedGuard(SRWLOCK* lock) noexcept : m_lock(lock) {
        if (m_lock) AcquireSRWLockShared(m_lock);
    }
    ~SrwSharedGuard() {
        if (m_lock) ReleaseSRWLockShared(m_lock);
    }
    SrwSharedGuard(const SrwSharedGuard&) = delete;
    SrwSharedGuard& operator=(const SrwSharedGuard&) = delete;

private:
    SRWLOCK* m_lock;
};
