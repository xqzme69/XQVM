#include "xqvm/profile.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <numeric>
#include <vector>

namespace xqvm {
namespace {

constexpr std::array<Byte, 4> kProfileMagic{'X', 'Q', 'P', 'F'};
constexpr std::uint16_t kProfileVersion = 1;
constexpr std::size_t kProfileSize = 32;
constexpr std::size_t kProfileChecksumOffset = 24;

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

// Both keys intentionally occupy fixed, distinct positions in the fingerprint.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] std::uint64_t profile_fingerprint(const PrimitiveEncoding& primitives,
                                                std::uint64_t code_key,
                                                std::uint64_t continuation_key) noexcept {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (Byte encoded : primitives.table()) {
        hash = (hash ^ encoded) * 0x100000001B3ULL;
    }
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        hash = (hash ^ static_cast<Byte>(code_key >> shift)) * 0x100000001B3ULL;
        hash = (hash ^ static_cast<Byte>(continuation_key >> shift)) * 0x100000001B3ULL;
    }
    return mix64(hash);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

void write_u16(std::vector<Byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<Byte>(value);
    bytes[offset + 1U] = static_cast<Byte>(value >> 8U);
}

void write_u32(std::vector<Byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        bytes[offset + shift / 8U] = static_cast<Byte>(value >> shift);
    }
}

void write_u64(std::vector<Byte>& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        bytes[offset + shift / 8U] = static_cast<Byte>(value >> shift);
    }
}

[[nodiscard]] std::uint16_t read_u16(std::span<const Byte> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

[[nodiscard]] std::uint32_t read_u32(std::span<const Byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(bytes[offset + shift / 8U]) << shift;
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64(std::span<const Byte> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(bytes[offset + shift / 8U]) << shift;
    }
    return value;
}

[[nodiscard]] std::uint32_t crc32(std::span<const Byte> bytes) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (Byte byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

} // namespace

Continuation ContinuationCodec::seal(std::uint32_t island_offset) const noexcept {
    const Word mask = mix64(key_ ^ 0x434F4E54494E5545ULL);
    const Word lane = static_cast<Word>(island_offset) ^ mask;
    const Word proof =
        mix64(key_ ^ (static_cast<Word>(island_offset) << 32U) ^ lane ^ 0x50524F4F4656514DULL);
    return Continuation{lane, proof};
}

Result<std::uint32_t> ContinuationCodec::open(Continuation continuation) const {
    const Word mask = mix64(key_ ^ 0x434F4E54494E5545ULL);
    const Word decoded = continuation.lane ^ mask;
    if (decoded > std::numeric_limits<std::uint32_t>::max()) {
        return make_error(ErrorCode::Decode, 0, "continuation lane is outside offset domain");
    }
    const auto island_offset = static_cast<std::uint32_t>(decoded);
    if (seal(island_offset) != continuation) {
        return make_error(ErrorCode::Decode, island_offset, "continuation proof mismatch");
    }
    return island_offset;
}

PrimitiveEncoding PrimitiveEncoding::shuffled(std::uint64_t seed) noexcept {
    std::array<Byte, 256> candidates{};
    std::iota(candidates.begin(), candidates.end(), Byte{0});
    std::uint64_t state = seed;
    for (std::size_t index = candidates.size() - 1; index > 0; --index) {
        state = mix64(state);
        const std::size_t selected = static_cast<std::size_t>(state % (index + 1U));
        std::swap(candidates[index], candidates[selected]);
    }

    PrimitiveEncoding encoding;
    std::copy_n(candidates.begin(), encoding.encoded_.size(), encoding.encoded_.begin());
    return encoding;
}

Byte PrimitiveEncoding::encode(PrimitiveKind kind) const noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < encoded_.size() ? encoded_[index] : Byte{0};
}

Result<PrimitiveKind> PrimitiveEncoding::decode(Byte encoded) const {
    const auto found = std::find(encoded_.begin(), encoded_.end(), encoded);
    if (found == encoded_.end()) {
        return make_error(ErrorCode::Decode, encoded, "unknown physical primitive code");
    }
    return static_cast<PrimitiveKind>(std::distance(encoded_.begin(), found));
}

EncodingProfile EncodingProfile::from_seed(std::uint64_t seed) noexcept {
    EncodingProfile profile;
    profile.seed_ = seed;
    profile.primitives_ = PrimitiveEncoding::shuffled(mix64(seed ^ 0x5052494D49544956ULL));
    profile.code_key_ = mix64(seed ^ 0x434F44454B4559ULL);
    profile.continuation_key_ = mix64(seed ^ 0x434F4E544B4559ULL);
    profile.fingerprint_ =
        profile_fingerprint(profile.primitives_, profile.code_key_, profile.continuation_key_);
    return profile;
}

Byte EncodingProfile::mask_code_byte(Byte plain, std::size_t position) const noexcept {
    const Word stream = mix64(code_key_ ^ static_cast<Word>(position) * 0xD6E8FEB86659FD93ULL ^
                              (static_cast<Word>(position) << 32U));
    return static_cast<Byte>(plain ^ static_cast<Byte>(stream >> ((position & 7U) * 8U)));
}

Result<std::vector<Byte>> serialize_profile(const EncodingProfile& profile) {
    std::vector<Byte> bytes(kProfileSize, 0);
    std::copy(kProfileMagic.begin(), kProfileMagic.end(), bytes.begin());
    write_u16(bytes, 4, kProfileVersion);
    write_u16(bytes, 6, static_cast<std::uint16_t>(kProfileSize));
    write_u64(bytes, 8, profile.seed());
    write_u64(bytes, 16, profile.fingerprint());
    write_u32(bytes, kProfileChecksumOffset, 0);
    write_u32(bytes, kProfileChecksumOffset, crc32(bytes));
    return bytes;
}

Result<void> save_profile(const EncodingProfile& profile, const std::filesystem::path& path) {
    auto serialized = serialize_profile(profile);
    if (!serialized)
        return serialized.error();

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return make_error(ErrorCode::Io, 0, "cannot open profile output: " + path.string());
    }
    output.write(reinterpret_cast<const char*>(serialized.value().data()),
                 static_cast<std::streamsize>(serialized.value().size()));
    output.close();
    if (!output) {
        return make_error(ErrorCode::Io, 0, "cannot write profile: " + path.string());
    }
    return {};
}

Result<EncodingProfile> parse_profile(std::span<const Byte> bytes) {
    if (bytes.size() != kProfileSize) {
        return make_error(ErrorCode::InvalidModule, bytes.size(), "unexpected profile size");
    }
    if (!std::equal(kProfileMagic.begin(), kProfileMagic.end(), bytes.begin())) {
        return make_error(ErrorCode::InvalidModule, 0, "bad profile magic");
    }
    if (read_u16(bytes, 4) != kProfileVersion || read_u16(bytes, 6) != kProfileSize) {
        return make_error(ErrorCode::InvalidModule, 4, "unsupported profile version");
    }
    if (!std::all_of(bytes.begin() + 28, bytes.end(), [](Byte byte) { return byte == 0; })) {
        return make_error(ErrorCode::InvalidModule, 28, "profile reserved bytes must be zero");
    }
    const std::uint32_t checksum = read_u32(bytes, kProfileChecksumOffset);
    std::vector<Byte> checksum_bytes(bytes.begin(), bytes.end());
    write_u32(checksum_bytes, kProfileChecksumOffset, 0);
    if (crc32(checksum_bytes) != checksum) {
        return make_error(ErrorCode::ChecksumMismatch, kProfileChecksumOffset,
                          "profile CRC32 mismatch");
    }

    const EncodingProfile profile = EncodingProfile::from_seed(read_u64(bytes, 8));
    if (profile.fingerprint() != read_u64(bytes, 16)) {
        return make_error(ErrorCode::InvalidModule, 16, "profile fingerprint mismatch");
    }
    return profile;
}

Result<EncodingProfile> load_profile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return make_error(ErrorCode::Io, 0, "cannot open profile: " + path.string());
    }
    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    if (end < 0) {
        return make_error(ErrorCode::Io, 0, "cannot determine profile size: " + path.string());
    }
    if (end != static_cast<std::streamoff>(kProfileSize)) {
        const std::size_t observed =
            static_cast<std::uintmax_t>(end) <= std::numeric_limits<std::size_t>::max()
                ? static_cast<std::size_t>(end)
                : 0;
        return make_error(ErrorCode::InvalidModule, observed, "unexpected profile size");
    }
    input.seekg(0, std::ios::beg);
    std::array<Byte, kProfileSize> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        return make_error(ErrorCode::Io, 0, "cannot read profile: " + path.string());
    }
    return parse_profile(bytes);
}

} // namespace xqvm
