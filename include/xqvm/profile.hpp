#ifndef XQVM_PROFILE_HPP
#define XQVM_PROFILE_HPP

#include "xqvm/result.hpp"
#include "xqvm/schema.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace xqvm {

struct Continuation {
    Word lane{};
    Word proof{};

    [[nodiscard]] friend constexpr bool operator==(const Continuation&,
                                                   const Continuation&) noexcept = default;
};

class ContinuationCodec {
  public:
    explicit ContinuationCodec(std::uint64_t key) noexcept : key_(key) {}

    [[nodiscard]] Continuation seal(std::uint32_t island_offset) const noexcept;
    [[nodiscard]] Result<std::uint32_t> open(Continuation continuation) const;

  private:
    std::uint64_t key_{};
};

class PrimitiveEncoding {
  public:
    PrimitiveEncoding(const PrimitiveEncoding&) = default;
    PrimitiveEncoding(PrimitiveEncoding&&) noexcept = default;
    PrimitiveEncoding& operator=(const PrimitiveEncoding&) = default;
    PrimitiveEncoding& operator=(PrimitiveEncoding&&) noexcept = default;

    [[nodiscard]] static PrimitiveEncoding shuffled(std::uint64_t seed) noexcept;

    [[nodiscard]] Byte encode(PrimitiveKind kind) const noexcept;
    [[nodiscard]] Result<PrimitiveKind> decode(Byte encoded) const;
    [[nodiscard]] const std::array<Byte, kPrimitiveSchemaCount>& table() const noexcept {
        return encoded_;
    }

  private:
    friend class EncodingProfile;

    PrimitiveEncoding() = default;

    std::array<Byte, kPrimitiveSchemaCount> encoded_{};
};

class EncodingProfile {
  public:
    EncodingProfile(const EncodingProfile&) = default;
    EncodingProfile(EncodingProfile&&) noexcept = default;
    EncodingProfile& operator=(const EncodingProfile&) = default;
    EncodingProfile& operator=(EncodingProfile&&) noexcept = default;

    [[nodiscard]] static EncodingProfile from_seed(std::uint64_t seed) noexcept;

    [[nodiscard]] std::uint64_t seed() const noexcept {
        return seed_;
    }
    [[nodiscard]] std::uint64_t fingerprint() const noexcept {
        return fingerprint_;
    }
    [[nodiscard]] const PrimitiveEncoding& primitives() const noexcept {
        return primitives_;
    }
    [[nodiscard]] ContinuationCodec continuations() const noexcept {
        return ContinuationCodec(continuation_key_);
    }
    [[nodiscard]] Byte mask_code_byte(Byte plain, std::size_t position) const noexcept;

  private:
    EncodingProfile() = default;

    std::uint64_t seed_{};
    std::uint64_t fingerprint_{};
    std::uint64_t code_key_{};
    std::uint64_t continuation_key_{};
    PrimitiveEncoding primitives_{};
};

[[nodiscard]] Result<void> save_profile(const EncodingProfile& profile,
                                        const std::filesystem::path& path);
[[nodiscard]] Result<std::vector<Byte>> serialize_profile(const EncodingProfile& profile);
[[nodiscard]] Result<EncodingProfile> parse_profile(std::span<const Byte> bytes);
[[nodiscard]] Result<EncodingProfile> load_profile(const std::filesystem::path& path);

} // namespace xqvm

#endif // XQVM_PROFILE_HPP
