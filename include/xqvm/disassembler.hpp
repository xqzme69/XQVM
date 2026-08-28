#ifndef XQVM_DISASSEMBLER_HPP
#define XQVM_DISASSEMBLER_HPP

#include "xqvm/module.hpp"
#include "xqvm/primitive.hpp"
#include "xqvm/profile.hpp"
#include "xqvm/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xqvm {

struct PhysicalVerificationReport {
    std::size_t islands{};
    std::size_t primitive_nodes{};
    std::size_t carrier_writes{};
    std::size_t memory_writes{};
    std::vector<std::uint32_t> island_offsets;
};

[[nodiscard]] Result<PhysicalVerificationReport>
verify_physical_module(const Module& module, const EncodingProfile& profile,
                       const PrimitivePlanLimits& limits = {});
[[nodiscard]] Result<std::string> disassemble_module(const Module& module,
                                                     const EncodingProfile& profile,
                                                     const PrimitivePlanLimits& limits = {});

} // namespace xqvm

#endif // XQVM_DISASSEMBLER_HPP
