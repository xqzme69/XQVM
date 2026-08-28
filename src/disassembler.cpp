#include "xqvm/disassembler.hpp"

#include "xqvm/compiler.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>

namespace xqvm {
namespace {

class Reader {
  public:
    Reader(const Module& module, const EncodingProfile& profile, std::size_t position)
        : code_(module.code), profile_(profile), position_(position), limit_(module.code.size()) {}

    [[nodiscard]] std::size_t position() const noexcept {
        return position_;
    }

    [[nodiscard]] Result<void> set_limit(std::size_t limit) {
        if (limit < position_ || limit > code_.size()) {
            return make_error(ErrorCode::Decode, position_, "physical island exceeds code section");
        }
        limit_ = limit;
        return {};
    }

    [[nodiscard]] Result<Byte> byte() {
        if (position_ >= limit_) {
            return make_error(ErrorCode::Decode, position_, "truncated physical island");
        }
        const Byte result = profile_.mask_code_byte(code_[position_], position_);
        ++position_;
        return result;
    }

    [[nodiscard]] Result<std::uint16_t> u16() {
        auto low = byte();
        auto high = byte();
        if (!low)
            return low.error();
        if (!high)
            return high.error();
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(low.value()) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(high.value()) << 8U));
    }

    [[nodiscard]] Result<std::uint32_t> u32() {
        std::uint32_t result = 0;
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            auto value = byte();
            if (!value)
                return value.error();
            result |= static_cast<std::uint32_t>(value.value()) << shift;
        }
        return result;
    }

    [[nodiscard]] Result<std::uint64_t> u64() {
        std::uint64_t result = 0;
        for (unsigned shift = 0; shift < 64U; shift += 8U) {
            auto value = byte();
            if (!value)
                return value.error();
            result |= static_cast<std::uint64_t>(value.value()) << shift;
        }
        return result;
    }

  private:
    const std::vector<Byte>& code_;
    const EncodingProfile& profile_;
    std::size_t position_{};
    std::size_t limit_{};
};

struct IslandStats {
    std::uint32_t size{};
    std::uint16_t nodes{};
    std::uint16_t carrier_writes{};
    std::uint16_t memory_writes{};
};

[[nodiscard]] std::string hex_word(Word value, unsigned width) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(static_cast<int>(width))
           << std::setfill('0') << value;
    return output.str();
}

[[nodiscard]] Result<NodeId> reference(Reader& reader, std::size_t current, const char* role) {
    auto id = reader.u16();
    if (!id)
        return id.error();
    if (id.value() >= current) {
        return make_error(ErrorCode::Verification, id.value(),
                          std::string(role) + " is not a prior primitive node");
    }
    return id.value();
}

[[nodiscard]] Result<IslandStats> scan_island(const Module& module, const EncodingProfile& profile,
                                              std::uint32_t offset,
                                              const PrimitivePlanLimits& limits,
                                              std::ostringstream* text) {
    Reader reader(module, profile, offset);
    auto size = reader.u32();
    auto node_count = reader.u16();
    auto carrier_writes = reader.u16();
    auto memory_writes = reader.u16();
    auto reserved = reader.u16();
    if (!size)
        return size.error();
    if (!node_count)
        return node_count.error();
    if (!carrier_writes)
        return carrier_writes.error();
    if (!memory_writes)
        return memory_writes.error();
    if (!reserved)
        return reserved.error();
    if (reserved.value() != 0 || size.value() < 12U || size.value() > module.code.size() - offset) {
        return make_error(ErrorCode::Verification, offset, "invalid physical island header");
    }
    if (node_count.value() == 0 || node_count.value() > limits.max_nodes ||
        carrier_writes.value() > limits.max_carrier_writes ||
        memory_writes.value() > limits.max_memory_writes) {
        return make_error(ErrorCode::Verification, offset,
                          "physical island exceeds verifier limits");
    }
    if (auto limited = reader.set_limit(offset + size.value()); !limited)
        return limited.error();

    if (text != nullptr) {
        *text << "island_" << hex_word(offset, 8).substr(2) << ":\n"
              << "  ; size=" << size.value() << ", nodes=" << node_count.value()
              << ", carrier-writes=" << carrier_writes.value()
              << ", memory-writes=" << memory_writes.value() << '\n';
    }

    for (std::uint16_t index = 0; index < node_count.value(); ++index) {
        auto encoded = reader.byte();
        if (!encoded)
            return encoded.error();
        auto kind = profile.primitives().decode(encoded.value());
        if (!kind)
            return kind.error();
        std::ostringstream operands;
        switch (kind.value()) {
        case PrimitiveKind::Literal: {
            auto value = reader.u64();
            if (!value)
                return value.error();
            operands << hex_word(value.value(), 16);
            break;
        }
        case PrimitiveKind::ReadCarrier: {
            auto carrier = reader.u32();
            if (!carrier)
                return carrier.error();
            if (carrier.value() >= module.carrier_count) {
                return make_error(ErrorCode::Verification, carrier.value(),
                                  "carrier read out of range");
            }
            operands << "c" << carrier.value();
            break;
        }
        case PrimitiveKind::ReadMemory8:
        case PrimitiveKind::ReadMemory64:
        case PrimitiveKind::Not:
        case PrimitiveKind::Neg: {
            auto input = reference(reader, index, "unary operand");
            if (!input)
                return input.error();
            operands << "n" << input.value();
            break;
        }
        case PrimitiveKind::Compare: {
            auto condition = reader.byte();
            if (!condition)
                return condition.error();
            if (condition.value() > static_cast<Byte>(CompareCondition::SignedLessEqual)) {
                return make_error(ErrorCode::Verification, condition.value(),
                                  "invalid comparison mode");
            }
            auto left = reference(reader, index, "compare left");
            auto right = reference(reader, index, "compare right");
            if (!left)
                return left.error();
            if (!right)
                return right.error();
            operands << "cc" << static_cast<unsigned>(condition.value()) << ", n" << left.value()
                     << ", n" << right.value();
            break;
        }
        case PrimitiveKind::Select: {
            auto condition = reference(reader, index, "select condition");
            auto when_true = reference(reader, index, "select true value");
            auto when_false = reference(reader, index, "select false value");
            if (!condition)
                return condition.error();
            if (!when_true)
                return when_true.error();
            if (!when_false)
                return when_false.error();
            operands << "n" << condition.value() << ", n" << when_true.value() << ", n"
                     << when_false.value();
            break;
        }
        case PrimitiveKind::Mix: {
            auto left = reference(reader, index, "mix left");
            auto right = reference(reader, index, "mix right");
            auto salt = reader.u64();
            if (!left)
                return left.error();
            if (!right)
                return right.error();
            if (!salt)
                return salt.error();
            operands << "n" << left.value() << ", n" << right.value() << ", "
                     << hex_word(salt.value(), 16);
            break;
        }
        default: {
            auto left = reference(reader, index, "binary left");
            auto right = reference(reader, index, "binary right");
            if (!left)
                return left.error();
            if (!right)
                return right.error();
            operands << "n" << left.value() << ", n" << right.value();
            break;
        }
        }
        if (text != nullptr) {
            *text << "  n" << index << " = " << primitive_name(kind.value());
            const std::string rendered = operands.str();
            if (!rendered.empty())
                *text << ' ' << rendered;
            *text << '\n';
        }
    }

    std::unordered_set<CarrierId> targets;
    for (std::uint16_t index = 0; index < carrier_writes.value(); ++index) {
        auto carrier = reader.u32();
        auto value = reader.u16();
        if (!carrier)
            return carrier.error();
        if (!value)
            return value.error();
        if (carrier.value() >= module.carrier_count || value.value() >= node_count.value()) {
            return make_error(ErrorCode::Verification, reader.position(), "invalid carrier commit");
        }
        if (carrier.value() >= 9U && carrier.value() < kFirstAllocatedCarrier) {
            return make_error(ErrorCode::Verification, carrier.value(),
                              "island writes a read-only ABI carrier");
        }
        if (!targets.emplace(carrier.value()).second) {
            return make_error(ErrorCode::Verification, carrier.value(), "duplicate carrier commit");
        }
        if (text != nullptr) {
            *text << "  delay c" << carrier.value() << " <- n" << value.value() << '\n';
        }
    }
    for (CarrierId carrier = kContinuationLaneCarrier; carrier < kContinuationProofCarrier + 3U;
         ++carrier) {
        if (!targets.contains(carrier)) {
            return make_error(ErrorCode::Verification, carrier,
                              "island does not replace every distributed continuation share");
        }
    }
    for (std::uint16_t index = 0; index < memory_writes.value(); ++index) {
        auto width = reader.byte();
        auto address = reader.u16();
        auto value = reader.u16();
        if (!width)
            return width.error();
        if (!address)
            return address.error();
        if (!value)
            return value.error();
        if ((width.value() != 1 && width.value() != 8) || address.value() >= node_count.value() ||
            value.value() >= node_count.value()) {
            return make_error(ErrorCode::Verification, reader.position(), "invalid memory commit");
        }
        if (text != nullptr) {
            *text << "  delay mem" << static_cast<unsigned>(width.value() * 8U) << "[n"
                  << address.value() << "] <- n" << value.value() << '\n';
        }
    }
    if (reader.position() != offset + size.value()) {
        return make_error(ErrorCode::Verification, reader.position(),
                          "island size has trailing bytes");
    }
    if (text != nullptr)
        *text << "  commit\n\n";
    return IslandStats{
        size.value(),
        node_count.value(),
        carrier_writes.value(),
        memory_writes.value(),
    };
}

[[nodiscard]] Result<PhysicalVerificationReport> scan_module(const Module& module,
                                                             const EncodingProfile& profile,
                                                             const PrimitivePlanLimits& limits,
                                                             std::ostringstream* text) {
    if (auto valid = validate_module(module); !valid) {
        return valid.error();
    }
    if (module.profile_fingerprint != profile.fingerprint()) {
        return make_error(ErrorCode::Verification, 0, "module and external profile do not match");
    }
    if (module.carrier_count < kFirstAllocatedCarrier ||
        module.initial_carriers.size() != module.carrier_count || module.code.empty()) {
        return make_error(ErrorCode::Verification, 0, "module physical state is malformed");
    }
    if (module.argument_count > kMaximumArguments || module.data.size() > module.memory_size) {
        return make_error(ErrorCode::Verification, 0, "module execution contract is malformed");
    }

    PhysicalVerificationReport report;
    std::uint32_t offset = 0;
    while (offset < module.code.size()) {
        report.island_offsets.push_back(offset);
        auto island = scan_island(module, profile, offset, limits, text);
        if (!island)
            return island.error();
        ++report.islands;
        report.primitive_nodes += island.value().nodes;
        report.carrier_writes += island.value().carrier_writes;
        report.memory_writes += island.value().memory_writes;
        if (island.value().size == 0 ||
            island.value().size > std::numeric_limits<std::uint32_t>::max() - offset) {
            return make_error(ErrorCode::Verification, offset, "island layout does not advance");
        }
        offset += island.value().size;
    }
    if (offset != module.code.size()) {
        return make_error(ErrorCode::Verification, offset,
                          "island stream does not cover code section");
    }

    const Continuation entry{
        module.initial_carriers[kContinuationLaneCarrier] ^
            module.initial_carriers[kContinuationLaneCarrier + 1U] ^
            module.initial_carriers[kContinuationLaneCarrier + 2U],
        module.initial_carriers[kContinuationProofCarrier] ^
            module.initial_carriers[kContinuationProofCarrier + 1U] ^
            module.initial_carriers[kContinuationProofCarrier + 2U],
    };
    auto entry_offset = profile.continuations().open(entry);
    if (!entry_offset)
        return entry_offset.error();
    if (!std::binary_search(report.island_offsets.begin(), report.island_offsets.end(),
                            entry_offset.value())) {
        return make_error(ErrorCode::Verification, entry_offset.value(),
                          "entry is not an island boundary");
    }
    return report;
}

} // namespace

Result<PhysicalVerificationReport> verify_physical_module(const Module& module,
                                                          const EncodingProfile& profile,
                                                          const PrimitivePlanLimits& limits) {
    return scan_module(module, profile, limits, nullptr);
}

Result<std::string> disassemble_module(const Module& module, const EncodingProfile& profile,
                                       const PrimitivePlanLimits& limits) {
    std::ostringstream text;
    text << "; XQVM physical island stream v" << kModuleVersion << '\n'
         << "; profile=" << hex_word(module.profile_fingerprint, 16)
         << ", carriers=" << module.carrier_count << ", arguments=" << module.argument_count
         << ", memory=" << module.memory_size << " bytes\n"
         << "; source origins live only in the private sidecar\n\n";
    auto report = scan_module(module, profile, limits, &text);
    if (!report)
        return report.error();
    return text.str();
}

} // namespace xqvm
