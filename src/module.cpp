#include "xqvm/module.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <string>

namespace xqvm {
namespace {

constexpr std::array<Byte, 4> kMagic{'X', 'Q', 'V', 'M'};
constexpr std::size_t kChecksumOffset = 40;

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

[[nodiscard]] Result<void> validate_module_shape(const Module& module, const ModuleLimits& limits) {
    if (module.code.empty()) {
        return make_error(ErrorCode::InvalidModule, 0, "module has no physical islands");
    }
    if (module.carrier_count == 0 || module.initial_carriers.size() != module.carrier_count) {
        return make_error(ErrorCode::InvalidModule, module.carrier_count,
                          "carrier count does not match initial carrier state");
    }
    if (module.data.size() > module.memory_size) {
        return make_error(ErrorCode::InvalidModule, 0, "initial data exceeds VM memory");
    }
    if (module.argument_count > kMaximumArguments) {
        return make_error(ErrorCode::InvalidModule, module.argument_count,
                          "module exceeds the physical argument carrier window");
    }
    if (module.memory_size > limits.max_memory_size || module.carrier_count > limits.max_carriers) {
        return make_error(ErrorCode::InvalidModule, 0, "VM state exceeds configured limits");
    }
    if (module.code.size() > limits.max_code_size || module.data.size() > limits.max_data_size) {
        return make_error(ErrorCode::InvalidModule, 0, "module section exceeds configured limit");
    }
    if (module.code.size() > std::numeric_limits<std::uint32_t>::max() ||
        module.data.size() > std::numeric_limits<std::uint32_t>::max() ||
        module.initial_carriers.size() > std::numeric_limits<std::uint32_t>::max() / sizeof(Word)) {
        return make_error(ErrorCode::InvalidModule, 0, "module section exceeds format size");
    }
    const std::size_t carrier_bytes = module.initial_carriers.size() * sizeof(Word);
    if (module.code.size() > std::numeric_limits<std::size_t>::max() - kModuleHeaderSize ||
        module.data.size() >
            std::numeric_limits<std::size_t>::max() - kModuleHeaderSize - module.code.size() ||
        carrier_bytes > std::numeric_limits<std::size_t>::max() - kModuleHeaderSize -
                            module.code.size() - module.data.size()) {
        return make_error(ErrorCode::InvalidModule, 0, "module size overflows host size_t");
    }
    const std::size_t total =
        kModuleHeaderSize + module.code.size() + module.data.size() + carrier_bytes;
    if (total > limits.max_file_size) {
        return make_error(ErrorCode::InvalidModule, 0, "module exceeds configured file limit");
    }
    return {};
}

} // namespace

Result<void> validate_module(const Module& module, const ModuleLimits& limits) {
    return validate_module_shape(module, limits);
}

Result<std::vector<Byte>> serialize_module(const Module& module, const ModuleLimits& limits) {
    if (auto valid = validate_module_shape(module, limits); !valid) {
        return valid.error();
    }
    const std::size_t carrier_bytes = module.initial_carriers.size() * sizeof(Word);
    const std::size_t total =
        kModuleHeaderSize + module.code.size() + module.data.size() + carrier_bytes;
    std::vector<Byte> bytes(total, 0);
    std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
    write_u16(bytes, 4, kModuleVersion);
    write_u16(bytes, 6, static_cast<std::uint16_t>(kModuleHeaderSize));
    write_u32(bytes, 8, 0);
    write_u32(bytes, 12, module.memory_size);
    write_u32(bytes, 16, module.carrier_count);
    write_u32(bytes, 20, static_cast<std::uint32_t>(module.code.size()));
    write_u32(bytes, 24, static_cast<std::uint32_t>(module.data.size()));
    write_u32(bytes, 28, static_cast<std::uint32_t>(carrier_bytes));
    write_u64(bytes, 32, module.profile_fingerprint);
    write_u16(bytes, 44, module.argument_count);

    std::size_t cursor = kModuleHeaderSize;
    std::copy(module.code.begin(), module.code.end(), bytes.data() + cursor);
    cursor += module.code.size();
    std::copy(module.data.begin(), module.data.end(), bytes.data() + cursor);
    cursor += module.data.size();
    for (Word carrier : module.initial_carriers) {
        write_u64(bytes, cursor, carrier);
        cursor += sizeof(Word);
    }

    write_u32(bytes, kChecksumOffset, 0);
    write_u32(bytes, kChecksumOffset, crc32(bytes));
    return bytes;
}

Result<Module> parse_module(std::span<const Byte> bytes, const ModuleLimits& limits) {
    if (bytes.size() < kModuleHeaderSize) {
        return make_error(ErrorCode::InvalidModule, bytes.size(), "truncated module header");
    }
    if (bytes.size() > limits.max_file_size) {
        return make_error(ErrorCode::InvalidModule, 0, "module exceeds configured file limit");
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return make_error(ErrorCode::InvalidModule, 0, "bad module magic");
    }
    if (read_u16(bytes, 4) != kModuleVersion || read_u16(bytes, 6) != kModuleHeaderSize) {
        return make_error(ErrorCode::InvalidModule, 4, "unsupported module format");
    }
    if (read_u32(bytes, 8) != 0) {
        return make_error(ErrorCode::InvalidModule, 8, "module uses unknown flags");
    }
    for (std::size_t offset = 46; offset < kModuleHeaderSize; ++offset) {
        if (bytes[offset] != 0) {
            return make_error(ErrorCode::InvalidModule, offset, "reserved header byte is not zero");
        }
    }

    const std::uint32_t stored_checksum = read_u32(bytes, kChecksumOffset);
    std::vector<Byte> checksum_input(bytes.begin(), bytes.end());
    write_u32(checksum_input, kChecksumOffset, 0);
    if (crc32(checksum_input) != stored_checksum) {
        return make_error(ErrorCode::ChecksumMismatch, kChecksumOffset, "module CRC32 mismatch");
    }

    const std::uint32_t memory_size = read_u32(bytes, 12);
    const std::uint32_t carrier_count = read_u32(bytes, 16);
    const std::uint32_t code_size = read_u32(bytes, 20);
    const std::uint32_t data_size = read_u32(bytes, 24);
    const std::uint32_t carrier_bytes = read_u32(bytes, 28);
    if (memory_size > limits.max_memory_size || carrier_count > limits.max_carriers) {
        return make_error(ErrorCode::InvalidModule, 12, "VM state exceeds configured limits");
    }
    if (code_size > limits.max_code_size || data_size > limits.max_data_size) {
        return make_error(ErrorCode::InvalidModule, 20, "module section exceeds configured limit");
    }
    if (carrier_bytes != static_cast<std::uint64_t>(carrier_count) * sizeof(Word)) {
        return make_error(ErrorCode::InvalidModule, 28, "invalid initial carrier section size");
    }
    const std::size_t expected = kModuleHeaderSize + static_cast<std::size_t>(code_size) +
                                 static_cast<std::size_t>(data_size) +
                                 static_cast<std::size_t>(carrier_bytes);
    if (expected != bytes.size()) {
        return make_error(ErrorCode::InvalidModule, 20, "section sizes do not match file size");
    }

    Module module;
    module.memory_size = memory_size;
    module.carrier_count = carrier_count;
    module.argument_count = read_u16(bytes, 44);
    module.profile_fingerprint = read_u64(bytes, 32);
    std::size_t cursor = kModuleHeaderSize;
    module.code.assign(bytes.data() + cursor, bytes.data() + cursor + code_size);
    cursor += code_size;
    module.data.assign(bytes.data() + cursor, bytes.data() + cursor + data_size);
    cursor += data_size;
    module.initial_carriers.reserve(carrier_count);
    for (std::uint32_t index = 0; index < carrier_count; ++index) {
        module.initial_carriers.push_back(read_u64(bytes, cursor));
        cursor += sizeof(Word);
    }
    if (auto valid = validate_module_shape(module, limits); !valid) {
        return valid.error();
    }
    return module;
}

Result<void> save_module(const Module& module, const std::filesystem::path& path) {
    auto serialized = serialize_module(module);
    if (!serialized)
        return serialized.error();

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return make_error(ErrorCode::Io, 0, "cannot open output module: " + path.string());
    }
    output.write(reinterpret_cast<const char*>(serialized.value().data()),
                 static_cast<std::streamsize>(serialized.value().size()));
    output.close();
    if (!output) {
        return make_error(ErrorCode::Io, 0, "cannot write module: " + path.string());
    }
    return {};
}

Result<Module> load_module(const std::filesystem::path& path, const ModuleLimits& limits) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return make_error(ErrorCode::Io, 0, "cannot open module: " + path.string());
    }
    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    if (end < 0) {
        return make_error(ErrorCode::Io, 0, "cannot determine module size: " + path.string());
    }
    if (static_cast<std::uintmax_t>(end) > limits.max_file_size) {
        return make_error(ErrorCode::InvalidModule, 0, "module exceeds configured file limit");
    }
    input.seekg(0, std::ios::beg);
    std::vector<Byte> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input && !bytes.empty()) {
        return make_error(ErrorCode::Io, 0, "cannot read module: " + path.string());
    }
    return parse_module(bytes, limits);
}

} // namespace xqvm
