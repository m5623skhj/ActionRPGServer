#pragma once
#include <array>
#include <string>
#include <stdexcept>
#include <openssl/rand.h>

namespace ActionRPG::Items
{
    inline std::string NewIncarnation()
    {
        std::array<unsigned char, 16> bytes{};
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
            throw std::runtime_error("Unable to create item use incarnation.");
        constexpr char HEX[] = "0123456789abcdef";
        std::string result; result.reserve(32);
        for (const auto value : bytes) { result += HEX[value >> 4]; result += HEX[value & 15]; }
        return result;
    }
}
