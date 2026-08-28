#ifndef XQVM_PHYSICAL_HPP
#define XQVM_PHYSICAL_HPP

#include "xqvm/result.hpp"
#include "xqvm/schema.hpp"

#include <array>
#include <span>
#include <string_view>

namespace xqvm {

enum class ValueDomain : Byte {
    Integer,
    Address,
    Boolean,
    Semantic,
    History,
    ContinuationLane,
    ContinuationProof,
};

[[nodiscard]] constexpr std::string_view domain_name(ValueDomain domain) noexcept {
    switch (domain) {
    case ValueDomain::Integer:
        return "integer";
    case ValueDomain::Address:
        return "address";
    case ValueDomain::Boolean:
        return "boolean";
    case ValueDomain::Semantic:
        return "semantic";
    case ValueDomain::History:
        return "history";
    case ValueDomain::ContinuationLane:
        return "continuation.lane";
    case ValueDomain::ContinuationProof:
        return "continuation.proof";
    }
    return "invalid";
}

enum class ShareFamily : Byte {
    Xor,
    Additive,
};

struct AffineMap {
    Word multiplier{1};
    Word addend{};

    [[nodiscard]] Word apply(Word value) const noexcept;
    [[nodiscard]] AffineMap inverse() const noexcept;
    [[nodiscard]] AffineMap compose(const AffineMap& inner) const noexcept;

    [[nodiscard]] friend constexpr bool operator==(const AffineMap&,
                                                   const AffineMap&) noexcept = default;
};

struct PhysicalShare {
    CarrierId carrier{};
    AffineMap storage{};

    [[nodiscard]] friend constexpr bool operator==(const PhysicalShare&,
                                                   const PhysicalShare&) noexcept = default;
};

struct PhysicalValue {
    ValueDomain domain{ValueDomain::Integer};
    ShareFamily family{ShareFamily::Xor};
    OriginId origin{};
    std::array<PhysicalShare, 3> shares{};

    [[nodiscard]] friend constexpr bool operator==(const PhysicalValue&,
                                                   const PhysicalValue&) noexcept = default;
};

[[nodiscard]] Word mix_word(Word left, Word right, Word salt) noexcept;
[[nodiscard]] std::array<Word, 3> split_value(ShareFamily family, Word value, Word first,
                                              Word second) noexcept;
[[nodiscard]] Word join_shares(ShareFamily family, const std::array<Word, 3>& shares) noexcept;
[[nodiscard]] Result<Word> read_physical_value(const PhysicalValue& value,
                                               std::span<const Word> carriers);
[[nodiscard]] Result<void> write_physical_value(const PhysicalValue& value, Word logical,
                                                Word first, Word second, std::span<Word> carriers);

} // namespace xqvm

#endif // XQVM_PHYSICAL_HPP
