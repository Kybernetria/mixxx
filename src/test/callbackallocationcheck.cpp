#include "test/callbackallocationcheck.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace mixxxtest {
thread_local bool countCallbackAllocations = false;
thread_local std::size_t callbackAllocations = 0;
thread_local std::size_t callbackDeallocations = 0;
} // namespace mixxxtest

void* operator new(std::size_t size) {
    if (mixxxtest::countCallbackAllocations)
        ++mixxxtest::callbackAllocations;
    if (void* pointer = std::malloc(size ? size : 1))
        return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}
void operator delete(void* pointer) noexcept {
    if (pointer && mixxxtest::countCallbackAllocations)
        ++mixxxtest::callbackDeallocations;
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
    ::operator delete(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    ::operator delete(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    ::operator delete(pointer);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    ::operator delete(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    ::operator delete(pointer);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    if (mixxxtest::countCallbackAllocations)
        ++mixxxtest::callbackAllocations;
    void* pointer = nullptr;
    // Explicit aligned-new callers (e.g. LLVM) may request alignments smaller
    // than a pointer. posix_memalign additionally requires pointer alignment.
    const auto allocationAlignment = std::max(sizeof(void*),
            static_cast<std::size_t>(alignment));
#ifdef _WIN32
    pointer = _aligned_malloc(size ? size : 1, allocationAlignment);
#else
    if (posix_memalign(&pointer, allocationAlignment, size ? size : 1) != 0)
        pointer = nullptr;
#endif
    if (pointer)
        return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size, alignment);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}
void* operator new[](std::size_t size,
        std::align_val_t alignment,
        const std::nothrow_t& tag) noexcept {
    return ::operator new(size, alignment, tag);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
    if (pointer && mixxxtest::countCallbackAllocations)
        ++mixxxtest::callbackDeallocations;
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
void operator delete[](void* pointer, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}
void operator delete(void* pointer, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(pointer, alignment);
}
void operator delete[](void* pointer, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(pointer, alignment);
}

TEST(CallbackAllocationCheckTest, SmallExplicitAlignments) {
    for (const auto value : {1u, 2u, 4u, 8u}) {
        const auto alignment = static_cast<std::align_val_t>(value);
        void* pointer = ::operator new(512, alignment, std::nothrow);
        ASSERT_NE(nullptr, pointer);
        EXPECT_EQ(0u, reinterpret_cast<std::uintptr_t>(pointer) % value);
        ::operator delete(pointer, alignment, std::nothrow);
    }
}

TEST(CallbackAllocationCheckTest, MatchingNothrowAndAlignedFamilies) {
    constexpr auto alignment = std::align_val_t(64);
    mixxxtest::callbackAllocations = 0;
    mixxxtest::callbackDeallocations = 0;
    mixxxtest::countCallbackAllocations = true;
    void* scalar = ::operator new(8);
    void* array = ::operator new[](8);
    void* nothrowScalar = ::operator new(8, std::nothrow);
    void* nothrowArray = ::operator new[](8, std::nothrow);
    void* alignedScalar = ::operator new(8, alignment);
    void* alignedArray = ::operator new[](8, alignment);
    void* alignedNothrowScalar = ::operator new(8, alignment, std::nothrow);
    void* alignedNothrowArray = ::operator new[](8, alignment, std::nothrow);
    ::operator delete(scalar);
    ::operator delete[](array);
    ::operator delete(nothrowScalar, std::nothrow);
    ::operator delete[](nothrowArray, std::nothrow);
    ::operator delete(alignedScalar, alignment);
    ::operator delete[](alignedArray, alignment);
    ::operator delete(alignedNothrowScalar, alignment, std::nothrow);
    ::operator delete[](alignedNothrowArray, alignment, std::nothrow);
    mixxxtest::countCallbackAllocations = false;
    EXPECT_EQ(8u, mixxxtest::callbackAllocations);
    EXPECT_EQ(8u, mixxxtest::callbackDeallocations);
}
