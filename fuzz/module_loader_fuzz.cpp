#include "xqvm/compiler.hpp"
#include "xqvm/disassembler.hpp"
#include "xqvm/module.hpp"
#include "xqvm/profile.hpp"
#include "xqvm/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < sizeof(std::uint64_t))
        return 0;
    std::uint64_t seed = 0;
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        seed |= static_cast<std::uint64_t>(data[shift / 8U]) << shift;
    }
    xqvm::ModuleLimits limits;
    limits.max_file_size = 1U << 20U;
    limits.max_code_size = 1U << 18U;
    limits.max_data_size = 1U << 18U;
    limits.max_memory_size = 1U << 20U;
    limits.max_carriers = 1U << 16U;

    const xqvm::EncodingProfile profile = xqvm::EncodingProfile::from_seed(seed);
    const auto physical_bytes =
        std::span<const xqvm::Byte>(data + sizeof(seed), size - sizeof(seed));
    (void)xqvm::parse_profile(physical_bytes);
    auto module = xqvm::parse_module(physical_bytes, limits);
    if (module) {
        auto verified = xqvm::verify_physical_module(module.value(), profile);
        if (verified) {
            (void)xqvm::disassemble_module(module.value(), profile);
            const std::size_t argument_count = module.value().argument_count;
            auto machine = xqvm::Machine::create(std::move(module.value()), profile);
            if (machine) {
                std::vector<xqvm::Word> arguments(argument_count, 0);
                xqvm::RunOptions options;
                options.max_islands = 16;
                (void)machine.value().run(arguments, options);
            }
        }
    }

    if (!physical_bytes.empty() && physical_bytes.size() <= limits.max_code_size) {
        xqvm::Module physical;
        physical.memory_size = 4096;
        physical.carrier_count = 64;
        physical.profile_fingerprint = profile.fingerprint();
        physical.code.assign(physical_bytes.begin(), physical_bytes.end());
        physical.initial_carriers.assign(physical.carrier_count, 0);
        const xqvm::Continuation entry = profile.continuations().seal(0);
        physical.initial_carriers[xqvm::kContinuationLaneCarrier] = entry.lane;
        physical.initial_carriers[xqvm::kContinuationProofCarrier] = entry.proof;
        auto physical_verified = xqvm::verify_physical_module(physical, profile);
        if (physical_verified) {
            (void)xqvm::disassemble_module(physical, profile);
            auto machine = xqvm::Machine::create(std::move(physical), profile);
            if (machine) {
                xqvm::RunOptions options;
                options.max_islands = 16;
                (void)machine.value().run({}, options);
            }
        }
    }
    return 0;
}
