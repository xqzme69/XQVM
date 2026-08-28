#ifndef XQVM_RESULT_HPP
#define XQVM_RESULT_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace xqvm {

enum class ErrorCode : std::uint8_t {
    Io,
    InvalidArgument,
    InvalidModule,
    ChecksumMismatch,
    Assembly,
    Decode,
    Verification,
    Host,
    IslandBounds,
    InvalidSchema,
    InvalidOperand,
    CarrierBounds,
    MemoryBounds,
    WriteConflict,
    ResourceLimit,
};

struct Error {
    ErrorCode code{ErrorCode::InvalidArgument};
    std::size_t offset{};
    std::string message;
};

template <typename T> class Result {
  public:
    Result(T value) : storage_(std::move(value)) {}
    Result(Error error) : storage_(std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] T& value() & {
        return std::get<T>(storage_);
    }
    [[nodiscard]] const T& value() const& {
        return std::get<T>(storage_);
    }
    [[nodiscard]] T&& value() && {
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] Error& error() & {
        return std::get<Error>(storage_);
    }
    [[nodiscard]] const Error& error() const& {
        return std::get<Error>(storage_);
    }

  private:
    std::variant<T, Error> storage_;
};

template <> class Result<void> {
  public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept {
        return !error_.has_value();
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] Error& error() & {
        return *error_;
    }
    [[nodiscard]] const Error& error() const& {
        return *error_;
    }

  private:
    std::optional<Error> error_;
};

[[nodiscard]] inline Error make_error(ErrorCode code, std::size_t offset, std::string message) {
    return Error{code, offset, std::move(message)};
}

} // namespace xqvm

#endif // XQVM_RESULT_HPP
