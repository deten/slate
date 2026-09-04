#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cad {

/// A permanent object identifier.
///
/// Every object in a DWG database carries a permanent 64-bit handle, written in
/// the file as uppercase hexadecimal. Handles are how objects reference one
/// another — an entity names its layer by handle, a layout its block record, an
/// xref its host — and they survive save and reload.
///
/// Handles are the sole object identity in this system, deliberately:
///
///  - Raw pointers would dangle as soon as undo replaces an object with a
///    restored clone.
///  - Array indices are invalidated by every deletion.
///  - Handles round-trip to DWG exactly, so a reference we could not fully
///    interpret on load can still be written back out intact.
///
/// Stored as a 64-bit integer rather than a string: a real drawing performs
/// millions of handle comparisons and map lookups, and integer hashing is far
/// cheaper. The hexadecimal form exists only at the file boundary.
///
/// A distinct type rather than a bare `std::uint64_t` so a handle cannot be
/// silently confused with a count, an index or another kind of id.
class Handle {
public:
    constexpr Handle() noexcept = default;

    explicit constexpr Handle(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

    /// The null handle, used where DWG stores a reference to nothing.
    [[nodiscard]] static constexpr Handle null() noexcept { return Handle{0}; }

    [[nodiscard]] constexpr bool isNull() const noexcept { return value_ == 0; }

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value_ != 0; }

    [[nodiscard]] constexpr bool operator==(const Handle& other) const noexcept = default;
    [[nodiscard]] constexpr auto operator<=>(const Handle& other) const noexcept = default;

    /// Canonical uppercase hexadecimal, as written to DXF.
    [[nodiscard]] std::string toString() const;

    /// Parse a hexadecimal handle.
    /// @throws std::invalid_argument when the text is not valid hexadecimal.
    [[nodiscard]] static Handle parse(std::string_view text);

    /// Parse without throwing; empty for malformed input.
    ///
    /// Used throughout the DXF reader, where a malformed handle in one entity
    /// must not abort the load of an entire professional drawing.
    [[nodiscard]] static std::optional<Handle> tryParse(std::string_view text) noexcept;

private:
    std::uint64_t value_ = 0;
};

/// Allocates fresh handles that cannot collide with any already in a database.
///
/// DWG stores a "next handle" seed in its header. On load the allocator takes
/// the larger of that seed and one past the highest handle actually observed,
/// so a file with an inconsistent seed still cannot produce a collision.
class HandleAllocator {
public:
    explicit constexpr HandleAllocator(std::uint64_t seed = 1) noexcept
        : next_(seed < 1 ? 1 : seed) {}

    /// Allocate the next unused handle.
    [[nodiscard]] constexpr Handle allocate() noexcept { return Handle{next_++}; }

    /// Ensure future allocations come after `handle`. Called for every object
    /// read from a file.
    constexpr void observe(Handle handle) noexcept {
        if (handle.value() >= next_) {
            next_ = handle.value() + 1;
        }
    }

    /// The handle that will be allocated next, for writing the file header.
    [[nodiscard]] constexpr Handle peek() const noexcept { return Handle{next_}; }

private:
    std::uint64_t next_ = 1;
};

}  // namespace cad

template <>
struct std::hash<cad::Handle> {
    [[nodiscard]] std::size_t operator()(const cad::Handle& handle) const noexcept {
        return std::hash<std::uint64_t>{}(handle.value());
    }
};
