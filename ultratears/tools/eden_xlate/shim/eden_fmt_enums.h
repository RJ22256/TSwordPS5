// Eden formats its enums with fmt (exception messages, logs) as the older fmt it pins did implicitly; current fmt
// (MSYS2's) no longer converts enums. Format any enum without its own formatter as its underlying value.
#pragma once
#include <type_traits>
#include <fmt/format.h>

template <typename E>
struct fmt::formatter<E, char, std::enable_if_t<std::is_enum_v<E>>> : fmt::formatter<std::underlying_type_t<E>, char> {
    auto format(E e, fmt::format_context& ctx) const {
        return fmt::formatter<std::underlying_type_t<E>, char>::format(static_cast<std::underlying_type_t<E>>(e), ctx);
    }
};
