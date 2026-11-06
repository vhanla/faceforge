#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <new>

// Single-buffer bump allocator. Frame arenas are reset once per frame; state
// arenas persist for the lifetime of the app. No allocation happens in loops.
struct Arena {
    uint8_t* buffer = nullptr;
    size_t capacity = 0;
    size_t used = 0;

    static Arena create(size_t bytes) {
        Arena a;
        a.buffer = static_cast<uint8_t*>(std::malloc(bytes));
        a.capacity = a.buffer ? bytes : 0;
        a.used = 0;
        return a;
    }

    void* alloc(size_t size, size_t alignment = 8) {
        if (!buffer) return nullptr;
        uintptr_t current = reinterpret_cast<uintptr_t>(buffer) + used;
        uintptr_t aligned = (current + (alignment - 1)) & ~(uintptr_t)(alignment - 1);
        size_t offset = static_cast<size_t>(aligned - reinterpret_cast<uintptr_t>(buffer));
        if (offset + size > capacity) return nullptr;
        used = offset + size;
        return buffer + offset;
    }

    template <typename T>
    T* make() {
        void* mem = alloc(sizeof(T), alignof(T));
        if (!mem) return nullptr;
        return new (mem) T{};
    }

    void reset() { used = 0; }

    void destroy() {
        std::free(buffer);
        buffer = nullptr;
        capacity = 0;
        used = 0;
    }
};
