#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace GameEngine
{

/// An owned run of bytes of a fixed size for a read to fill: unlike `std::vector<uint8>(n)`, it does not
/// set the bytes first, so the owner writes each byte before it reads it. It converts to
/// `std::span<const uint8>` implicitly. Move-only; moving leaves the source empty.
class ByteBuffer
{
  public:
    ByteBuffer() noexcept = default;

    /// Allocates `size` bytes and does not set them.
    explicit ByteBuffer(std::size_t size)
        : m_Bytes(std::make_unique_for_overwrite<std::uint8_t[]>(size))
        , m_Size(size)
    {
    }

    ByteBuffer(ByteBuffer&& other) noexcept
        : m_Bytes(std::move(other.m_Bytes))
        , m_Size(std::exchange(other.m_Size, 0))
    {
    }

    ByteBuffer& operator=(ByteBuffer&& other) noexcept
    {
        m_Bytes = std::move(other.m_Bytes);
        m_Size = std::exchange(other.m_Size, 0);
        return *this;
    }

    std::uint8_t* Data() noexcept { return m_Bytes.get(); }
    std::size_t Size() const noexcept { return m_Size; }

    std::uint8_t* begin() noexcept { return m_Bytes.get(); }
    std::uint8_t* end() noexcept { return m_Bytes.get() + m_Size; }
    const std::uint8_t* begin() const noexcept { return m_Bytes.get(); }
    const std::uint8_t* end() const noexcept { return m_Bytes.get() + m_Size; }

  private:
    std::unique_ptr<std::uint8_t[]> m_Bytes;
    std::size_t m_Size = 0;
};

} // namespace GameEngine
