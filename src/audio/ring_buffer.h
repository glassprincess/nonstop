#pragma once

#include <vector>
#include <atomic>
#include <cstdint>
#include <algorithm>

namespace nonstop {

// one writer, one reader, no locks
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity = 16384)
        : m_capacity(capacity)
        , m_buffer(capacity)
        , m_writePos(0)
        , m_readPos(0)
    {}

    void resize(size_t capacity) {
        m_capacity = capacity;
        m_buffer.resize(capacity);
        reset();
    }

    void reset() {
        m_writePos.store(0, std::memory_order_relaxed);
        m_readPos.store(0, std::memory_order_relaxed);
    }

    size_t write(const T* data, size_t count) {
        const size_t currentRead = m_readPos.load(std::memory_order_acquire);
        const size_t currentWrite = m_writePos.load(std::memory_order_relaxed);

        size_t availableSpace = 0;
        if (currentWrite >= currentRead) {
            availableSpace = (m_capacity - currentWrite) + currentRead;
        } else {
            availableSpace = currentRead - currentWrite;
        }

        // one slot stays empty so full and empty look different
        if (availableSpace <= 1) return 0;
        const size_t toWrite = std::min(count, availableSpace - 1);

        const size_t part1 = std::min(toWrite, m_capacity - currentWrite);
        const size_t part2 = toWrite - part1;

        std::copy_n(data, part1, m_buffer.data() + currentWrite);
        if (part2 > 0) {
            std::copy_n(data + part1, part2, m_buffer.data());
        }

        m_writePos.store((currentWrite + toWrite) % m_capacity, std::memory_order_release);
        return toWrite;
    }

    size_t read(T* outData, size_t count) {
        const size_t currentWrite = m_writePos.load(std::memory_order_acquire);
        const size_t currentRead = m_readPos.load(std::memory_order_relaxed);

        size_t availableData = 0;
        if (currentWrite >= currentRead) {
            availableData = currentWrite - currentRead;
        } else {
            availableData = (m_capacity - currentRead) + currentWrite;
        }

        const size_t toRead = std::min(count, availableData);
        if (toRead == 0) return 0;

        const size_t part1 = std::min(toRead, m_capacity - currentRead);
        const size_t part2 = toRead - part1;

        std::copy_n(m_buffer.data() + currentRead, part1, outData);
        if (part2 > 0) {
            std::copy_n(m_buffer.data(), part2, outData + part1);
        }

        m_readPos.store((currentRead + toRead) % m_capacity, std::memory_order_release);
        return toRead;
    }

    size_t availableRead() const {
        const size_t currentWrite = m_writePos.load(std::memory_order_acquire);
        const size_t currentRead = m_readPos.load(std::memory_order_relaxed);
        if (currentWrite >= currentRead) {
            return currentWrite - currentRead;
        }
        return (m_capacity - currentRead) + currentWrite;
    }

private:
    size_t m_capacity;
    std::vector<T> m_buffer;
    std::atomic<size_t> m_writePos;
    std::atomic<size_t> m_readPos;
};

} // namespace nonstop
