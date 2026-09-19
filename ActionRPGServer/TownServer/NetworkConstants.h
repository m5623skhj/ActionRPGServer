#pragma once

#include <cstddef>
#include <cstdint>

namespace TownServer::Network
{
    inline constexpr std::uint16_t DEFAULT_PORT = 7777;
    inline constexpr std::size_t DEFAULT_IO_THREAD_COUNT = 4;
    inline constexpr std::size_t MAX_IO_THREAD_COUNT = 64;
    inline constexpr std::size_t PACKET_HEADER_SIZE = sizeof(std::uint32_t);
    inline constexpr std::uint32_t MAX_PACKET_BODY_SIZE = 64 * 1024;
    inline constexpr std::size_t MAX_QUEUED_SEND_BYTES = 1024 * 1024;
}
