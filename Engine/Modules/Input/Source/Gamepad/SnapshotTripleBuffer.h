#pragma once

#include <atomic>
#include <cstdint>

namespace GameEngine::Input
{

/**
 * @brief Hands the latest value from one writer thread to one reader thread without a lock or a wait.
 * Each of the three buffers has one owner at a time: the writer, the reader or the published slot.
 * A burst of Publish calls between two Acquire calls delivers only the last value.
 */
template <typename T>
class SnapshotTripleBuffer
{
  public:
    /// The writer's buffer. Writer thread only.
    T& WriteBuffer() { return m_Buffers[m_WriteIndex]; }

    /// Publishes the writer's buffer and gives the writer the previous published buffer.
    void Publish()
    {
        const std::uint8_t previous =
            m_Published.exchange(static_cast<std::uint8_t>(m_WriteIndex | kNewBit), std::memory_order_acq_rel);
        m_WriteIndex = static_cast<std::uint8_t>(previous & kIndexMask);
    }

    /// Takes the published buffer when it is newer than the reader's. Reader thread only.
    /// @return True when ReadBuffer changed.
    bool Acquire()
    {
        if ((m_Published.load(std::memory_order_relaxed) & kNewBit) == 0)
            return false;
        const std::uint8_t previous = m_Published.exchange(m_ReadIndex, std::memory_order_acq_rel);
        m_ReadIndex = static_cast<std::uint8_t>(previous & kIndexMask);
        return true;
    }

    /// The reader's buffer: the latest value Acquire took. Reader thread only.
    const T& ReadBuffer() const { return m_Buffers[m_ReadIndex]; }

    /// Every buffer, for a release of what they hold once both threads have stopped.
    T* AllBuffers() { return m_Buffers; }
    static constexpr int kBufferCount = 3;

  private:
    static constexpr std::uint8_t kIndexMask = 0x3;
    static constexpr std::uint8_t kNewBit = 0x4;

    T m_Buffers[kBufferCount] = {};
    std::uint8_t m_WriteIndex = 0;
    std::uint8_t m_ReadIndex = 1;
    std::atomic<std::uint8_t> m_Published{2};
};

} // namespace GameEngine::Input
