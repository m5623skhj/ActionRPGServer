#pragma once

#include <cstddef>
#include <cstdint>

namespace TownServer::Network
{
    inline constexpr std::uint16_t DEFAULT_PORT = 7777;
    inline constexpr std::uint16_t DEFAULT_ROOM_CONTROL_PORT = 7780;
    inline constexpr std::size_t DEFAULT_IO_THREAD_COUNT = 4;
    inline constexpr std::size_t MAX_IO_THREAD_COUNT = 64;
}
