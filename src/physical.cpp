#include "xqvm/physical.hpp"

#include <cstddef>

namespace xqvm {
namespace {

[[nodiscard]] Word inverse_odd(Word value) noexcept {
    Word inverse = value;
    for (unsigned iteration = 0; iteration < 6; ++iteration) {
        inverse *= 2U - value * inverse;
    }
    return inverse;
}

[[nodiscard]] Result<void> validate_physical_layout(const PhysicalValue& value,
                                                    std::size_t carrier_count) {
    if (static_cast<Byte>(value.domain) > static_cast<Byte>(ValueDomain::ContinuationProof)) {
        return make_error(ErrorCode::InvalidArgument, static_cast<Byte>(value.domain),
                          "physical value uses an invalid domain");
    }
    if (static_cast<Byte>(value.family) > static_cast<Byte>(ShareFamily::Additive)) {
        return make_error(ErrorCode::InvalidArgument, static_cast<Byte>(value.family),
                          "physical value uses an invalid share family");
    }
    for (std::size_t index = 0; index < value.shares.size(); ++index) {
        const PhysicalShare& share = value.shares[index];
        if (share.carrier >= carrier_count) {
            return make_error(ErrorCode::InvalidArgument, share.carrier,
                              "physical share references a missing carrier");
        }
        if ((share.storage.multiplier & 1U) == 0) {
            return make_error(ErrorCode::InvalidArgument, share.carrier,
                              "physical share uses a non-invertible affine map");
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (value.shares[prior].carrier == share.carrier) {
                return make_error(ErrorCode::InvalidArgument, share.carrier,
                                  "physical value reuses a carrier");
            }
        }
    }
    return {};
}

} // namespace

Word AffineMap::apply(Word value) const noexcept {
    return multiplier * value + addend;
}

AffineMap AffineMap::inverse() const noexcept {
    const Word inverse_multiplier = inverse_odd(multiplier);
    return AffineMap{inverse_multiplier, Word{0} - inverse_multiplier * addend};
}

AffineMap AffineMap::compose(const AffineMap& inner) const noexcept {
    return AffineMap{
        multiplier * inner.multiplier,
        multiplier * inner.addend + addend,
    };
}

// The positional order is part of the non-commutative mixing primitive.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Word mix_word(Word left, Word right, Word salt) noexcept {
    Word value = left + 0x9E3779B97F4A7C15ULL + salt;
    value ^= right + 0xD1B54A32D192ED03ULL + (value << 7U) + (value >> 3U);
    value ^= value >> 30U;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27U;
    value *= 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

std::array<Word, 3> split_value(ShareFamily family, Word value, Word first, Word second) noexcept {
    if (family == ShareFamily::Xor) {
        return {first, second, value ^ first ^ second};
    }
    return {first, second, value - first - second};
}

Word join_shares(ShareFamily family, const std::array<Word, 3>& shares) noexcept {
    if (family == ShareFamily::Xor) {
        return shares[0] ^ shares[1] ^ shares[2];
    }
    return shares[0] + shares[1] + shares[2];
}

Result<Word> read_physical_value(const PhysicalValue& value, std::span<const Word> carriers) {
    if (auto valid = validate_physical_layout(value, carriers.size()); !valid)
        return valid.error();
    std::array<Word, 3> logical{};
    for (std::size_t index = 0; index < value.shares.size(); ++index) {
        const PhysicalShare& share = value.shares[index];
        logical[index] = share.storage.inverse().apply(carriers[share.carrier]);
    }
    return join_shares(value.family, logical);
}

Result<void> write_physical_value(const PhysicalValue& value, Word logical, Word first, Word second,
                                  std::span<Word> carriers) {
    if (auto valid = validate_physical_layout(value, carriers.size()); !valid)
        return valid.error();
    const auto shares = split_value(value.family, logical, first, second);
    for (std::size_t index = 0; index < value.shares.size(); ++index) {
        const PhysicalShare& share = value.shares[index];
        carriers[share.carrier] = share.storage.apply(shares[index]);
    }
    return {};
}

} // namespace xqvm
