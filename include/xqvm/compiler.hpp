#ifndef XQVM_COMPILER_HPP
#define XQVM_COMPILER_HPP

#include "xqvm/machine_ir.hpp"
#include "xqvm/module.hpp"
#include "xqvm/physical.hpp"
#include "xqvm/primitive.hpp"
#include "xqvm/profile.hpp"
#include "xqvm/result.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xqvm {

constexpr CarrierId kContinuationLaneCarrier = 0;
constexpr CarrierId kContinuationProofCarrier = 3;
constexpr CarrierId kReturnValueCarrier = 6;
constexpr CarrierId kArgumentCarrierBase = 16;
constexpr CarrierId kFirstAllocatedCarrier = 32;
constexpr std::uint32_t kHaltContinuation = 0xFFFFFFFFU;

struct SidecarPhysicalValue {
    std::string role;
    OriginId origin{};
    ValueDomain domain{ValueDomain::Integer};
    ShareFamily family{ShareFamily::Xor};
    std::array<PhysicalShare, 3> shares{};
};

struct SidecarRecord {
    OriginId origin{};
    std::string kind;
    std::string label;
    std::vector<SidecarPhysicalValue> physical_values;
    std::uint32_t island_offset{0xFFFFFFFFU};
    Continuation continuation{};
};

struct OriginSidecar {
    std::string function;
    std::uint64_t profile_fingerprint{};
    std::vector<SidecarRecord> records;
};

struct CompileOptions {
    std::uint32_t memory_size{64U * 1024U};
    std::vector<Byte> initial_data;
    PrimitivePlanLimits plan_limits{};
};

struct CompiledProgram {
    Module module;
    OriginSidecar sidecar;
};

[[nodiscard]] Result<CompiledProgram> compile_machine_ir(const MachineFunction& function,
                                                         const EncodingProfile& profile,
                                                         const CompileOptions& options = {});
[[nodiscard]] Result<void> save_origin_sidecar(const OriginSidecar& sidecar,
                                               const std::filesystem::path& path);

} // namespace xqvm

#endif // XQVM_COMPILER_HPP
