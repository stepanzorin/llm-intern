// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <format>
#include <stdexcept>
#include <string_view>

namespace stz::intern {

enum class assistant_profile_e {
    workflow,
    texting,
};

[[nodiscard]] inline std::string_view to_string(const assistant_profile_e profile) noexcept {
    switch (profile) {
        case assistant_profile_e::workflow: return "workflow";
        case assistant_profile_e::texting: return "texting";
    }

    return "workflow";
}

[[nodiscard]] inline assistant_profile_e assistant_profile_from_string(const std::string_view text) {
    if (text == "workflow") {
        return assistant_profile_e::workflow;
    }

    if (text == "texting") {
        return assistant_profile_e::texting;
    }

    throw std::runtime_error{std::format("Unknown assistant profile '{}'", text)};
}

} // namespace stz::intern
