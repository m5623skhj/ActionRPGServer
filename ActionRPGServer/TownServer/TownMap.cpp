#include "TownMap.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    constexpr float PLAYER_HORIZONTAL_RADIUS = 32.0f;
    constexpr float PLAYER_DEPTH_RADIUS = 18.0f;
    constexpr std::size_t MAX_IMAGE_COUNT = 2048;
    constexpr std::size_t MAX_ASSET_PATH_LENGTH = 240;
    constexpr std::size_t MAX_POLYGON_COUNT = 256;
    constexpr std::size_t MAX_VERTICES_PER_POLYGON = 2048;
    constexpr std::size_t MAX_TOTAL_VERTEX_COUNT = 32768;
    constexpr std::size_t PLAYER_FOOTPRINT_SAMPLE_COUNT = 16;

    void ValidateFinite(const float inValue, const char* inField)
    {
        if (!std::isfinite(inValue))
        {
            throw std::runtime_error(std::string("Town map contains a non-finite value: ") + inField);
        }
    }

    TownProtocol::Vector2 ReadPoint(const nlohmann::json& inPoint)
    {
        TownProtocol::Vector2 point{ inPoint.at("x").get<float>(), inPoint.at("y").get<float>() };
        ValidateFinite(point.x, "polygon.x");
        ValidateFinite(point.y, "polygon.y");
        return point;
    }

    std::vector<TownProtocol::Polygon> ReadPolygons(const nlohmann::json& inDocument,
        const char* inField)
    {
        const nlohmann::json& input = inDocument.at(inField);
        if (!input.is_array() || input.size() > MAX_POLYGON_COUNT)
        {
            throw std::runtime_error(std::string("Invalid polygon collection: ") + inField);
        }

        std::vector<TownProtocol::Polygon> polygons;
        polygons.reserve(input.size());
        for (const nlohmann::json& inputPolygon : input)
        {
            if (!inputPolygon.is_array() || inputPolygon.size() < 3
                || inputPolygon.size() > MAX_VERTICES_PER_POLYGON)
            {
                throw std::runtime_error(std::string("Invalid polygon vertex count: ") + inField);
            }

            TownProtocol::Polygon polygon;
            polygon.reserve(inputPolygon.size());
            for (const nlohmann::json& inputPoint : inputPolygon)
            {
                polygon.push_back(ReadPoint(inputPoint));
            }
            polygons.push_back(std::move(polygon));
        }
        return polygons;
    }

    bool IsPointOnSegment(const TownProtocol::Vector2 inPoint, const TownProtocol::Vector2 inStart,
        const TownProtocol::Vector2 inEnd) noexcept
    {
        constexpr float EPSILON = 0.001f;
        const float deltaX = inEnd.x - inStart.x;
        const float deltaY = inEnd.y - inStart.y;
        const float cross = (inPoint.x - inStart.x) * deltaY - (inPoint.y - inStart.y) * deltaX;
        if (std::abs(cross) > EPSILON)
        {
            return false;
        }
        const float dot = (inPoint.x - inStart.x) * deltaX + (inPoint.y - inStart.y) * deltaY;
        const float lengthSquared = deltaX * deltaX + deltaY * deltaY;
        return dot >= -EPSILON && dot <= lengthSquared + EPSILON;
    }

    bool IsPointInPolygon(const TownProtocol::Vector2 inPoint,
        const TownProtocol::Polygon& inPolygon) noexcept
    {
        bool inside = false;
        for (std::size_t current = 0, previous = inPolygon.size() - 1;
            current < inPolygon.size(); previous = current++)
        {
            const TownProtocol::Vector2& start = inPolygon[previous];
            const TownProtocol::Vector2& end = inPolygon[current];
            if (IsPointOnSegment(inPoint, start, end))
            {
                return true;
            }

            if ((start.y > inPoint.y) != (end.y > inPoint.y))
            {
                const float intersectionX = (end.x - start.x) * (inPoint.y - start.y)
                    / (end.y - start.y) + start.x;
                if (inPoint.x < intersectionX)
                {
                    inside = !inside;
                }
            }
        }
        return inside;
    }

    bool IsPointInAnyPolygon(const TownProtocol::Vector2 inPoint,
        const std::vector<TownProtocol::Polygon>& inPolygons) noexcept
    {
        return std::ranges::any_of(inPolygons, [inPoint](const TownProtocol::Polygon& inPolygon)
        {
            return IsPointInPolygon(inPoint, inPolygon);
        });
    }

    bool IsSafeRelativeAsset(const std::string& inAsset)
    {
        // Asset names are UTF-8; checking separator bytes avoids a locale-dependent path conversion.
        if (inAsset.empty() || inAsset.front() == '/' || inAsset.front() == '\\'
            || inAsset.find(':') != std::string::npos || inAsset.find('\0') != std::string::npos)
        {
            return false;
        }

        std::size_t componentStart = 0;
        for (std::size_t index = 0; index <= inAsset.size(); ++index)
        {
            if (index != inAsset.size() && inAsset[index] != '/' && inAsset[index] != '\\')
            {
                continue;
            }
            if (inAsset.compare(componentStart, index - componentStart, "..") == 0)
            {
                return false;
            }
            componentStart = index + 1;
        }
        return true;
    }
}

namespace TownServer::Domain
{
    TownMap TownMap::Load(const std::filesystem::path& inPath)
    {
        std::ifstream stream(inPath);
        if (!stream)
        {
            throw std::runtime_error("Unable to open town map: " + inPath.string());
        }

        nlohmann::json document;
        stream >> document;
        if (document.at("version").get<int>() != 2)
        {
            throw std::runtime_error("Unsupported town map version.");
        }

        TownProtocol::MapInfo info;
        info.mapId = document.at("mapId").get<std::string>();
        info.worldLeft = document.at("world").at("left").get<float>();
        info.worldTop = document.at("world").at("top").get<float>();
        info.worldRight = document.at("world").at("right").get<float>();
        info.worldBottom = document.at("world").at("bottom").get<float>();
        info.spawnX = document.at("spawn").at("x").get<float>();
        info.spawnY = document.at("spawn").at("y").get<float>();
        info.sectorSize = document.at("sectorSize").get<float>();
        info.walkSpeed = document.at("walkSpeed").get<float>();
        info.runSpeed = document.at("runSpeed").get<float>();

        const float values[] = {
            info.worldLeft, info.worldTop, info.worldRight, info.worldBottom,
            info.spawnX, info.spawnY, info.sectorSize, info.walkSpeed, info.runSpeed
        };
        for (const float value : values)
        {
            ValidateFinite(value, "map scalar");
        }
        if (info.mapId.empty() || info.worldRight <= info.worldLeft || info.worldBottom <= info.worldTop
            || info.sectorSize <= 0.0f || info.walkSpeed <= 0.0f || info.runSpeed < info.walkSpeed)
        {
            throw std::runtime_error("Town map scalar values are inconsistent.");
        }

        const nlohmann::json& images = document.at("images");
        if (!images.is_array() || images.size() > MAX_IMAGE_COUNT)
        {
            throw std::runtime_error("Invalid town image collection.");
        }
        info.images.reserve(images.size());
        for (const nlohmann::json& inputImage : images)
        {
            TownProtocol::MapImage image{
                inputImage.at("asset").get<std::string>(),
                inputImage.at("x").get<float>(), inputImage.at("y").get<float>(),
                inputImage.at("width").get<float>(), inputImage.at("height").get<float>()
            };
            ValidateFinite(image.x, "image.x");
            ValidateFinite(image.y, "image.y");
            ValidateFinite(image.width, "image.width");
            ValidateFinite(image.height, "image.height");
            if (!IsSafeRelativeAsset(image.asset) || image.asset.size() > MAX_ASSET_PATH_LENGTH
                || image.width <= 0.0f || image.height <= 0.0f)
            {
                throw std::runtime_error("Invalid town image.");
            }
            info.images.push_back(std::move(image));
        }

        info.walkablePolygons = ReadPolygons(document, "walkablePolygons");
        info.blockedPolygons = ReadPolygons(document, "blockedPolygons");
        if (info.walkablePolygons.empty())
        {
            throw std::runtime_error("Town map requires at least one walkable polygon.");
        }
        std::size_t totalVertexCount{};
        for (const auto* polygons : { &info.walkablePolygons, &info.blockedPolygons })
            for (const TownProtocol::Polygon& polygon : *polygons) totalVertexCount += polygon.size();
        if (totalVertexCount > MAX_TOTAL_VERTEX_COUNT)
        {
            throw std::runtime_error("Town map contains too many polygon vertices.");
        }
        for (const auto* polygons : { &info.walkablePolygons, &info.blockedPolygons })
        {
            for (const TownProtocol::Polygon& polygon : *polygons)
            {
                for (const TownProtocol::Vector2 point : polygon)
                {
                    if (point.x < info.worldLeft || point.x > info.worldRight
                        || point.y < info.worldTop || point.y > info.worldBottom)
                    {
                        throw std::runtime_error("Town polygon lies outside the world bounds.");
                    }
                }
            }
        }

        TownMap result(std::move(info));
        if (!result.IsPositionValid({ result.info.spawnX, result.info.spawnY }))
        {
            throw std::runtime_error("Town spawn is not valid for the player footprint.");
        }
        return result;
    }

    bool TownMap::IsPositionValid(const TownProtocol::Vector2 inPosition) const noexcept
    {
        if (inPosition.x < info.worldLeft || inPosition.x > info.worldRight
            || inPosition.y < info.worldTop || inPosition.y > info.worldBottom)
        {
            return false;
        }

        const auto isPointValid = [this](const TownProtocol::Vector2 inPoint)
        {
            return IsPointInAnyPolygon(inPoint, info.walkablePolygons)
                && !IsPointInAnyPolygon(inPoint, info.blockedPolygons);
        };
        if (!isPointValid(inPosition))
        {
            return false;
        }

        constexpr float TWO_PI = 6.28318530717958647692f;
        for (std::size_t index = 0; index < PLAYER_FOOTPRINT_SAMPLE_COUNT; ++index)
        {
            const float angle = TWO_PI * static_cast<float>(index)
                / static_cast<float>(PLAYER_FOOTPRINT_SAMPLE_COUNT);
            const TownProtocol::Vector2 sample{
                inPosition.x + std::cos(angle) * PLAYER_HORIZONTAL_RADIUS,
                inPosition.y + std::sin(angle) * PLAYER_DEPTH_RADIUS
            };
            if (!isPointValid(sample))
            {
                return false;
            }
        }
        return true;
    }

    TownProtocol::Vector2 TownMap::ConstrainMovement(const TownProtocol::Vector2 inPrevious,
        const TownProtocol::Vector2 inProposed) const noexcept
    {
        if (!IsPositionValid(inPrevious))
        {
            return { info.spawnX, info.spawnY };
        }

        const float deltaX = inProposed.x - inPrevious.x;
        const float deltaY = inProposed.y - inPrevious.y;
        const float distance = std::sqrt(deltaX * deltaX + deltaY * deltaY);
        const int stepCount = std::max(1, static_cast<int>(std::ceil(distance / 4.0f)));
        TownProtocol::Vector2 lastValid = inPrevious;
        for (int step = 1; step <= stepCount; ++step)
        {
            const float ratio = static_cast<float>(step) / static_cast<float>(stepCount);
            TownProtocol::Vector2 candidate{
                inPrevious.x + deltaX * ratio,
                inPrevious.y + deltaY * ratio
            };
            if (IsPositionValid(candidate))
            {
                lastValid = candidate;
                continue;
            }

            TownProtocol::Vector2 low = lastValid;
            TownProtocol::Vector2 high = candidate;
            for (int iteration = 0; iteration < 12; ++iteration)
            {
                const TownProtocol::Vector2 middle{
                    (low.x + high.x) * 0.5f,
                    (low.y + high.y) * 0.5f
                };
                if (IsPositionValid(middle))
                {
                    low = middle;
                }
                else
                {
                    high = middle;
                }
            }
            return low;
        }
        return lastValid;
    }

    const TownProtocol::MapInfo& TownMap::GetInfo() const noexcept
    {
        return info;
    }

    TownMap::TownMap(TownProtocol::MapInfo inInfo)
        : info(std::move(inInfo))
    {
    }
}
