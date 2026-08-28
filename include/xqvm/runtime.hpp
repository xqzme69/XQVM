#ifndef XQVM_RUNTIME_HPP
#define XQVM_RUNTIME_HPP

#include "xqvm/module.hpp"
#include "xqvm/primitive.hpp"
#include "xqvm/profile.hpp"
#include "xqvm/result.hpp"
#include "xqvm/schema.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xqvm {

enum class RuntimeFaultCode : std::uint8_t {
    ArgumentWindow,
    ContinuationProof,
    IslandBounds,
    InvalidSchema,
    InvalidOperand,
    CarrierBounds,
    MemoryBounds,
    WriteConflict,
    ResourceLimit,
};

struct RuntimeFault {
    RuntimeFaultCode code{RuntimeFaultCode::InvalidOperand};
    std::size_t location{};
    std::string message;
};

struct MachineState {
    std::vector<Word> carriers;
    std::vector<Byte> memory;
    std::uint64_t islands_executed{};
};

struct IslandTraceEvent {
    std::uint64_t ordinal{};
    std::uint32_t opened_offset{};
    std::uint16_t primitive_nodes{};
    std::uint16_t carrier_writes{};
    std::uint16_t memory_writes{};
};

using TraceCallback = std::function<void(const IslandTraceEvent&)>;

struct RunOptions {
    std::uint64_t max_islands{1'000'000};
    std::optional<PrimitivePlanLimits> plan_limits;
    TraceCallback trace;
};

struct RunResult {
    std::optional<Word> value;
    std::optional<RuntimeFault> fault;
    MachineState final_state;

    [[nodiscard]] bool ok() const noexcept {
        return value.has_value() && !fault.has_value();
    }
};

class Machine {
  public:
    [[nodiscard]] static Result<Machine> create(Module module, EncodingProfile profile,
                                                const PrimitivePlanLimits& plan_limits = {});
    [[nodiscard]] RunResult run(std::span<const Word> arguments = {},
                                const RunOptions& options = {}) const;

  private:
    Machine(Module module, EncodingProfile profile, PrimitivePlanLimits plan_limits,
            std::vector<std::uint32_t> island_offsets)
        : module_(std::move(module)), profile_(profile), plan_limits_(plan_limits),
          island_offsets_(std::move(island_offsets)) {}

    Module module_;
    EncodingProfile profile_;
    PrimitivePlanLimits plan_limits_;
    std::vector<std::uint32_t> island_offsets_;
};

[[nodiscard]] std::string_view runtime_fault_name(RuntimeFaultCode code) noexcept;

} // namespace xqvm

#endif // XQVM_RUNTIME_HPP
