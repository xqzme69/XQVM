#ifndef XQVM_MODULE_HPP
#define XQVM_MODULE_HPP

#include "xqvm/result.hpp"
#include "xqvm/schema.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace xqvm {

constexpr std::uint16_t kModuleVersion = 3;
constexpr std::size_t kModuleHeaderSize = 64;
constexpr std::uint16_t kMaximumArguments = 16;

struct Module {
    std::uint32_t memory_size{};
    std::uint32_t carrier_count{};
    std::uint16_t argument_count{};
    std::uint64_t profile_fingerprint{};
    std::vector<Byte> code;
    std::vector<Byte> data;
    std::vector<Word> initial_carriers;
};

struct ModuleLimits {
    std::size_t max_file_size{std::size_t{64} * 1024U * 1024U};
    std::size_t max_code_size{std::size_t{16} * 1024U * 1024U};
    std::size_t max_data_size{std::size_t{16} * 1024U * 1024U};
    std::uint32_t max_memory_size{64U * 1024U * 1024U};
    std::uint32_t max_carriers{1U << 20U};
};

[[nodiscard]] Result<void> validate_module(const Module& module, const ModuleLimits& limits = {});
[[nodiscard]] Result<std::vector<Byte>> serialize_module(const Module& module,
                                                         const ModuleLimits& limits = {});
[[nodiscard]] Result<Module> parse_module(std::span<const Byte> bytes,
                                          const ModuleLimits& limits = {});
[[nodiscard]] Result<void> save_module(const Module& module, const std::filesystem::path& path);
[[nodiscard]] Result<Module> load_module(const std::filesystem::path& path,
                                         const ModuleLimits& limits = {});

} // namespace xqvm

#endif // XQVM_MODULE_HPP
