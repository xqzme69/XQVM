#include "xqvm/compiler.hpp"
#include "xqvm/disassembler.hpp"
#include "xqvm/examples.hpp"
#include "xqvm/module.hpp"
#include "xqvm/profile.hpp"
#include "xqvm/runtime.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr xqvm::Word kDemoSeed = 0xC4A7D93E21B6580FULL;

[[nodiscard]] int fail(const xqvm::Error& error) {
    std::cerr << "error at 0x" << std::hex << error.offset << ": " << error.message << '\n';
    return 1;
}

[[nodiscard]] int fail(const xqvm::RuntimeFault& fault) {
    std::cerr << xqvm::runtime_fault_name(fault.code) << " at 0x" << std::hex << fault.location
              << ": " << fault.message << '\n';
    return 1;
}

[[nodiscard]] xqvm::Result<xqvm::Word> parse_word(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return xqvm::make_error(xqvm::ErrorCode::InvalidArgument, 0, "empty integer");
    }
    xqvm::Word value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return xqvm::make_error(xqvm::ErrorCode::InvalidArgument, 0,
                                "invalid 64-bit integer: " + std::string(text));
    }
    return value;
}

[[nodiscard]] xqvm::Result<std::vector<xqvm::Word>> parse_arguments(int argc, char** argv,
                                                                    int first) {
    std::vector<xqvm::Word> values;
    for (int index = first; index < argc; ++index) {
        auto value = parse_word(argv[index]);
        if (!value)
            return value.error();
        values.push_back(value.value());
    }
    return values;
}

[[nodiscard]] xqvm::Result<xqvm::CompiledProgram>
compile_demo(const xqvm::EncodingProfile& profile) {
    return xqvm::compile_machine_ir(xqvm::build_factorial_ir(), profile);
}

[[nodiscard]] xqvm::Result<void> save_fuzz_seed(const xqvm::Module& module,
                                                const xqvm::EncodingProfile& profile,
                                                const std::filesystem::path& path) {
    if (module.profile_fingerprint != profile.fingerprint()) {
        return xqvm::make_error(xqvm::ErrorCode::Verification, 0,
                                "module and external profile do not match");
    }
    auto serialized = xqvm::serialize_module(module);
    if (!serialized)
        return serialized.error();
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return xqvm::make_error(xqvm::ErrorCode::Io, 0, "cannot open fuzz seed: " + path.string());
    }
    const xqvm::Word seed = profile.seed();
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        output.put(static_cast<char>(static_cast<xqvm::Byte>(seed >> shift)));
    }
    output.write(reinterpret_cast<const char*>(serialized.value().data()),
                 static_cast<std::streamsize>(serialized.value().size()));
    output.close();
    if (!output) {
        return xqvm::make_error(xqvm::ErrorCode::Io, 0, "cannot write fuzz seed: " + path.string());
    }
    return {};
}

[[nodiscard]] int execute(xqvm::Module module, xqvm::EncodingProfile profile,
                          std::span<const xqvm::Word> arguments, bool trace) {
    auto verified = xqvm::verify_physical_module(module, profile);
    if (!verified)
        return fail(verified.error());
    auto machine = xqvm::Machine::create(std::move(module), profile);
    if (!machine)
        return fail(machine.error());
    xqvm::RunOptions options;
    if (trace) {
        options.trace = [](const xqvm::IslandTraceEvent& event) {
            std::cout << "island #" << event.ordinal << " opened@0x" << std::hex
                      << event.opened_offset << std::dec << " nodes=" << event.primitive_nodes
                      << " delayed-carriers=" << event.carrier_writes
                      << " delayed-memory=" << event.memory_writes << '\n';
        };
    }
    xqvm::RunResult result = machine.value().run(arguments, options);
    if (!result.ok())
        return fail(*result.fault);
    std::cout << "result = 0x" << std::hex << std::uppercase << *result.value << std::dec << " ("
              << *result.value << "), islands = " << result.final_state.islands_executed << '\n';
    return 0;
}

void usage() {
    std::cerr << "usage:\n"
              << "  xqvm demo [n]\n"
              << "  xqvm build-demo PREFIX [seed]\n"
              << "  xqvm verify MODULE PROFILE\n"
              << "  xqvm disasm MODULE PROFILE\n"
              << "  xqvm run MODULE PROFILE [arg ...]\n"
              << "  xqvm trace MODULE PROFILE [arg ...]\n"
              << "  xqvm fuzz-seed MODULE PROFILE OUTPUT\n";
}

} // namespace

[[nodiscard]] int run_cli(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string_view command = argv[1];

    if (command == "fuzz-seed") {
        if (argc != 5) {
            usage();
            return 2;
        }
        auto module = xqvm::load_module(argv[2]);
        auto profile = xqvm::load_profile(argv[3]);
        if (!module)
            return fail(module.error());
        if (!profile)
            return fail(profile.error());
        const std::filesystem::path output_path = argv[4];
        if (!output_path.parent_path().empty()) {
            std::error_code directory_error;
            std::filesystem::create_directories(output_path.parent_path(), directory_error);
            if (directory_error) {
                return fail(xqvm::make_error(xqvm::ErrorCode::Io, 0,
                                             "cannot create output directory: " +
                                                 directory_error.message()));
            }
        }
        auto saved = save_fuzz_seed(module.value(), profile.value(), output_path);
        if (!saved)
            return fail(saved.error());
        std::cout << "fuzz seed " << std::filesystem::absolute(output_path).string() << '\n';
        return 0;
    }
    if (command == "demo") {
        if (argc > 3) {
            usage();
            return 2;
        }
        xqvm::Word argument = 10;
        if (argc == 3) {
            auto parsed = parse_word(argv[2]);
            if (!parsed)
                return fail(parsed.error());
            argument = parsed.value();
        }
        const xqvm::EncodingProfile profile = xqvm::EncodingProfile::from_seed(kDemoSeed);
        auto compiled = compile_demo(profile);
        if (!compiled)
            return fail(compiled.error());
        return execute(std::move(compiled.value().module), profile,
                       std::span<const xqvm::Word>(&argument, 1), false);
    }

    if (command == "build-demo") {
        if (argc < 3 || argc > 4) {
            usage();
            return 2;
        }
        xqvm::Word seed = kDemoSeed;
        if (argc == 4) {
            auto parsed = parse_word(argv[3]);
            if (!parsed)
                return fail(parsed.error());
            seed = parsed.value();
        }
        const xqvm::EncodingProfile profile = xqvm::EncodingProfile::from_seed(seed);
        auto compiled = compile_demo(profile);
        if (!compiled)
            return fail(compiled.error());
        const std::filesystem::path prefix = argv[2];
        const std::filesystem::path module_path = prefix.string() + ".xqvm";
        const std::filesystem::path profile_path = prefix.string() + ".xqprofile";
        const std::filesystem::path sidecar_path = prefix.string() + ".xqmap";
        if (!prefix.parent_path().empty()) {
            std::error_code directory_error;
            std::filesystem::create_directories(prefix.parent_path(), directory_error);
            if (directory_error) {
                return fail(xqvm::make_error(xqvm::ErrorCode::Io, 0,
                                             "cannot create output directory: " +
                                                 directory_error.message()));
            }
        }
        if (auto saved = xqvm::save_module(compiled.value().module, module_path); !saved) {
            return fail(saved.error());
        }
        if (auto saved = xqvm::save_profile(profile, profile_path); !saved) {
            return fail(saved.error());
        }
        if (auto saved = xqvm::save_origin_sidecar(compiled.value().sidecar, sidecar_path);
            !saved) {
            return fail(saved.error());
        }
        std::cout << "module   " << std::filesystem::absolute(module_path).string() << '\n'
                  << "profile  " << std::filesystem::absolute(profile_path).string() << '\n'
                  << "private  " << std::filesystem::absolute(sidecar_path).string() << '\n';
        return 0;
    }

    if (command == "verify" || command == "disasm" || command == "run" || command == "trace") {
        if (argc < 4) {
            usage();
            return 2;
        }
        auto module = xqvm::load_module(argv[2]);
        auto profile = xqvm::load_profile(argv[3]);
        if (!module)
            return fail(module.error());
        if (!profile)
            return fail(profile.error());

        if (command == "verify") {
            if (argc != 4) {
                usage();
                return 2;
            }
            auto report = xqvm::verify_physical_module(module.value(), profile.value());
            if (!report)
                return fail(report.error());
            std::cout << "valid: " << report.value().islands << " physical islands, "
                      << report.value().primitive_nodes << " primitive nodes, "
                      << report.value().carrier_writes << " delayed carrier writes, "
                      << report.value().memory_writes << " delayed memory writes\n";
            return 0;
        }
        if (command == "disasm") {
            if (argc != 4) {
                usage();
                return 2;
            }
            auto text = xqvm::disassemble_module(module.value(), profile.value());
            if (!text)
                return fail(text.error());
            std::cout << text.value();
            return 0;
        }
        auto arguments = parse_arguments(argc, argv, 4);
        if (!arguments)
            return fail(arguments.error());
        return execute(std::move(module.value()), profile.value(), arguments.value(),
                       command == "trace");
    }

    usage();
    return 2;
}

int main(int argc, char** argv) noexcept {
    try {
        return run_cli(argc, argv);
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "fatal: %s\n", exception.what());
        return 1;
    } catch (...) {
        std::fputs("fatal: non-standard exception\n", stderr);
        return 1;
    }
}
