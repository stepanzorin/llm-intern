// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#include "organization_config.hpp"

#include "emojis.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <format>
#include <initializer_list>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

namespace stz::intern {

namespace {

using json = nlohmann::json;

struct service_match_s {
    const organization_service_s *service = nullptr;
    std::size_t score = 0;
    std::size_t mention_position = std::string_view::npos;
};

[[nodiscard]] std::vector<std::string_view> beauty_directions_from_query(
        std::string_view query);

[[nodiscard]] std::string read_text_file(const std::filesystem::path &filename) {
    auto stream = std::ifstream{filename, std::ios::binary};

    if (!stream) {
        throw std::runtime_error{std::format("Cannot open organization config file: {}",
                                             filename.string())};
    }

    auto buffer = std::ostringstream{};
    buffer << stream.rdbuf();

    if (!stream.good() && !stream.eof()) {
        throw std::runtime_error{std::format("Cannot read organization config file: {}",
                                             filename.string())};
    }

    return std::move(buffer).str();
}

[[nodiscard]] std::string string_value(const json &object,
                                       const std::string_view key,
                                       const std::string_view fallback = {}) {
    if (!object.is_object()) {
        return std::string{fallback};
    }

    const auto it = object.find(key);

    if (it == object.end() || !it->is_string()) {
        return std::string{fallback};
    }

    return it->get<std::string>();
}

[[nodiscard]] bool bool_value(const json &object,
                              const std::string_view key,
                              const bool fallback = false) {
    if (!object.is_object()) {
        return fallback;
    }

    const auto it = object.find(key);

    if (it == object.end() || !it->is_boolean()) {
        return fallback;
    }

    return it->get<bool>();
}

[[nodiscard]] std::size_t size_value(const json &object,
                                     const std::string_view key,
                                     const std::size_t fallback) {
    if (!object.is_object()) {
        return fallback;
    }

    const auto it = object.find(key);

    if (it == object.end() || !it->is_number_unsigned()) {
        return fallback;
    }

    return it->get<std::size_t>();
}

[[nodiscard]] std::optional<std::size_t> optional_size_value(
        const json &object,
        const std::string_view key) {
    if (!object.is_object()) {
        return std::nullopt;
    }

    const auto it = object.find(key);

    if (it == object.end() || !it->is_number_unsigned()) {
        return std::nullopt;
    }

    return it->get<std::size_t>();
}

[[nodiscard]] std::vector<std::string> string_array(const json &value) {
    auto result = std::vector<std::string>{};

    if (!value.is_array()) {
        return result;
    }

    result.reserve(value.size());

    for (const auto &item : value) {
        if (item.is_string()) {
            auto text = item.get<std::string>();

            if (!text.empty()) {
                result.push_back(std::move(text));
            }
        }
    }

    return result;
}

[[nodiscard]] const json &object_value(const json &object, const std::string_view key) {
    static const auto empty = json::object();

    if (!object.is_object()) {
        return empty;
    }

    const auto it = object.find(key);

    if (it == object.end() || !it->is_object()) {
        return empty;
    }

    return *it;
}

[[nodiscard]] const json &array_value(const json &object, const std::string_view key) {
    static const auto empty = json::array();

    if (!object.is_object()) {
        return empty;
    }

    const auto it = object.find(key);

    if (it == object.end() || !it->is_array()) {
        return empty;
    }

    return *it;
}

[[nodiscard]] bool valid_time(const std::string_view value) noexcept {
    if (value.size() != 5 || value[2] != ':' ||
        std::isdigit(static_cast<unsigned char>(value[0])) == 0 ||
        std::isdigit(static_cast<unsigned char>(value[1])) == 0 ||
        std::isdigit(static_cast<unsigned char>(value[3])) == 0 ||
        std::isdigit(static_cast<unsigned char>(value[4])) == 0) {
        return false;
    }

    const auto hours = (value[0] - '0') * 10 + (value[1] - '0');
    const auto minutes = (value[3] - '0') * 10 + (value[4] - '0');

    return hours >= 0 && hours <= 23 && minutes >= 0 && minutes <= 59;
}

void validate_schedule(const organization_schedule_s &schedule) {
    for (const auto &rule : schedule.regular) {
        if (!valid_time(rule.opens) || !valid_time(rule.closes)) {
            throw std::runtime_error{std::format(
                    "Invalid organization schedule interval '{}-{}'",
                    rule.opens,
                    rule.closes)};
        }
    }
}

[[nodiscard]] organization_schedule_s parse_schedule(const json &value) {
    auto result = organization_schedule_s{};
    result.timezone = string_value(value, "timezone");

    for (const auto &item : array_value(value, "regular")) {
        if (!item.is_object()) {
            continue;
        }

        result.regular.push_back(organization_schedule_rule_s{
                .label = string_value(item, "label"),
                .days = string_array(item.value("days", json::array())),
                .opens = string_value(item, "opens"),
                .closes = string_value(item, "closes"),
        });
    }

    const auto &holidays = object_value(value, "holidays");
    result.holidays = organization_schedule_exception_s{
            .policy = string_value(holidays, "policy"),
            .note = string_value(holidays, "note"),
    };

    const auto &new_year = object_value(value, "new_year");
    result.new_year = organization_schedule_exception_s{
            .policy = string_value(new_year, "policy"),
            .note = string_value(new_year, "note"),
    };

    validate_schedule(result);

    return result;
}

[[nodiscard]] std::vector<organization_booking_method_s> parse_booking_methods(
        const json &value) {
    auto result = std::vector<organization_booking_method_s>{};

    if (!value.is_array()) {
        return result;
    }

    result.reserve(value.size());

    for (const auto &item : value) {
        if (!item.is_object()) {
            continue;
        }

        result.push_back(organization_booking_method_s{
                .id = string_value(item, "id"),
                .enabled = bool_value(item, "enabled"),
                .label = string_value(item, "label"),
                .value = string_value(item, "value"),
                .instructions = string_value(item, "instructions"),
        });
    }

    return result;
}

[[nodiscard]] bool bool_value_any(const json &object,
                                  const std::initializer_list<std::string_view> keys,
                                  const bool fallback = false) {
    for (const auto key : keys) {
        if (object.is_object()) {
            const auto it = object.find(key);

            if (it != object.end() && it->is_boolean()) {
                return it->get<bool>();
            }
        }
    }

    return fallback;
}

[[nodiscard]] organization_minor_service_direction_s parse_minor_service_direction(
        const json &value,
        const std::string_view id) {
    auto result = organization_minor_service_direction_s{
            .available = bool_value_any(value, {"available", "avaiable", "avaiable:"}),
            .min_age = optional_size_value(value, "min_age"),
    };

    if (!result.available) {
        result.min_age.reset();
        return result;
    }

    if (!result.min_age.has_value()) {
        throw std::runtime_error{std::format(
                "Minor beauty direction '{}' is available but has no min_age",
                id)};
    }

    if (*result.min_age >= 18) {
        throw std::runtime_error{std::format(
                "Minor beauty direction '{}' has invalid min_age {}",
                id,
                *result.min_age)};
    }

    return result;
}

[[nodiscard]] const organization_minor_service_direction_s *beauty_child_direction(
        const beauty_salon_service_directions_s &directions,
        const std::string_view id) noexcept {
    if (id == "child_manicure") return &directions.child_manicure;
    if (id == "child_pedicure") return &directions.child_pedicure;
    if (id == "child_brows") return &directions.child_brows;
    if (id == "child_eyelashes") return &directions.child_eyelashes;
    if (id == "child_hairdressing") return &directions.child_hairdressing;
    if (id == "child_cosmetology") return &directions.child_cosmetology;
    if (id == "child_hair_removal") return &directions.child_hair_removal;
    if (id == "child_makeup") return &directions.child_makeup;
    if (id == "child_massage") return &directions.child_massage;
    if (id == "child_podology") return &directions.child_podology;
    return nullptr;
}

[[nodiscard]] bool beauty_direction_is_child(const std::string_view id) noexcept {
    return id == "child_manicure" || id == "child_pedicure" || id == "child_brows" ||
           id == "child_eyelashes" || id == "child_hairdressing" ||
           id == "child_cosmetology" || id == "child_hair_removal" ||
           id == "child_makeup" || id == "child_massage" || id == "child_podology";
}

[[nodiscard]] bool beauty_direction_enabled(
        const beauty_salon_service_directions_s &directions,
        const std::string_view id) noexcept {
    if (id == "manicure") return directions.manicure;
    if (id == "pedicure") return directions.pedicure;
    if (id == "brows") return directions.brows;
    if (id == "eyelashes") return directions.eyelashes;
    if (id == "hairdressing") return directions.hairdressing;
    if (id == "cosmetology") return directions.cosmetology;
    if (id == "hair_removal") return directions.hair_removal;
    if (id == "makeup") return directions.makeup;
    if (id == "massage") return directions.massage;
    if (id == "podology") return directions.podology;

    if (const auto *child = beauty_child_direction(directions, id); child != nullptr) {
        return child->available;
    }

    return false;
}

[[nodiscard]] bool known_beauty_direction(const std::string_view id) noexcept {
    return id == "manicure" || id == "pedicure" || id == "brows" ||
           id == "eyelashes" || id == "hairdressing" ||
           id == "cosmetology" || id == "hair_removal" ||
           id == "makeup" || id == "massage" || id == "podology" ||
           beauty_direction_is_child(id);
}

[[nodiscard]] beauty_salon_service_directions_s parse_beauty_directions(
        const json &value) {
    return beauty_salon_service_directions_s{
            .manicure = bool_value(value, "manicure"),
            .child_manicure = parse_minor_service_direction(object_value(value, "child_manicure"), "child_manicure"),
            .pedicure = bool_value(value, "pedicure"),
            .child_pedicure = parse_minor_service_direction(object_value(value, "child_pedicure"), "child_pedicure"),
            .brows = bool_value(value, "brows"),
            .child_brows = parse_minor_service_direction(object_value(value, "child_brows"), "child_brows"),
            .eyelashes = bool_value(value, "eyelashes"),
            .child_eyelashes = parse_minor_service_direction(object_value(value, "child_eyelashes"), "child_eyelashes"),
            .hairdressing = bool_value(value, "hairdressing"),
            .child_hairdressing = parse_minor_service_direction(object_value(value, "child_hairdressing"), "child_hairdressing"),
            .cosmetology = bool_value(value, "cosmetology"),
            .child_cosmetology = parse_minor_service_direction(object_value(value, "child_cosmetology"), "child_cosmetology"),
            .hair_removal = bool_value(value, "hair_removal"),
            .child_hair_removal = parse_minor_service_direction(object_value(value, "child_hair_removal"), "child_hair_removal"),
            .makeup = bool_value(value, "makeup"),
            .child_makeup = parse_minor_service_direction(object_value(value, "child_makeup"), "child_makeup"),
            .massage = bool_value(value, "massage"),
            .child_massage = parse_minor_service_direction(object_value(value, "child_massage"), "child_massage"),
            .podology = bool_value(value, "podology"),
            .child_podology = parse_minor_service_direction(object_value(value, "child_podology"), "child_podology"),
    };
}

[[nodiscard]] organization_service_minor_access_s parse_minor_access(
        const json &value,
        const std::string_view service_name) {
    auto result = organization_service_minor_access_s{
            .allowed = bool_value(value, "allowed"),
            .min_age = optional_size_value(value, "min_age"),
    };

    if (!result.allowed) {
        result.min_age.reset();
        return result;
    }

    if (!result.min_age.has_value()) {
        throw std::runtime_error{std::format(
                "Service '{}' allows minors but has no min_age",
                service_name)};
    }

    if (*result.min_age >= 18) {
        throw std::runtime_error{std::format(
                "Service '{}' has invalid minor min_age {}",
                service_name,
                *result.min_age)};
    }

    return result;
}

[[nodiscard]] std::optional<organization_service_minor_access_s>
derive_minor_access_from_child_directions(
        const beauty_salon_service_directions_s &directions,
        const std::vector<std::string> &service_directions) {
    auto result = organization_service_minor_access_s{};
    auto saw_child_direction = false;

    for (const auto &direction : service_directions) {
        const auto *child = beauty_child_direction(directions, direction);

        if (child == nullptr) {
            continue;
        }

        saw_child_direction = true;

        if (!child->available) {
            continue;
        }

        result.allowed = true;

        if (child->min_age.has_value() &&
            (!result.min_age.has_value() || *child->min_age < *result.min_age)) {
            result.min_age = child->min_age;
        }
    }

    if (!saw_child_direction) {
        return std::nullopt;
    }

    if (!result.allowed) {
        result.min_age.reset();
    }

    return result;
}

[[nodiscard]] std::optional<organization_service_minor_access_s> parse_service_minor_access(
        const json &item,
        const std::string_view service_name,
        const beauty_salon_service_directions_s &directions,
        const std::vector<std::string> &service_directions) {
    if (const auto derived = derive_minor_access_from_child_directions(directions,
                                                                       service_directions);
        derived.has_value()) {
        return derived;
    }

    if (!item.is_object() || !item.contains("minor_access")) {
        return std::nullopt;
    }

    return parse_minor_access(object_value(item, "minor_access"), service_name);
}

[[nodiscard]] organization_service_warranty_s parse_service_warranty(
        const json &item,
        const std::string_view service_name) {
    if (item.is_object() && item.contains("warranty")) {
        const auto &value = object_value(item, "warranty");
        auto result = organization_service_warranty_s{
                .provided = bool_value(value, "provided"),
                .period_days = optional_size_value(value, "period_days"),
                .note = string_value(value, "note"),
        };

        if (result.provided &&
            (!result.period_days.has_value() || *result.period_days == 0)) {
            throw std::runtime_error{std::format(
                    "Service '{}' provides warranty but has no positive period_days",
                    service_name)};
        }

        if (!result.provided) {
            result.period_days.reset();
        }

        return result;
    }

    // Transitional compatibility with configs created before the structured
    // warranty object was introduced. Such entries keep working, but cannot
    // provide an exact warranty period until migrated to warranty.period_days.
    return organization_service_warranty_s{
            .provided = bool_value(item, "warranty_case"),
            .period_days = std::nullopt,
            .note = string_value(item, "warranty_note"),
    };
}

[[nodiscard]] std::vector<organization_service_s> parse_services(
        const json &value,
        const beauty_salon_service_directions_s &directions) {
    auto result = std::vector<organization_service_s>{};

    if (!value.is_array()) {
        return result;
    }

    result.reserve(value.size());

    for (const auto &item : value) {
        if (!item.is_object()) {
            continue;
        }

        const auto name = string_value(item, "name");

        if (name.empty()) {
            continue;
        }

        auto service_directions = string_array(item.value("directions", json::array()));

        result.push_back(organization_service_s{
                .name = name,
                .aliases = string_array(item.value("aliases", json::array())),
                .directions = service_directions,
                .minor_access = parse_service_minor_access(item,
                                                            name,
                                                            directions,
                                                            service_directions),
                .warranty = parse_service_warranty(item, name),
        });
    }

    return result;
}

void validate_beauty_services(const beauty_salon_config_s &config) {
    for (const auto &service : config.services) {
        for (const auto &direction : service.directions) {
            if (!known_beauty_direction(direction)) {
                throw std::runtime_error{std::format(
                        "Service '{}' references unknown beauty direction '{}'",
                        service.name,
                        direction)};
            }
        }
    }
}

[[nodiscard]] beauty_salon_config_s parse_beauty_salon(const json &value) {
    auto result = beauty_salon_config_s{};
    result.service_directions = parse_beauty_directions(
            object_value(value, "service_directions"));
    result.services = parse_services(array_value(value, "services"),
                                     result.service_directions);
    result.product_categories = string_array(
            value.value("product_categories", json::array()));

    validate_beauty_services(result);
    return result;
}

[[nodiscard]] coffee_shop_config_s parse_coffee_shop(const json &value) {
    return coffee_shop_config_s{
            .menu_categories = string_array(value.value("menu_categories", json::array())),
            .alternative_milk_types = string_array(value.value("alternative_milk_types", json::array())),
            .takeaway_available = bool_value(value, "takeaway_available"),
            .delivery_available = bool_value(value, "delivery_available"),
            .decaf_available = bool_value(value, "decaf_available"),
            .reusable_cup_allowed = bool_value(value, "reusable_cup_allowed"),
    };
}

[[nodiscard]] gas_station_config_s parse_gas_station(const json &value) {
    auto result = gas_station_config_s{
            .canister_refueling_allowed = bool_value(value, "canister_refueling_allowed"),
            .accepted_canister_types = string_array(value.value("accepted_canister_types", json::array())),
            .has_shop = bool_value(value, "has_shop"),
            .has_toilet = bool_value(value, "has_toilet"),
            .has_tire_inflation = bool_value(value, "has_tire_inflation"),
    };

    for (const auto &item : array_value(value, "fuel_types")) {
        if (!item.is_object()) {
            continue;
        }

        const auto name = string_value(item, "name");

        if (name.empty()) {
            continue;
        }

        result.fuel_types.push_back(gas_station_fuel_s{
                .name = name,
                .aliases = string_array(item.value("aliases", json::array())),
        });
    }

    return result;
}

[[nodiscard]] gym_config_s parse_gym(const json &value) {
    auto result = gym_config_s{
            .has_shower = bool_value(value, "has_shower"),
            .towels_provided = bool_value(value, "towels_provided"),
            .has_lockers = bool_value(value, "has_lockers"),
            .has_sauna = bool_value(value, "has_sauna"),
            .trial_workout_available = bool_value(value, "trial_workout_available"),
    };

    for (const auto &item : array_value(value, "coaching_options")) {
        if (!item.is_object()) {
            continue;
        }

        result.coaching_options.push_back(gym_coaching_option_s{
                .qualification = string_value(item, "qualification"),
                .specialization = string_value(item, "specialization"),
        });
    }

    return result;
}

[[nodiscard]] organization_business_details_s parse_business_details(const json &value) {
    const auto &beauty = object_value(value, "beauty_salon");
    const auto &legacy_hair = object_value(value, "hair_salon");

    return organization_business_details_s{
            .beauty_salon = parse_beauty_salon(beauty.empty() ? legacy_hair : beauty),
            .coffee_shop = parse_coffee_shop(object_value(value, "coffee_shop")),
            .gas_station = parse_gas_station(object_value(value, "gas_station")),
            .gym = parse_gym(object_value(value, "gym")),
    };
}

[[nodiscard]] char32_t decode_utf8_codepoint(const std::string_view text,
                                             std::size_t &offset) noexcept {
    const auto first = static_cast<unsigned char>(text[offset++]);

    if ((first & 0x80U) == 0) {
        return first;
    }

    const auto take_continuation = [&]() noexcept -> unsigned char {
        if (offset >= text.size()) {
            return 0;
        }

        return static_cast<unsigned char>(text[offset++]) & 0x3FU;
    };

    if ((first & 0xE0U) == 0xC0U) {
        return static_cast<char32_t>(((first & 0x1FU) << 6U) | take_continuation());
    }

    if ((first & 0xF0U) == 0xE0U) {
        return static_cast<char32_t>(((first & 0x0FU) << 12U) |
                                     (take_continuation() << 6U) |
                                     take_continuation());
    }

    if ((first & 0xF8U) == 0xF0U) {
        return static_cast<char32_t>(((first & 0x07U) << 18U) |
                                     (take_continuation() << 12U) |
                                     (take_continuation() << 6U) |
                                     take_continuation());
    }

    return U' ';
}

void append_utf8(std::string &result, const char32_t codepoint) {
    if (codepoint <= 0x7FU) {
        result.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
        result.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0xFFFFU) {
        result.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else {
        result.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        result.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

[[nodiscard]] char32_t lowercase_codepoint(char32_t codepoint) noexcept {
    if (codepoint >= U'A' && codepoint <= U'Z') {
        return codepoint + (U'a' - U'A');
    }

    if (codepoint >= U'А' && codepoint <= U'Я') {
        return codepoint + (U'а' - U'А');
    }

    if (codepoint == U'Ё') {
        return U'е';
    }

    if (codepoint == U'ё') {
        return U'е';
    }

    return codepoint;
}

[[nodiscard]] char32_t uppercase_codepoint(char32_t codepoint) noexcept {
    if (codepoint >= U'a' && codepoint <= U'z') {
        return codepoint - (U'a' - U'A');
    }

    if (codepoint >= U'а' && codepoint <= U'я') {
        return codepoint - (U'а' - U'А');
    }

    return codepoint;
}

[[nodiscard]] std::string capitalize_first(std::string value) {
    if (value.empty()) {
        return value;
    }

    auto offset = std::size_t{0};
    const auto first = uppercase_codepoint(decode_utf8_codepoint(value, offset));
    auto result = std::string{};
    result.reserve(value.size());
    append_utf8(result, first);
    result.append(value.substr(offset));
    return result;
}

[[nodiscard]] std::string lowercase_first(std::string value) {
    if (value.empty()) {
        return value;
    }

    auto offset = std::size_t{0};
    const auto first = lowercase_codepoint(decode_utf8_codepoint(value, offset));
    auto result = std::string{};
    result.reserve(value.size());
    append_utf8(result, first);
    result.append(value.substr(offset));
    return result;
}

[[nodiscard]] bool is_word_codepoint(const char32_t codepoint) noexcept {
    return (codepoint >= U'a' && codepoint <= U'z') ||
           (codepoint >= U'0' && codepoint <= U'9') ||
           (codepoint >= U'а' && codepoint <= U'я') ||
           codepoint == U'і' || codepoint == U'ї' || codepoint == U'є';
}

[[nodiscard]] std::string normalize_query(const std::string_view text) {
    auto result = std::string{};
    result.reserve(text.size());

    auto previous_space = true;
    auto offset = std::size_t{0};

    while (offset < text.size()) {
        auto codepoint = lowercase_codepoint(decode_utf8_codepoint(text, offset));

        if (!is_word_codepoint(codepoint)) {
            if (!previous_space) {
                result.push_back(' ');
                previous_space = true;
            }

            continue;
        }

        append_utf8(result, codepoint);
        previous_space = false;
    }

    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }

    return result;
}

[[nodiscard]] bool contains_any(const std::string_view text,
                                const std::initializer_list<std::string_view> values) noexcept {
    return std::ranges::any_of(values, [text](const std::string_view value) {
        return text.contains(value);
    });
}

[[nodiscard]] bool contains_word(const std::string_view text,
                                 const std::string_view word) {
    if (text.empty() || word.empty()) {
        return false;
    }

    const auto padded_text = std::format(" {} ", text);
    const auto padded_word = std::format(" {} ", word);
    return padded_text.contains(padded_word);
}

/*
 * Intent matching must tolerate polite and auxiliary words inserted between
 * the meaningful words: "как называется Ваш салон", "как я могу записаться"
 * and similar phrases. Prefixes are used deliberately to cover Russian word
 * forms without introducing a full morphological analyser.
 */
[[nodiscard]] bool contains_word_prefix(const std::string_view text,
                                        const std::string_view prefix) noexcept {
    if (text.empty() || prefix.empty()) {
        return false;
    }

    auto offset = std::size_t{0};

    while (offset < text.size()) {
        const auto end = text.find(' ', offset);
        const auto word = text.substr(offset,
                                      end == std::string_view::npos
                                              ? text.size() - offset
                                              : end - offset);

        if (word.starts_with(prefix)) {
            return true;
        }

        if (end == std::string_view::npos) {
            break;
        }

        offset = end + 1;
    }

    return false;
}

[[nodiscard]] bool contains_any_word_prefix(
        const std::string_view text,
        const std::initializer_list<std::string_view> prefixes) noexcept {
    return std::ranges::any_of(prefixes, [text](const std::string_view prefix) {
        return contains_word_prefix(text, prefix);
    });
}

[[nodiscard]] bool contains_any_word(
        const std::string_view text,
        const std::initializer_list<std::string_view> words) {
    return std::ranges::any_of(words, [text](const std::string_view word) {
        return contains_word(text, word);
    });
}

[[nodiscard]] std::string join_strings(const std::vector<std::string> &values,
                                       const std::string_view separator) {
    auto result = std::string{};

    for (const auto &value : values) {
        if (value.empty()) {
            continue;
        }

        if (!result.empty()) {
            result += separator;
        }

        result += value;
    }

    return result;
}

[[nodiscard]] std::string join_human_readable(const std::vector<std::string> &values) {
    if (values.empty()) {
        return {};
    }

    if (values.size() == 1) {
        return values.front();
    }

    auto result = std::string{};

    for (auto index = std::size_t{0}; index < values.size(); ++index) {
        if (index != 0) {
            result += index + 1 == values.size() ? " и " : ", ";
        }

        result += values[index];
    }

    return result;
}

[[nodiscard]] char32_t last_utf8_codepoint(const std::string_view value) noexcept {
    auto offset = std::size_t{0};
    auto result = char32_t{0};

    while (offset < value.size()) {
        result = decode_utf8_codepoint(value, offset);
    }

    return result;
}

[[nodiscard]] bool ends_with_emoji_like_symbol(const std::string_view value) noexcept {
    const auto last = last_utf8_codepoint(value);

    return last == 0xFE0F ||
           (last >= 0x2600 && last <= 0x27BF) ||
           (last >= 0x1F000 && last <= 0x1FAFF);
}

[[nodiscard]] std::string sentence(std::string value) {
    if (value.empty()) {
        return value;
    }

    const auto last = value.back();

    if (last == '.' || last == '!' || last == '?' || ends_with_emoji_like_symbol(value)) {
        return value;
    }

    constexpr auto friendly_prefix = std::string_view{"[[friendly:"};
    constexpr auto friendly_suffix = std::string_view{"]]"};

    if (value.ends_with(friendly_suffix)) {
        const auto marker_begin = value.rfind(friendly_prefix);

        if (marker_begin != std::string::npos) {
            auto text_before_marker = std::string_view{value}.substr(0, marker_begin);
            while (!text_before_marker.empty() && text_before_marker.back() == ' ') {
                text_before_marker.remove_suffix(1);
            }

            if (!text_before_marker.empty()) {
                const auto before_marker = text_before_marker.back();
                if (before_marker == '.' || before_marker == '!' || before_marker == '?' ||
                    ends_with_emoji_like_symbol(text_before_marker)) {
                    return value;
                }
            }
        }
    }

    const auto last_line_begin = value.rfind('\n');
    const auto last_line = std::string_view{value}.substr(
            last_line_begin == std::string::npos ? 0 : last_line_begin + 1);

    if (last_line.starts_with('@') || last_line.starts_with('+') ||
        last_line.starts_with("http://") || last_line.starts_with("https://")) {
        return value;
    }

    value.push_back('.');
    return value;
}

[[nodiscard]] std::string text_section(const std::string_view title,
                                       const std::string_view body) {
    if (body.empty()) {
        return {};
    }

    if (title == "Важно") {
        return std::format("Важно: {}", sentence(std::string{body}));
    }

    return std::format("{}:\n{}", title, body);
}

[[nodiscard]] std::string inline_text_section(const std::string_view title,
                                              const std::string_view body) {
    if (body.empty()) {
        return {};
    }

    return std::format("{}: {}", title, body);
}

[[nodiscard]] std::string em_dash_list(const std::vector<std::string> &values) {
    auto lines = std::vector<std::string>{};
    lines.reserve(values.size());

    for (const auto &value : values) {
        if (!value.empty()) {
            lines.push_back(std::format("— {}", value));
        }
    }

    return join_strings(lines, "\n");
}

[[nodiscard]] std::string append_sentence(std::string text,
                                          const std::string_view detail) {
    text = sentence(std::move(text));

    if (detail.empty()) {
        return text;
    }

    text.push_back(' ');
    text += sentence(capitalize_first(std::string{detail}));
    return text;
}

[[nodiscard]] std::string append_important_note(std::string text,
                                                const std::string_view note) {
    text = sentence(std::move(text));

    if (note.empty()) {
        return text;
    }

    text += "\n\n";
    text += text_section("Важно", note);
    return text;
}

[[nodiscard]] bool asks_location(const std::string_view query) noexcept {
    return contains_any(query,
                        {
                                "где находится",
                                "где расположен",
                                "где расположена",
                                "где расположено",
                                "как найти",
                                "куда подойти",
                            });
}

[[nodiscard]] bool asks_how(const std::string_view query) noexcept {
    return contains_any_word(query, {"как", "куда"}) ||
           contains_any(query,
                        {
                                "каким способом",
                                "что нужно сделать",
                            });
}

[[nodiscard]] std::string affirmative_note(const std::string_view note) {
    if (note.empty()) {
        return {};
    }

    return sentence(std::format("Да, {}", lowercase_first(std::string{note})));
}

[[nodiscard]] std::string location_note(const std::string_view subject,
                                        const std::string_view note) {
    if (note.empty()) {
        return sentence(std::string{subject});
    }

    const auto normalized = normalize_query(note);
    const auto markers = std::array{
            std::string_view{"расположен "},
            std::string_view{"расположена "},
            std::string_view{"расположено "},
            std::string_view{"находится "},
    };

    for (const auto marker : markers) {
        const auto marker_position = normalized.find(marker);

        if (marker_position == std::string::npos) {
            continue;
        }

        const auto original_position = note.find(marker);

        if (original_position != std::string::npos) {
            return sentence(std::format("{} {}",
                                        subject,
                                        note.substr(original_position)));
        }
    }

    return append_sentence(std::string{subject}, note);
}

[[nodiscard]] std::string strip_leading_confirmation(std::string value) {
    constexpr auto prefixes = std::array{
            std::string_view{"Да, "},
            std::string_view{"Да. "},
    };

    for (const auto prefix : prefixes) {
        if (value.starts_with(prefix)) {
            value.erase(0, prefix.size());
            return capitalize_first(std::move(value));
        }
    }

    return value;
}

[[nodiscard]] std::optional<emoji_kind_e> emoji_kind_for_topic(
        const std::string_view topic) noexcept {
    if (topic == "name") return emoji_kind_e::name;
    if (topic == "address") return emoji_kind_e::address;
    if (topic == "schedule") return emoji_kind_e::schedule;
    if (topic == "payment") return emoji_kind_e::payment;
    if (topic == "cash") return emoji_kind_e::cash;
    if (topic == "card") return emoji_kind_e::card;
    if (topic == "email") return emoji_kind_e::email;
    if (topic == "booking") return emoji_kind_e::booking;
    if (topic == "phone") return emoji_kind_e::phone;
    if (topic == "website") return emoji_kind_e::website;
    if (topic == "parking") return emoji_kind_e::parking;
    if (topic == "wifi") return emoji_kind_e::wifi;
    if (topic == "coffee") return emoji_kind_e::coffee;
    if (topic == "child_zone") return emoji_kind_e::child_zone;
    if (topic == "pets") return emoji_kind_e::pets;
    if (topic == "smoking") return emoji_kind_e::smoking;
    if (topic == "services") return emoji_kind_e::services;
    if (topic == "products") return emoji_kind_e::products;
    if (topic == "warranty" || topic == "warranty_incident") return emoji_kind_e::warranty;
    if (topic == "menu") return emoji_kind_e::menu;
    if (topic == "alternative_milk") return emoji_kind_e::alternative_milk;
    if (topic == "decaf") return emoji_kind_e::decaf;
    if (topic == "takeaway") return emoji_kind_e::takeaway;
    if (topic == "delivery") return emoji_kind_e::delivery;
    if (topic == "fuel") return emoji_kind_e::fuel;
    if (topic == "canister") return emoji_kind_e::canister;
    if (topic == "shop") return emoji_kind_e::shop;
    if (topic == "toilet") return emoji_kind_e::toilet;
    if (topic == "shower") return emoji_kind_e::shower;
    if (topic == "lockers") return emoji_kind_e::lockers;
    if (topic == "sauna") return emoji_kind_e::sauna;
    if (topic == "trial_workout") return emoji_kind_e::trial_workout;
    if (topic == "coaching") return emoji_kind_e::coaching;
    if (topic == "telegram") return emoji_kind_e::telegram;
    if (topic == "whatsapp") return emoji_kind_e::whatsapp;
    if (topic == "max") return emoji_kind_e::max;
    if (topic == "maps") return emoji_kind_e::maps;
    if (topic == "service_directions") return emoji_kind_e::service_directions;
    if (topic == "service_direction" || topic == "service_availability") return emoji_kind_e::services;
    if (topic == "product_availability") return emoji_kind_e::products;
    if (topic == "minors") return emoji_kind_e::minors;
    if (topic == "gift_certificate") return emoji_kind_e::gift_certificate;
    if (topic == "staff_call_button") return emoji_kind_e::staff_call_button;
    if (topic == "ramp") return emoji_kind_e::ramp;
    if (topic == "accessible_parking") return emoji_kind_e::accessible_parking;
    if (topic == "smile") return emoji_kind_e::smile;
    if (topic == "tada") return emoji_kind_e::tada;
    if (topic == "confetti_ball") return emoji_kind_e::confetti_ball;
    if (topic == "birthday") return emoji_kind_e::birthday;
    if (topic == "disappointment") return emoji_kind_e::disappointment;

    return std::nullopt;
}

[[nodiscard]] std::string emoji_for(const std::string_view topic) {
    const auto kind = emoji_kind_for_topic(topic);
    return kind.has_value() ? std::string{put_emoji(*kind)} : std::string{};
}

[[nodiscard]] std::string with_friendly_emoji(std::string text,
                                               const emoji_kind_e kind) {
    const auto emoji = put_emoji(kind);

    if (emoji.empty()) {
        return text;
    }

    text += std::format("[[friendly: {}]]", emoji);
    return text;
}

[[nodiscard]] organization_config_answer_s make_answer(const organization_config_s &config,
                                                        std::string topic,
                                                        std::string fact_text,
                                                        std::string customer_text) {
    return organization_config_answer_s{
            .topic = topic,
            .fact_text = sentence(std::move(fact_text)),
            .customer_text = sentence(std::move(customer_text)),
            .emoji = emoji_for(topic),
    };
}

struct schedule_request_s {
    bool full = false;
    bool days = false;
    bool opening_time = false;
    bool closing_time = false;
    bool holidays = false;
    bool new_year = false;
    bool december_31 = false;
    bool january_1_or_2 = false;
    std::vector<std::string_view> weekdays = {};

    [[nodiscard]] bool matched() const noexcept {
        return full || days || opening_time || closing_time || holidays || new_year ||
               !weekdays.empty();
    }
};

struct weekday_view_s {
    std::string_view id = {};
    std::string_view customer_label = {};
};

constexpr auto weekdays = std::array{
        weekday_view_s{"monday", "по понедельникам"},
        weekday_view_s{"tuesday", "по вторникам"},
        weekday_view_s{"wednesday", "по средам"},
        weekday_view_s{"thursday", "по четвергам"},
        weekday_view_s{"friday", "по пятницам"},
        weekday_view_s{"saturday", "по субботам"},
        weekday_view_s{"sunday", "по воскресеньям"},
};

[[nodiscard]] bool covers_every_day(const organization_schedule_rule_s &rule) {
    static const auto every_day = std::unordered_set<std::string>{
            "monday",
            "tuesday",
            "wednesday",
            "thursday",
            "friday",
            "saturday",
            "sunday",
    };

    if (rule.days.size() != every_day.size()) {
        return false;
    }

    return std::ranges::all_of(rule.days, [](const std::string &day) {
        return every_day.contains(day);
    });
}

[[nodiscard]] std::string schedule_rule_label(const organization_schedule_rule_s &rule) {
    if (!rule.label.empty()) {
        return rule.label;
    }

    return join_strings(rule.days, ", ");
}

[[nodiscard]] std::string regular_schedule_text(const organization_schedule_s &schedule,
                                                 const bool customer_facing) {
    auto parts = std::vector<std::string>{};

    for (const auto &rule : schedule.regular) {
        if (rule.opens.empty() || rule.closes.empty()) {
            continue;
        }

        if (schedule.regular.size() == 1 && covers_every_day(rule)) {
            return customer_facing
                           ? std::format("Мы работаем ежедневно с {} до {}", rule.opens, rule.closes)
                           : std::format("График работы: ежедневно с {} до {}", rule.opens, rule.closes);
        }

        const auto label = schedule_rule_label(rule);

        if (!label.empty()) {
            parts.push_back(std::format("{} — с {} до {}", label, rule.opens, rule.closes));
        }
    }

    if (parts.empty()) {
        return {};
    }

    return customer_facing
                   ? std::format("Мы работаем по следующему графику: {}",
                                 join_strings(parts, "; "))
                   : std::format("График работы: {}", join_strings(parts, "; "));
}

[[nodiscard]] std::string schedule_days_text(const organization_schedule_s &schedule,
                                              const bool customer_facing) {
    if (schedule.regular.size() == 1 && covers_every_day(schedule.regular.front())) {
        return customer_facing
                       ? "Мы работаем ежедневно, с понедельника по воскресенье"
                       : "Рабочие дни: ежедневно, с понедельника по воскресенье";
    }

    auto labels = std::vector<std::string>{};

    for (const auto &rule : schedule.regular) {
        if (const auto label = schedule_rule_label(rule); !label.empty()) {
            labels.push_back(label);
        }
    }

    if (labels.empty()) {
        return {};
    }

    return customer_facing
                   ? std::format("Мы работаем по дням: {}", join_human_readable(labels))
                   : std::format("Рабочие дни: {}", join_human_readable(labels));
}

[[nodiscard]] std::string schedule_boundary_text(const organization_schedule_s &schedule,
                                                  const bool opening,
                                                  const bool customer_facing) {
    auto values = std::vector<std::string>{};

    for (const auto &rule : schedule.regular) {
        const auto &time = opening ? rule.opens : rule.closes;

        if (!time.empty() && std::ranges::find(values, time) == values.end()) {
            values.push_back(time);
        }
    }

    if (values.empty()) {
        return {};
    }

    if (values.size() == 1) {
        if (customer_facing) {
            return opening
                           ? std::format("Мы открываемся в {}", values.front())
                           : std::format("Мы работаем до {}", values.front());
        }

        return opening
                       ? std::format("Время открытия: {}", values.front())
                       : std::format("Время закрытия: {}", values.front());
    }

    auto parts = std::vector<std::string>{};

    for (const auto &rule : schedule.regular) {
        const auto label = schedule_rule_label(rule);
        const auto &time = opening ? rule.opens : rule.closes;

        if (!label.empty() && !time.empty()) {
            parts.push_back(std::format("{} — {} {}",
                                        label,
                                        opening ? "с" : "до",
                                        time));
        }
    }

    if (parts.empty()) {
        return {};
    }

    return customer_facing
                   ? std::format("{}: {}",
                                 opening ? "Мы открываемся" : "Мы закрываемся",
                                 join_strings(parts, "; "))
                   : std::format("{}: {}",
                                 opening ? "Время открытия" : "Время закрытия",
                                 join_strings(parts, "; "));
}

[[nodiscard]] std::string schedule_exception_text(
        const organization_schedule_exception_s &exception,
        const std::string_view fact_prefix,
        const std::string_view customer_prefix) {
    if (exception.policy.empty()) {
        return {};
    }

    auto result = customer_prefix.empty()
                          ? std::format("{}: {}", fact_prefix, exception.policy)
                          : std::format("{} {}", customer_prefix, exception.policy);

    if (!exception.note.empty()) {
        const auto normalized_note = normalize_query(exception.note);
        const auto is_condition = normalized_note.starts_with("если ") ||
                                  normalized_note.starts_with("при условии ");
        result += is_condition
                          ? std::format(", {}", lowercase_first(exception.note))
                          : std::format(". {}", capitalize_first(exception.note));
    }

    return result;
}

[[nodiscard]] std::string schedule_exception_policy_part(
        const organization_schedule_exception_s &exception,
        const std::initializer_list<std::string_view> markers) {
    auto offset = std::size_t{0};

    while (offset < exception.policy.size()) {
        const auto end = exception.policy.find(';', offset);
        auto part = exception.policy.substr(offset,
                                            end == std::string::npos
                                                    ? exception.policy.size() - offset
                                                    : end - offset);

        while (!part.empty() && part.front() == ' ') {
            part.erase(part.begin());
        }
        while (!part.empty() && part.back() == ' ') {
            part.pop_back();
        }

        const auto normalized_part = normalize_query(part);

        if (!part.empty() && std::ranges::any_of(markers, [&](const auto marker) {
                return normalized_part.contains(marker);
            })) {
            auto result = capitalize_first(std::move(part));

            if (!exception.note.empty()) {
                result += std::format(". {}", capitalize_first(exception.note));
            }

            return result;
        }

        if (end == std::string::npos) {
            break;
        }

        offset = end + 1;
    }

    return {};
}

[[nodiscard]] std::string schedule_weekdays_text(
        const organization_schedule_s &schedule,
        const std::vector<std::string_view> &requested_weekdays,
        const bool customer_facing) {
    auto parts = std::vector<std::string>{};

    for (const auto requested_day : requested_weekdays) {
        const auto weekday_it = std::ranges::find(weekdays,
                                                  requested_day,
                                                  &weekday_view_s::id);

        if (weekday_it == weekdays.end()) {
            continue;
        }

        const auto rule_it = std::ranges::find_if(schedule.regular,
                                                  [&](const auto &rule) {
                                                      return std::ranges::find(rule.days,
                                                                               requested_day) !=
                                                             rule.days.end();
                                                  });

        if (rule_it == schedule.regular.end() ||
            rule_it->opens.empty() || rule_it->closes.empty()) {
            parts.push_back(customer_facing
                                    ? std::format("{} мы не работаем",
                                                  capitalize_first(
                                                          std::string{weekday_it->customer_label}))
                                    : std::format("{} — выходной",
                                                  capitalize_first(
                                                          std::string{weekday_it->customer_label})));
            continue;
        }

        parts.push_back(customer_facing
                                ? std::format("{} мы работаем с {} до {}",
                                              capitalize_first(
                                                      std::string{weekday_it->customer_label}),
                                              rule_it->opens,
                                              rule_it->closes)
                                : std::format("{} — с {} до {}",
                                              capitalize_first(
                                                      std::string{weekday_it->customer_label}),
                                              rule_it->opens,
                                              rule_it->closes));
    }

    return join_strings(parts, "\n");
}

[[nodiscard]] std::optional<organization_config_answer_s> schedule_answer(
        const organization_config_s &config,
        const schedule_request_s &request) {
    auto fact_parts = std::vector<std::string>{};
    auto customer_parts = std::vector<std::string>{};

    if (request.full) {
        fact_parts.push_back(regular_schedule_text(config.schedule, false));
        customer_parts.push_back(regular_schedule_text(config.schedule, true));
    } else {
        if (request.days) {
            fact_parts.push_back(schedule_days_text(config.schedule, false));
            customer_parts.push_back(schedule_days_text(config.schedule, true));
        }
        if (request.opening_time) {
            fact_parts.push_back(schedule_boundary_text(config.schedule, true, false));
            customer_parts.push_back(schedule_boundary_text(config.schedule, true, true));
        }
        if (request.closing_time) {
            fact_parts.push_back(schedule_boundary_text(config.schedule, false, false));
            customer_parts.push_back(schedule_boundary_text(config.schedule, false, true));
        }
        if (!request.weekdays.empty()) {
            fact_parts.push_back(schedule_weekdays_text(config.schedule,
                                                        request.weekdays,
                                                        false));
            customer_parts.push_back(schedule_weekdays_text(config.schedule,
                                                            request.weekdays,
                                                            true));
        }
    }

    if (request.holidays) {
        fact_parts.push_back(schedule_exception_text(config.schedule.holidays,
                                                     "Праздничные дни",
                                                     {}));
        customer_parts.push_back(schedule_exception_text(config.schedule.holidays,
                                                         "Праздничные дни",
                                                         "В праздничные дни мы работаем"));
    }

    if (request.new_year) {
        auto specific_policy = std::string{};

        if (request.january_1_or_2) {
            specific_policy = schedule_exception_policy_part(config.schedule.new_year,
                                                             {"1 и 2 января",
                                                              "1 января",
                                                              "2 января"});
        } else if (request.december_31) {
            specific_policy = schedule_exception_policy_part(config.schedule.new_year,
                                                             {"31 декабря"});
        }

        if (!specific_policy.empty()) {
            fact_parts.push_back(specific_policy);
            customer_parts.push_back(std::move(specific_policy));
        } else {
            fact_parts.push_back(schedule_exception_text(config.schedule.new_year,
                                                         "Новогодние праздники",
                                                         {}));
            customer_parts.push_back(schedule_exception_text(config.schedule.new_year,
                                                             "Новогодние праздники",
                                                             "В новогодние праздники мы работаем"));
        }
    }

    const auto fact = join_strings(fact_parts, "\n\n");
    const auto customer = join_strings(customer_parts, "\n\n");

    if (fact.empty() || customer.empty()) {
        return std::nullopt;
    }

    return make_answer(config, "schedule", fact, customer);
}

struct address_request_s {
    bool address = false;
    bool directions = false;
    bool landmark = false;
    bool entrance = false;

    [[nodiscard]] bool matched() const noexcept {
        return address || directions || landmark || entrance;
    }
};

[[nodiscard]] std::optional<organization_config_answer_s> address_answer(
        const organization_config_s &config,
        const address_request_s &request) {
    const auto &address = config.contacts.address;

    if (!request.matched()) {
        return std::nullopt;
    }

    auto fact_sections = std::vector<std::string>{};
    auto customer_sections = std::vector<std::string>{};

    const auto append_detail = [&](const std::string_view title,
                                   const std::string_view value) {
        if (value.empty()) {
            return;
        }

        fact_sections.push_back(inline_text_section(title, value));
        customer_sections.push_back(inline_text_section(title, value));
    };

    if (request.address) {
        append_detail("Адрес", address.formatted);
    }
    if (request.directions) {
        append_detail("Как добраться", address.directions);
    }
    if (request.landmark) {
        append_detail("Ориентир", address.landmark);
    }
    if (request.entrance) {
        append_detail("Вход", address.entrance);
    }

    if (fact_sections.empty()) {
        return std::nullopt;
    }

    return make_answer(config,
                       "address",
                       join_strings(fact_sections, "\n\n"),
                       join_strings(customer_sections, "\n\n"));
}

[[nodiscard]] std::string organization_customer_subject(
        const organization_business_type_e type) {
    switch (type) {
        case organization_business_type_e::beauty_salon:
            return "Наш салон";

        case organization_business_type_e::coffee_shop:
            return "Наша кофейня";

        case organization_business_type_e::gas_station:
            return "Наша АЗС";

        case organization_business_type_e::gym:
            return "Наш спортзал";

        case organization_business_type_e::unknown:
            return "Наша организация";
    }

    return "Наша организация";
}

[[nodiscard]] std::optional<organization_config_answer_s> identity_answer(
        const organization_config_s &config) {
    if (config.brand_name.empty()) {
        return std::nullopt;
    }

    const auto emphasized_name = std::format("«{}»", config.brand_name);

    return make_answer(
            config,
            "name",
            std::format("Название организации: {}", emphasized_name),
            std::format("{} называется {}",
                        organization_customer_subject(config.business_type),
                        emphasized_name));
}

struct payment_request_s {
    bool general = false;
    bool cash = false;
    bool card = false;
    bool credit_card = false;
    bool qr_code = false;
    bool sbp = false;
    bool online = false;
    bool gift_certificate = false;
    bool on_site = false;
    bool prepayment = false;
    bool prepayment_paid = false;
    bool postpayment = false;
    bool certificate_validity = false;

    [[nodiscard]] bool has_specific_method() const noexcept {
        return cash || card || credit_card || qr_code || sbp || online ||
               gift_certificate || on_site;
    }

    [[nodiscard]] bool matched() const noexcept {
        return general || has_specific_method() || prepayment ||
               prepayment_paid || postpayment || certificate_validity;
    }
};

struct payment_method_view_s {
    bool requested = false;
    bool available = false;
    std::string_view label = {};
};

[[nodiscard]] std::vector<std::string> alternative_payment_method_labels(
        const organization_payment_methods_s &payment,
        const payment_request_s &request,
        const bool customer_facing) {
    auto labels = std::vector<std::string>{};

    const auto add_label = [&](std::string label,
                               const std::optional<emoji_kind_e> emoji = std::nullopt) {
        if (customer_facing && emoji.has_value()) {
            label = with_friendly_emoji(std::move(label), *emoji);
        }

        labels.push_back(std::move(label));
    };

    if (payment.cash && !request.cash) {
        add_label("наличными", emoji_kind_e::cash);
    }

    const auto requested_available_card =
            request.card || (request.credit_card && payment.credit_card);

    if (payment.card && !requested_available_card) {
        add_label(payment.credit_card
                          ? "обычной или кредитной банковской картой"
                          : "обычной банковской картой",
                  emoji_kind_e::card);
    } else if (payment.credit_card && !request.credit_card) {
        add_label("кредитной банковской картой", emoji_kind_e::card);
    }

    if (payment.qr_code && !request.qr_code) {
        add_label("по QR-коду");
    }

    if (payment.sbp && !request.sbp) {
        add_label("через СБП");
    }

    if (payment.online && !request.online) {
        add_label("онлайн");
    }

    if (payment.gift_certificate && !request.gift_certificate) {
        auto label = std::string{"подарочным сертификатом или абонементом"};
        if (customer_facing) {
            label = with_friendly_emoji(std::move(label),
                                        emoji_kind_e::gift_certificate);
            label = with_friendly_emoji(std::move(label),
                                        emoji_kind_e::payment);
        }
        labels.push_back(std::move(label));
    }

    if (labels.empty() && payment.on_site && !request.on_site) {
        add_label("на месте");
    }

    return labels;
}

[[nodiscard]] std::size_t requested_payment_methods_count(
        const payment_request_s &request) noexcept {
    auto result = std::size_t{0};
    result += request.cash ? 1 : 0;
    result += request.card ? 1 : 0;
    result += request.credit_card ? 1 : 0;
    result += request.qr_code ? 1 : 0;
    result += request.sbp ? 1 : 0;
    result += request.online ? 1 : 0;
    result += request.gift_certificate ? 1 : 0;
    result += request.on_site ? 1 : 0;
    return result;
}

[[nodiscard]] std::string single_payment_success_text(
        const organization_payment_methods_s &payment,
        const payment_request_s &request,
        const std::string_view fallback_method) {
    if (request.card && payment.card) {
        if (payment.credit_card) {
            return "Да, оплатить можно обычной или кредитной банковской картой";
        }

        return "Да, оплатить можно обычной банковской картой";
    }

    if (request.credit_card && payment.credit_card) {
        return "Да, оплатить можно кредитной банковской картой";
    }

    if (request.gift_certificate && payment.gift_certificate) {
        return "Да, оплатить можно подарочным сертификатом или абонементом";
    }

    if (request.on_site && payment.on_site) {
        return "Да, оплатить можно на месте";
    }

    return std::format("Да, оплатить можно {}", fallback_method);
}

void apply_payment_answer_emoji(organization_config_answer_s &answer,
                                const organization_payment_methods_s &payment,
                                const payment_request_s &request) {
    if (request.general || !request.has_specific_method() ||
        requested_payment_methods_count(request) != 1) {
        return;
    }

    if ((request.cash && !payment.cash) || (request.card && !payment.card) ||
        (request.credit_card && !payment.credit_card) ||
        (request.qr_code && !payment.qr_code) || (request.sbp && !payment.sbp) ||
        (request.online && !payment.online) ||
        (request.gift_certificate && !payment.gift_certificate) ||
        (request.on_site && !payment.on_site)) {
        answer.emoji.clear();
        return;
    }

    if (request.cash) {
        answer.emoji = emoji_for("cash");
        return;
    }

    if (request.card || request.credit_card) {
        answer.emoji = emoji_for("card");
        return;
    }

    if (request.gift_certificate) {
        answer.emoji = emoji_for("gift_certificate");
        return;
    }

    /*
     * SBP, QR and online payment are not card payments. A generic money/card
     * emoji after such a precise answer looks misleading, so keep the answer
     * plain.
     */
    answer.emoji.clear();
}

[[nodiscard]] std::optional<organization_config_answer_s> payment_answer(
        const organization_config_s &config,
        const payment_request_s &request) {
    const auto &payment = config.payment_methods;
    const auto methods = std::array{
            payment_method_view_s{request.cash, payment.cash, "наличными"},
            payment_method_view_s{request.card, payment.card, "обычной банковской картой"},
            payment_method_view_s{request.credit_card, payment.credit_card, "кредитной картой"},
            payment_method_view_s{request.qr_code, payment.qr_code, "по QR-коду"},
            payment_method_view_s{request.sbp, payment.sbp, "через СБП"},
            payment_method_view_s{request.online, payment.online, "онлайн"},
            payment_method_view_s{request.gift_certificate,
                                  payment.gift_certificate,
                                  "подарочным сертификатом или абонементом"},
            payment_method_view_s{request.on_site, payment.on_site, "на месте"},
    };

    auto fact_sections = std::vector<std::string>{};
    auto customer_sections = std::vector<std::string>{};
    auto has_unavailable_method = false;
    auto customer_unavailable_intro = std::optional<std::string>{};
    auto customer_unavailable_section_index = std::optional<std::size_t>{};
    const auto prioritize_postpayment = request.postpayment &&
                                        request.general &&
                                        !request.has_specific_method();

    if (prioritize_postpayment) {
        const auto postpayment_text = payment.note.empty()
                                              ? std::string{
                                                        "Оплата производится после оказания услуги"}
                                              : capitalize_first(payment.note);
        fact_sections.push_back(postpayment_text);
        customer_sections.push_back(postpayment_text);
    }

    if (request.certificate_validity) {
        has_unavailable_method = !payment.gift_certificate;
        const auto text = !payment.gift_certificate
                                  ? std::string{
                                            "Оплата подарочным сертификатом или абонементом не предусмотрена"}
                                  : !payment.gift_certificate_note.empty()
                                            ? sentence(capitalize_first(
                                                      payment.gift_certificate_note))
                                            : std::string{
                                                      "Для оплаты необходимо предъявить действующий подарочный сертификат или абонемент"};

        if (!payment.gift_certificate) {
            customer_unavailable_intro =
                    "Оплата подарочным сертификатом или абонементом у нас пока не поддерживается";
        }

        fact_sections.push_back(text);
        customer_sections.push_back(customer_unavailable_intro.value_or(text));
        if (customer_unavailable_intro.has_value()) {
            customer_unavailable_section_index = customer_sections.size() - 1;
        }
    } else if (request.general) {
        auto fact_methods = std::vector<std::string>{};
        auto customer_methods = std::vector<std::string>{};

        if (payment.cash) {
            fact_methods.emplace_back("наличными");
            customer_methods.emplace_back(std::format("наличными {}", put_emoji(emoji_kind_e::cash)));
        }

        if (payment.card) {
            fact_methods.emplace_back(payment.credit_card
                                              ? "обычной или кредитной банковской картой"
                                              : "обычной банковской картой");
            customer_methods.emplace_back(std::format("{} {}",
                                                      payment.credit_card
                                                              ? "обычной или кредитной банковской картой"
                                                              : "обычной банковской картой",
                                                      put_emoji(emoji_kind_e::card)));
        } else if (payment.credit_card) {
            fact_methods.emplace_back("кредитной банковской картой");
            customer_methods.emplace_back(std::format("кредитной банковской картой {}",
                                                      put_emoji(emoji_kind_e::card)));
        }

        if (payment.qr_code) {
            fact_methods.emplace_back("по QR-коду");
            customer_methods.emplace_back("по QR-коду");
        }

        if (payment.sbp) {
            fact_methods.emplace_back("через СБП");
            customer_methods.emplace_back("через СБП");
        }

        if (payment.online) {
            fact_methods.emplace_back("онлайн");
            customer_methods.emplace_back("онлайн");
        }

        if (payment.gift_certificate) {
            fact_methods.emplace_back("подарочным сертификатом или абонементом");
            customer_methods.emplace_back(std::format("подарочным сертификатом или абонементом {}",
                                                      put_emoji(emoji_kind_e::gift_certificate)));
        }

        if (!fact_methods.empty()) {
            fact_sections.push_back(text_section("Доступные способы оплаты",
                                                 em_dash_list(fact_methods)));

            auto customer = std::string{"У нас Вы можете оплатить услуги и товары следующими способами:\n"};
            customer += em_dash_list(customer_methods);
            customer += "\n\n";
            customer += "Уточните, пожалуйста, присутствует ли здесь удобный для Вас способ оплаты? И какой? 😊";

            if (!payment.note.empty() || !payment.prepayment_required) {
                customer += "\n\n";
                customer += "Примечание: если Вы уже внесли предоплату, сообщите об этом администратору — мы проверим платёж и учтём его при окончательном расчёте.";
            }

            customer_sections.push_back(std::move(customer));
        }
    } else {
        auto available_methods = std::vector<std::string>{};
        auto unavailable_methods = std::vector<std::string>{};

        for (const auto &method : methods) {
            if (!method.requested) {
                continue;
            }

            if (!method.available) {
                has_unavailable_method = true;
            }

            (method.available ? available_methods : unavailable_methods).emplace_back(method.label);
        }

        if (available_methods.size() == 1 && unavailable_methods.empty()) {
            const auto text = single_payment_success_text(payment,
                                                          request,
                                                          available_methods.front());
            fact_sections.push_back(text);
            customer_sections.push_back(text);
        } else if (unavailable_methods.size() == 1 && available_methods.empty()) {
            const auto method = unavailable_methods.front();
            const auto text = std::format("Нет, оплата {} не предусмотрена", method);
            fact_sections.push_back(text);

            customer_unavailable_intro = request.online
                                                   ? std::string{
                                                             "Онлайн-оплаты у нас пока нет"}
                                                   : std::format(
                                                             "Оплата {} у нас пока не поддерживается",
                                                             method);
            customer_sections.push_back(*customer_unavailable_intro);
            customer_unavailable_section_index = customer_sections.size() - 1;
        } else {
            if (!available_methods.empty()) {
                fact_sections.push_back(inline_text_section(
                        "Можно оплатить",
                        join_human_readable(available_methods)));
                customer_sections.push_back(inline_text_section(
                        "Можно оплатить",
                        join_human_readable(available_methods)));
            }

            if (!unavailable_methods.empty()) {
                fact_sections.push_back(inline_text_section(
                        "Не предусмотрено",
                        join_human_readable(unavailable_methods)));
                customer_unavailable_intro = std::format(
                        "Некоторые запрошенные способы оплаты у нас пока не поддерживаются: {}",
                        join_human_readable(unavailable_methods));
                customer_sections.push_back(*customer_unavailable_intro);
                customer_unavailable_section_index = customer_sections.size() - 1;
            }
        }
    }

    if (!request.certificate_validity && request.gift_certificate &&
        payment.gift_certificate && !payment.gift_certificate_note.empty()) {
        const auto note = sentence(capitalize_first(payment.gift_certificate_note));
        fact_sections.push_back(note);
        customer_sections.push_back(note);
    }

    if (request.prepayment) {
        const auto prepayment_text = payment.prepayment_required
                                             ? std::string{"Для записи требуется обязательная предоплата"}
                                             : std::string{"Обязательная предоплата не требуется"};
        fact_sections.push_back(sentence(prepayment_text));
        customer_sections.push_back(sentence(prepayment_text));
    }

    if (request.prepayment_paid) {
        fact_sections.push_back(
                "Внесённую предоплату необходимо подтвердить у администратора");
        customer_sections.push_back(
                "Если Вы уже вносили предоплату, сообщите об этом администратору — мы перепроверим платёж и учтём его при расчёте");
    }

    if (request.postpayment && !prioritize_postpayment) {
        const auto postpayment_text = payment.note.empty()
                                              ? std::string{
                                                        "Оплата производится после оказания услуги"}
                                              : capitalize_first(payment.note);
        fact_sections.push_back(postpayment_text);
        customer_sections.push_back(postpayment_text);
    }

    if (!payment.note.empty() && request.prepayment && !request.postpayment) {
        fact_sections.push_back(sentence(capitalize_first(payment.note)));
        customer_sections.push_back(sentence(capitalize_first(payment.note)));
    }

    if (has_unavailable_method) {
        const auto fact_alternatives = alternative_payment_method_labels(
                payment,
                request,
                false);
        const auto customer_alternatives = alternative_payment_method_labels(
                payment,
                request,
                true);

        if (!fact_alternatives.empty()) {
            if (!fact_sections.empty()) {
                fact_sections.back() = sentence(std::move(fact_sections.back()));
            }

            fact_sections.push_back(text_section(
                    "Доступные альтернативные способы оплаты",
                    em_dash_list(fact_alternatives)));

            const auto intro = customer_unavailable_intro.value_or(
                    "Некоторые способы оплаты у нас пока не поддерживаются");
            const auto alternatives_text = std::format(
                    "{}, но Вы можете оплатить услугу другими способами:\n{}",
                    intro,
                    em_dash_list(customer_alternatives));

            if (customer_unavailable_section_index.has_value()) {
                customer_sections[*customer_unavailable_section_index] = alternatives_text;
            } else {
                customer_sections.push_back(alternatives_text);
            }

            customer_sections.push_back(
                    "Уточните, пожалуйста, присутствует ли здесь удобный для Вас способ оплаты? И какой?[[friendly: 😊]]");
        } else if (!customer_sections.empty()) {
            customer_sections.back() = sentence(std::move(customer_sections.back()));
        }
    }

    if (fact_sections.empty()) {
        return std::nullopt;
    }

    auto answer = make_answer(config,
                              "payment",
                              join_strings(fact_sections, "\n\n"),
                              join_strings(customer_sections, "\n\n"));
    apply_payment_answer_emoji(answer, payment, request);
    return answer;
}

[[nodiscard]] std::string booking_method_label(
        const organization_booking_method_s &method) {
    if (!method.label.empty()) {
        return method.label;
    }

    if (method.id == "phone") {
        return "по телефону";
    }
    if (method.id == "website") {
        return "на сайте";
    }
    if (method.id == "telegram") {
        return "через Telegram";
    }
    if (method.id == "whatsapp") {
        return "через WhatsApp";
    }
    if (method.id == "max") {
        return "через MAX";
    }
    if (method.id == "yandex_maps") {
        return "через Яндекс Карты";
    }
    if (method.id == "2gis") {
        return "через 2ГИС";
    }
    if (method.id == "google_maps") {
        return "через Google Maps";
    }

    return method.id;
}

[[nodiscard]] std::string booking_method_title(
        const organization_booking_method_s &method) {
    return capitalize_first(booking_method_label(method));
}

[[nodiscard]] bool is_booking_placeholder_value(const std::string_view value) {
    const auto normalized = normalize_query(value);

    return normalized == "карточка организации" ||
           normalized == "карточка компании" ||
           normalized == "карточка заведения";
}

[[nodiscard]] std::string customer_booking_instruction(std::string instruction) {
    if (instruction.empty()) {
        return instruction;
    }

    const auto normalized = normalize_query(instruction);
    const auto normalized_first_end = normalized.find(' ');
    const auto normalized_first = normalized.substr(
            0,
            normalized_first_end == std::string::npos
                    ? normalized.size()
                    : normalized_first_end);

    constexpr auto replacements = std::array{
            std::pair{std::string_view{"позвонить"}, std::string_view{"Позвоните"}},
            std::pair{std::string_view{"написать"}, std::string_view{"Напишите"}},
            std::pair{std::string_view{"выбрать"}, std::string_view{"Выберите"}},
            std::pair{std::string_view{"нажать"}, std::string_view{"Нажмите"}},
            std::pair{std::string_view{"перейти"}, std::string_view{"Перейдите"}},
            std::pair{std::string_view{"открыть"}, std::string_view{"Откройте"}},
            std::pair{std::string_view{"заполнить"}, std::string_view{"Заполните"}},
            std::pair{std::string_view{"указать"}, std::string_view{"Укажите"}},
            std::pair{std::string_view{"оформить"}, std::string_view{"Оформите"}},
            std::pair{std::string_view{"связаться"}, std::string_view{"Свяжитесь"}},
    };

    for (const auto &[infinitive, imperative] : replacements) {
        if (normalized_first != infinitive) {
            continue;
        }

        const auto original_first_end = instruction.find(' ');
        auto result = std::string{imperative};

        if (original_first_end != std::string::npos) {
            result += instruction.substr(original_first_end);
        }

        return sentence(std::move(result));
    }

    return sentence(capitalize_first(std::move(instruction)));
}

[[nodiscard]] std::string booking_method_value_line(
        const organization_booking_method_s &method) {
    if (method.value.empty() || is_booking_placeholder_value(method.value)) {
        return {};
    }

    return method.value;
}

[[nodiscard]] std::string booking_map_platform(
        const organization_booking_method_s &method) {
    if (method.id == "yandex_maps") {
        return "Яндекс Картах";
    }

    if (method.id == "2gis") {
        return "2ГИС";
    }

    if (method.id == "google_maps") {
        return "Google Maps";
    }

    return {};
}

[[nodiscard]] std::string booking_method_details(
        const organization_config_s &config,
        const organization_booking_method_s &method,
        const bool customer_facing) {
    auto lines = std::vector<std::string>{};

    if (const auto value = booking_method_value_line(method); !value.empty()) {
        lines.push_back(value);
    }

    if (const auto platform = booking_map_platform(method);
        customer_facing && !platform.empty() &&
        is_booking_placeholder_value(method.value)) {
        if (config.brand_name.empty()) {
            lines.push_back(std::format("Откройте карточку организации в {}.", platform));
        } else {
            lines.push_back(std::format("Откройте карточку «{}» в {}.",
                                        config.brand_name,
                                        platform));
        }
    }

    if (!method.instructions.empty()) {
        lines.push_back(customer_facing
                                ? customer_booking_instruction(method.instructions)
                                : sentence(capitalize_first(method.instructions)));
    }

    return join_strings(lines, "\n");
}

[[nodiscard]] std::string booking_method_section(
        const organization_config_s &config,
        const organization_booking_method_s &method,
        const bool customer_facing) {
    const auto title = booking_method_title(method);
    const auto details = booking_method_details(config, method, customer_facing);
    return details.empty() ? title : text_section(title, details);
}

struct booking_request_s {
    bool general = false;
    std::vector<std::string> method_ids = {};

    [[nodiscard]] bool has_specific_methods() const noexcept {
        return !method_ids.empty();
    }

    [[nodiscard]] bool matched() const noexcept {
        return general || has_specific_methods();
    }
};

void append_requested_booking_method(booking_request_s &request,
                                     const std::string_view method_id) {
    if (method_id.empty() ||
        std::ranges::find(request.method_ids, method_id) != request.method_ids.end()) {
        return;
    }

    request.method_ids.emplace_back(method_id);
}

[[nodiscard]] const organization_booking_method_s *find_booking_method(
        const organization_config_s &config,
        const std::string_view id) noexcept {
    const auto it = std::ranges::find(config.booking_methods, id,
                                      &organization_booking_method_s::id);
    return it == config.booking_methods.end() ? nullptr : &*it;
}

[[nodiscard]] std::string short_regular_schedule_text(const organization_config_s &config) {
    if (config.schedule.regular.size() == 1 &&
        covers_every_day(config.schedule.regular.front())) {
        const auto &rule = config.schedule.regular.front();

        if (!rule.opens.empty() && !rule.closes.empty()) {
            return std::format("ежедневно с {} до {}", rule.opens, rule.closes);
        }
    }

    auto result = regular_schedule_text(config.schedule, true);

    if (result.starts_with("Мы работаем ")) {
        result.erase(0, std::string_view{"Мы работаем "}.size());
    }

    return result;
}

[[nodiscard]] std::vector<organization_booking_method_s> ordered_booking_methods(
        const organization_config_s &config) {
    constexpr auto order = std::array{
            std::string_view{"phone"},
            std::string_view{"yandex_maps"},
            std::string_view{"telegram"},
            std::string_view{"whatsapp"},
            std::string_view{"max"},
            std::string_view{"website"},
            std::string_view{"2gis"},
            std::string_view{"google_maps"},
    };

    auto result = std::vector<organization_booking_method_s>{};
    auto added = std::unordered_set<std::string>{};

    const auto add_method = [&](organization_booking_method_s method) {
        if (!method.enabled || method.id.empty() || !added.insert(method.id).second) {
            return;
        }

        result.push_back(std::move(method));
    };

    for (const auto id : order) {
        if (const auto *method = find_booking_method(config, id); method != nullptr) {
            add_method(*method);
        }
    }

    if (find_booking_method(config, "telegram") == nullptr && !config.contacts.telegram.empty()) {
        add_method(organization_booking_method_s{
                .id = "telegram",
                .enabled = true,
                .label = "через Telegram",
                .value = config.contacts.telegram,
                .instructions = "написать администратору",
        });
    }

    if (find_booking_method(config, "whatsapp") == nullptr && !config.contacts.whatsapp.empty()) {
        add_method(organization_booking_method_s{
                .id = "whatsapp",
                .enabled = true,
                .label = "через WhatsApp",
                .value = config.contacts.whatsapp,
                .instructions = "написать администратору",
        });
    }

    if (find_booking_method(config, "max") == nullptr && !config.contacts.max.empty()) {
        add_method(organization_booking_method_s{
                .id = "max",
                .enabled = true,
                .label = "через MAX",
                .value = config.contacts.max,
                .instructions = "написать администратору",
        });
    }

    for (const auto &method : config.booking_methods) {
        add_method(method);
    }

    return result;
}

[[nodiscard]] std::optional<emoji_kind_e> booking_method_emoji_kind(
        const std::string_view method_id) noexcept {
    if (method_id == "phone") return emoji_kind_e::phone;
    if (method_id == "website") return emoji_kind_e::website;
    if (method_id == "telegram") return emoji_kind_e::telegram;
    if (method_id == "whatsapp") return emoji_kind_e::whatsapp;
    if (method_id == "max") return emoji_kind_e::max;
    if (method_id == "yandex_maps" || method_id == "2gis" ||
        method_id == "google_maps") {
        return emoji_kind_e::maps;
    }

    return std::nullopt;
}

[[nodiscard]] std::vector<std::string> alternative_booking_method_labels(
        const organization_config_s &config,
        const booking_request_s &request,
        const bool customer_facing) {
    auto labels = std::vector<std::string>{};

    for (const auto &method : ordered_booking_methods(config)) {
        if (std::ranges::find(request.method_ids, method.id) != request.method_ids.end()) {
            continue;
        }

        auto label = booking_method_label(method);
        if (customer_facing) {
            if (const auto emoji = booking_method_emoji_kind(method.id); emoji.has_value()) {
                label = with_friendly_emoji(std::move(label), *emoji);
            }
        }

        labels.push_back(std::move(label));
    }

    return labels;
}

[[nodiscard]] std::string booking_phone_section(const organization_config_s &config,
                                                const organization_booking_method_s &method) {
    auto lines = std::vector<std::string>{};

    if (const auto value = booking_method_value_line(method); !value.empty()) {
        lines.push_back(value);
    }

    if (const auto schedule = short_regular_schedule_text(config); !schedule.empty()) {
        lines.push_back(std::format("Позвоните в часы работы салона — {}.", schedule));
    } else if (!method.instructions.empty()) {
        lines.push_back(customer_booking_instruction(method.instructions));
    }

    lines.push_back("Мы всегда рады вашему звонку 😊");
    return text_section("По телефону", join_strings(lines, "\n"));
}

[[nodiscard]] std::string booking_yandex_maps_section(
        const organization_config_s &config) {
    const auto brand = config.brand_name.empty()
                               ? std::string{"название нашей организации"}
                               : std::format("наше название — «{}»", config.brand_name);

    auto lines = std::vector<std::string>{
            "1. Зайдите на Яндекс.Карты с телефона или компьютера: https://yandex.ru/maps.",
            std::format("2. В поиске напишите {}.", brand),
            "3. Откройте найденную карточку и сверьтесь с адресом на карте.",
            "4. Нажмите кнопку онлайн-записи, выберите удобную дату и время. И всё — готово! 🎉🎊",
    };

    return text_section("Через Яндекс.Карты", join_strings(lines, "\n"));
}

[[nodiscard]] std::string booking_messenger_display_name(const std::string_view method_id) {
    if (method_id == "telegram") return "Telegram";
    if (method_id == "whatsapp") return "WhatsApp";
    if (method_id == "max") return "MAX";
    return std::string{method_id};
}

[[nodiscard]] std::string booking_messenger_section(
        const organization_booking_method_s &method) {
    const auto display_name = booking_messenger_display_name(method.id);
    const auto value = booking_method_value_line(method);
    auto lines = std::vector<std::string>{};

    if (!value.empty()) {
        lines.push_back(std::format("1. В поиске {} укажите: {}.", display_name, value));
        lines.push_back("2. Обратите внимание на название и картинку профиля, чтобы выбрать наш аккаунт.");
        lines.push_back("3. Напишите на этот аккаунт — администратор с радостью поможет Вам записаться.");
    } else {
        lines.push_back(std::format("Напишите нам через {} — администратор поможет Вам записаться.",
                                    display_name));
    }

    if (method.id == "max") {
        lines.push_back("Примечание: поиск в MAX может работать нестабильно. Если наш профиль не находится, пожалуйста, воспользуйтесь другим способом записи. Приносим извинения за неудобства, эта техническая проблема не с нашей стороны 😅");
    }

    return text_section(std::format("Через {}", display_name), join_strings(lines, "\n"));
}

[[nodiscard]] std::string booking_website_section(
        const organization_booking_method_s &method) {
    auto lines = std::vector<std::string>{};

    if (const auto value = booking_method_value_line(method); !value.empty()) {
        lines.push_back(value);
    }

    if (!method.instructions.empty()) {
        lines.push_back(customer_booking_instruction(method.instructions));
    }

    return text_section("На сайте", join_strings(lines, "\n"));
}

[[nodiscard]] std::string rich_booking_method_section(
        const organization_config_s &config,
        const organization_booking_method_s &method) {
    if (method.id == "phone") {
        return booking_phone_section(config, method);
    }

    if (method.id == "yandex_maps") {
        return booking_yandex_maps_section(config);
    }

    if (method.id == "telegram" || method.id == "whatsapp" || method.id == "max") {
        return booking_messenger_section(method);
    }

    if (method.id == "website") {
        return booking_website_section(method);
    }

    return booking_method_section(config, method, true);
}

[[nodiscard]] std::optional<organization_config_answer_s> general_booking_answer(
        const organization_config_s &config) {
    auto methods = ordered_booking_methods(config);

    if (methods.empty()) {
        return std::nullopt;
    }

    auto fact_sections = std::vector<std::string>{};
    auto customer_sections = std::vector<std::string>{};
    fact_sections.reserve(methods.size());
    customer_sections.reserve(methods.size() + 1);

    for (const auto &method : methods) {
        fact_sections.push_back(booking_method_section(config, method, false));
        customer_sections.push_back(rich_booking_method_section(config, method));
    }

    auto fact = std::string{"Доступные способы записи:"};
    fact += "\n\n";
    fact += join_strings(fact_sections, "\n\n");

    auto customer = config.brand_name.empty()
                            ? std::string{"Записаться можно несколькими способами:"}
                            : std::format("Записаться в «{}» можно несколькими способами:",
                                          config.brand_name);
    customer += "\n\n";
    customer += join_strings(customer_sections, "\n\n");
    customer += "\n\n";
    customer += "При возникновении трудностей — пишите! С удовольствием помогу Вам разобраться с записью 😊";

    auto answer = make_answer(config, "booking", std::move(fact), std::move(customer));
    answer.emoji.clear();
    return answer;
}

[[nodiscard]] std::string specific_booking_customer_text(
        const organization_config_s &config,
        const organization_booking_method_s &method,
        const std::string_view normalized_query) {
    const auto value = booking_method_value_line(method);

    if (!method.enabled) {
        return std::format("Запись {} у нас пока не поддерживается",
                           booking_method_label(method));
    }

    if (method.id == "phone") {
        auto lines = std::vector<std::string>{};

        if (!value.empty()) {
            lines.push_back(std::format("Для записи позвоните по номеру {}", value));
        } else {
            lines.push_back("Для записи позвоните нам по телефону");
        }

        if (const auto schedule = short_regular_schedule_text(config); !schedule.empty()) {
            lines.push_back(std::format("Лучше звонить в часы работы салона — {}", schedule));
        }

        lines.push_back("Мы всегда рады вашему звонку 😊");
        return join_strings(lines, "\n");
    }

    if (method.id == "telegram" || method.id == "whatsapp" || method.id == "max") {
        return booking_messenger_section(method);
    }

    if (method.id == "yandex_maps") {
        return booking_yandex_maps_section(config);
    }

    if (const auto platform = booking_map_platform(method); !platform.empty()) {
        auto customer = std::string{};

        if (is_booking_placeholder_value(method.value)) {
            const auto method_label = booking_method_label(method);
            customer = config.brand_name.empty()
                               ? std::format("Чтобы записаться {}, откройте карточку организации",
                                             method_label)
                               : std::format("Чтобы записаться {}, откройте карточку «{}»",
                                             method_label,
                                             config.brand_name);
        } else if (!value.empty()) {
            customer = std::format("Записаться {} можно по ссылке: {}",
                                   booking_method_label(method),
                                   value);
        } else {
            customer = std::format("Записаться {} можно в карточке организации",
                                   booking_method_label(method));
        }

        if (!method.instructions.empty()) {
            customer = append_sentence(std::move(customer),
                                       customer_booking_instruction(method.instructions));
        }

        return customer;
    }

    if (method.id == "website") {
        return booking_website_section(method);
    }

    auto customer = std::format("Записаться {} можно", booking_method_label(method));

    if (asks_how(normalized_query)) {
        if (!value.empty()) {
            customer = append_sentence(std::move(customer), value);
        }
        if (!method.instructions.empty()) {
            customer = append_sentence(std::move(customer),
                                       customer_booking_instruction(method.instructions));
        }
    }

    return customer;
}

[[nodiscard]] std::optional<organization_config_answer_s> specific_booking_answer(
        const organization_config_s &config,
        const booking_request_s &request,
        const std::string_view normalized_query) {
    auto fact_sections = std::vector<std::string>{};
    auto customer_sections = std::vector<std::string>{};
    auto has_disabled_method = false;

    for (const auto &method_id : request.method_ids) {
        const auto *const method = find_booking_method(config, method_id);

        if (method == nullptr) {
            continue;
        }

        has_disabled_method = has_disabled_method || !method->enabled;

        auto fact = method->enabled
                            ? std::format("Запись {} доступна", booking_method_label(*method))
                            : std::format("Запись {} недоступна", booking_method_label(*method));

        if (method->enabled) {
            if (const auto details = booking_method_details(config, *method, false);
                !details.empty()) {
                fact = append_sentence(std::move(fact), details);
            }
        }

        fact_sections.push_back(std::move(fact));
        customer_sections.push_back(specific_booking_customer_text(config,
                                                                    *method,
                                                                    normalized_query));
    }

    if (fact_sections.empty()) {
        return std::nullopt;
    }

    if (has_disabled_method) {
        const auto fact_alternatives = alternative_booking_method_labels(
                config,
                request,
                false);
        const auto customer_alternatives = alternative_booking_method_labels(
                config,
                request,
                true);

        if (!fact_alternatives.empty()) {
            if (!fact_sections.empty()) {
                fact_sections.back() = sentence(std::move(fact_sections.back()));
            }

            fact_sections.push_back(text_section(
                    "Доступные альтернативные способы записи",
                    em_dash_list(fact_alternatives)));

            if (request.method_ids.size() == 1 && customer_sections.size() == 1) {
                customer_sections.back() = std::format(
                        "{}, но Вы можете воспользоваться другими способами записи:\n{}",
                        customer_sections.back(),
                        em_dash_list(customer_alternatives));
            } else {
                customer_sections.push_back(std::format(
                        "Некоторые способы записи у нас пока не поддерживаются, но Вы можете воспользоваться другими вариантами:\n{}",
                        em_dash_list(customer_alternatives)));
            }

            customer_sections.push_back(
                    "Уточните, пожалуйста, какой способ записи для Вас удобнее?[[friendly: 😊]]");
        }
    }

    auto answer = make_answer(config,
                              "booking",
                              join_strings(fact_sections, "\n\n"),
                              join_strings(customer_sections, "\n\n"));
    answer.emoji.clear();
    return answer;
}

[[nodiscard]] std::optional<organization_config_answer_s> booking_answer(
        const organization_config_s &config,
        const booking_request_s &request,
        const std::string_view normalized_query) {
    if (request.has_specific_methods()) {
        return specific_booking_answer(config, request, normalized_query);
    }

    return request.general ? general_booking_answer(config) : std::nullopt;
}

[[nodiscard]] std::string organization_phone_owner(
        const organization_business_type_e type) {
    switch (type) {
        case organization_business_type_e::beauty_salon:
            return "нашего салона";
        case organization_business_type_e::coffee_shop:
            return "нашей кофейни";
        case organization_business_type_e::gas_station:
            return "нашей АЗС";
        case organization_business_type_e::gym:
            return "нашего спортзала";
        case organization_business_type_e::unknown:
            return "нашей организации";
    }

    return "нашей организации";
}

[[nodiscard]] std::optional<organization_config_answer_s> phone_answer(
        const organization_config_s &config) {
    if (config.contacts.phone.empty()) {
        return std::nullopt;
    }

    const auto owner = organization_phone_owner(config.business_type);
    const auto phone_line = std::format("Номер телефона {}: {}",
                                        owner,
                                        config.contacts.phone);
    auto customer = sentence(phone_line);

    if (config.schedule.regular.size() == 1 &&
        covers_every_day(config.schedule.regular.front())) {
        const auto &rule = config.schedule.regular.front();

        if (!rule.opens.empty() && !rule.closes.empty()) {
            customer += "\n\n";
            customer += sentence(std::format(
                    "Звоните в часы работы {} — ежедневно с {} до {}",
                    owner,
                    rule.opens,
                    rule.closes));
        }
    } else if (const auto schedule = regular_schedule_text(config.schedule, true);
               !schedule.empty()) {
        customer += "\n\n";
        customer += sentence(std::format("Звоните в часы работы {}. {}",
                                         owner,
                                         schedule));
    }

    auto answer = make_answer(config,
                              "phone",
                              inline_text_section("Телефон", config.contacts.phone),
                              std::move(customer));

    if (const auto smile = emoji_for("smile"); !smile.empty()) {
        answer.emoji = smile;
    } else {
        answer.emoji = "😊";
    }

    return answer;
}

[[nodiscard]] std::optional<organization_config_answer_s> messenger_answer(
        const organization_config_s &config,
        const std::string_view topic,
        const std::string_view display_name,
        const std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }

    return make_answer(config,
                       std::string{topic},
                       inline_text_section(display_name, value),
                       std::format("Да, мы есть в {}: {}", display_name, value));
}

[[nodiscard]] std::optional<organization_config_answer_s> map_listing_answer(
        const organization_config_s &config,
        const std::string_view method_id,
        const std::string_view platform_location) {
    const auto *const method = find_booking_method(config, method_id);

    if (method == nullptr || !method->enabled) {
        return std::nullopt;
    }

    auto fact_parts = std::vector<std::string>{
            sentence(std::format("У организации есть карточка {}", platform_location)),
    };
    auto customer_parts = std::vector<std::string>{
            sentence(std::format("Да, мы есть {}", platform_location)),
    };

    if (const auto value = booking_method_value_line(*method); !value.empty()) {
        fact_parts.push_back(value);
        customer_parts.push_back(value);
    } else if (!config.brand_name.empty()) {
        customer_parts.push_back(sentence(
                std::format("Найдите {} и откройте карточку организации",
                            std::format("«{}»", config.brand_name))));
    }

    if (!method->instructions.empty()) {
        fact_parts.push_back(sentence(capitalize_first(method->instructions)));
        customer_parts.push_back(customer_booking_instruction(method->instructions));
    }

    return make_answer(config,
                       "maps",
                       join_strings(fact_parts, "\n\n"),
                       join_strings(customer_parts, "\n\n"));
}

[[nodiscard]] std::optional<organization_config_answer_s> contacts_answer(
        const organization_config_s &config) {
    auto fact_sections = std::vector<std::string>{};
    auto customer_sections = std::vector<std::string>{};

    const auto append_contact = [&](const std::string_view label,
                                    const std::string_view value) {
        if (value.empty()) {
            return;
        }

        fact_sections.push_back(inline_text_section(label, value));
        customer_sections.push_back(inline_text_section(label, value));
    };

    append_contact("Телефон", config.contacts.phone);
    append_contact("Telegram", config.contacts.telegram);
    append_contact("WhatsApp", config.contacts.whatsapp);
    append_contact("MAX", config.contacts.max);

    if (fact_sections.empty()) {
        return std::nullopt;
    }

    return make_answer(config,
                       "phone",
                       std::format("{}\n\n{}",
                                   std::string{"Контакты организации"},
                                   join_strings(fact_sections, "\n\n")),
                       std::format("{}\n\n{}",
                                   std::string{"Связаться с нами можно несколькими способами"},
                                   join_strings(customer_sections, "\n\n")));
}

[[nodiscard]] std::optional<organization_config_answer_s> website_answer(
        const organization_config_s &config) {
    if (config.contacts.website.empty()) {
        return std::nullopt;
    }

    return make_answer(config,
                       "website",
                       std::format("Сайт организации: {}", config.contacts.website),
                       std::format("Наш сайт: {}", config.contacts.website));
}

[[nodiscard]] organization_config_answer_s boolean_feature_answer(
        const organization_config_s &config,
        std::string topic,
        const bool available,
        const std::string_view fact_name,
        const std::string_view customer_yes,
        const std::string_view customer_no,
        const std::string_view note) {
    auto fact = std::format("{}: {}", fact_name, available ? "да" : "нет");
    auto customer = std::string{available ? customer_yes : customer_no};

    fact = append_sentence(std::move(fact), note);
    customer = append_sentence(std::move(customer), note);

    return make_answer(config, std::move(topic), std::move(fact), std::move(customer));
}

[[nodiscard]] std::optional<organization_config_answer_s> smoking_answer(
        const organization_config_s &config) {
    const auto &amenities = config.general_amenities;

    if (amenities.smoking_allowed) {
        auto customer = std::string{"Да, курение разрешено в обозначенной зоне"};

        if (!amenities.smoking_note.empty()) {
            customer = append_sentence(std::move(customer), amenities.smoking_note);
        }

        return make_answer(config,
                           "smoking",
                           "Курение: разрешено",
                           std::move(customer));
    }

    auto customer = std::string{
            "По правилам нашей организации курить внутри помещения, включая электронные сигареты и системы нагревания табака, запрещено.[[friendly: 🚭]]"};

    if (config.business_type == organization_business_type_e::beauty_salon) {
        customer =
                "По правилам нашего салона курить внутри помещения, включая электронные сигареты и системы нагревания табака, запрещено.[[friendly: 🚭]]";
    }

    if (amenities.smoke_breaks_allowed) {
        const auto break_text =
                config.business_type == organization_business_type_e::beauty_salon
                        ? std::string{
                                  "Но Вы всегда можете выйти на улицу.[[friendly: 😏]] Единственный момент: в процессе процедуры паузы нежелательны, так как они могут повлиять на итоговый результат.[[friendly: 💅🏻✨]] Просто скажите мастеру перед началом, что Вам понадобится перерыв, и Вы вместе выберете идеальное время для паузы.[[friendly: 😊🤝😊]]"}
                        : std::string{
                                  "Но Вы всегда можете выйти на улицу.[[friendly: 😏]] Единственный момент: в процессе обслуживания паузы могут быть нежелательны. Просто заранее скажите сотруднику, что Вам понадобится перерыв, и вы вместе выберете подходящее время.[[friendly: 😊🤝😊]]"};
        customer += "\n\n";
        customer += break_text;
    }

    auto fact = std::string{"Курение внутри помещения: запрещено"};
    if (amenities.smoke_breaks_allowed) {
        fact = append_sentence(std::move(fact),
                               "Перекуры на улице возможны по предварительному согласованию");
    }

    return make_answer(config,
                       "smoking",
                       std::move(fact),
                       std::move(customer));
}

[[nodiscard]] std::optional<organization_config_answer_s> coffee_for_guests_answer(
        const organization_config_s &config) {
    const auto &amenities = config.general_amenities;
    auto customer = amenities.has_free_coffee
                            ? std::string{"Да, мы предлагаем гостям кофе"}
                            : std::string{"Бесплатный кофе для гостей не предусмотрен"};

    if (!amenities.coffee_note.empty()) {
        if (amenities.has_free_coffee &&
            normalize_query(amenities.coffee_note) ==
                    "кофе предлагают гостям во время ожидания") {
            customer = "Да, во время ожидания мы предлагаем гостям кофе";
        } else {
            customer = amenities.has_free_coffee
                               ? affirmative_note(amenities.coffee_note)
                               : append_sentence(std::move(customer), amenities.coffee_note);
        }
    }

    return make_answer(config,
                       "coffee",
                       std::format("Бесплатный кофе для гостей: {}",
                                   amenities.has_free_coffee ? "да" : "нет"),
                       std::move(customer));
}

[[nodiscard]] std::optional<organization_config_answer_s> child_zone_answer(
        const organization_config_s &config) {
    const auto &amenities = config.general_amenities;
    auto customer = std::string{};

    if (amenities.has_child_zone) {
        customer = "Да, у нас есть детская зона";
        if (!amenities.child_zone_note.empty()) {
            customer = append_sentence(std::move(customer), amenities.child_zone_note);
        }
    } else {
        customer =
                "Отдельной детской зоны у нас, к сожалению, нет[[friendly: 🧸]]. Но в зоне ожидания есть удобный диван. Как вариант, Вы можете взять с собой планшет[[friendly: 📱]] или раскраску[[friendly: 🎨]], чтобы занять ребёнка[[friendly: 👼]]";
    }

    return make_answer(config,
                       "child_zone",
                       std::format("Детская зона: {}",
                                   amenities.has_child_zone ? "да" : "нет"),
                       std::move(customer));
}

[[nodiscard]] std::optional<organization_config_answer_s> gift_certificate_answer(
        const organization_config_s &config) {
    const auto &amenities = config.general_amenities;
    auto customer = amenities.has_gift_certificates
                            ? std::string{"Да, у нас можно приобрести подарочный сертификат"}
                            : std::string{"Подарочные сертификаты не предусмотрены"};

    if (amenities.has_gift_certificates &&
        !amenities.gift_certificates_note.empty()) {
        customer = affirmative_note(amenities.gift_certificates_note);
    }

    return make_answer(config,
                       "gift_certificate",
                       std::format("Подарочные сертификаты: {}",
                                   amenities.has_gift_certificates ? "да" : "нет"),
                       std::move(customer));
}

[[nodiscard]] std::optional<organization_config_answer_s> staff_call_button_answer(
        const organization_config_s &config,
        const std::string_view normalized_query) {
    const auto &amenities = config.general_amenities;

    if (!amenities.has_staff_call_button) {
        auto customer = std::string{
                "Кнопки вызова на улице у нас в данный момент действительно нет, но мы всегда на связи и готовы помочь![[friendly: 😉]]"};

        customer += "\n\n";
        if (!config.contacts.phone.empty()) {
            const auto destination =
                    config.business_type == organization_business_type_e::beauty_salon
                            ? std::string_view{"к салону"}
                            : std::string_view{"к нам"};
            customer += std::format(
                    "Пожалуйста, когда будете подходить {}, наберите нас по номеру[[friendly: 📞]] {}. Наш сотрудник выйдет к Вам на улицу, встретит и поможет со всеми вопросами.[[friendly: 😊]] Мы сделаем всё, чтобы Ваш визит прошёл максимально комфортно![[friendly: 🌿 ✨]]",
                    destination,
                    config.contacts.phone);
        } else {
            customer +=
                    "Пожалуйста, когда будете подходить, заранее свяжитесь с нами удобным способом. Наш сотрудник выйдет к Вам на улицу, встретит и поможет со всеми вопросами.[[friendly: 😊]] Мы сделаем всё, чтобы Ваш визит прошёл максимально комфортно![[friendly: 🌿 ✨]]";
        }

        auto answer = make_answer(config,
                                  "staff_call_button",
                                  "Уличная кнопка вызова персонала: нет",
                                  std::move(customer));
        answer.emoji.clear();
        return answer;
    }

    auto customer = amenities.staff_call_button_note.empty()
                            ? std::string{
                                      "Кнопка вызова персонала для помощи на улице расположена у входа"}
                            : location_note(
                                      "Кнопка вызова персонала для помощи на улице",
                                      amenities.staff_call_button_note);

    if (!asks_location(normalized_query)) {
        customer = sentence(std::format("Да, {}", lowercase_first(customer)));
    }

    customer = append_sentence(
            std::move(customer),
            "Смело нажимайте на неё — мы с радостью поможем Вам попасть к нам в помещение для обслуживания");

    auto answer = make_answer(config,
                              "staff_call_button",
                              "Уличная кнопка вызова персонала: да",
                              std::move(customer));
    answer.emoji = "😉 🌸";
    return answer;
}

[[nodiscard]] std::optional<organization_config_answer_s> ramp_answer(
        const organization_config_s &config,
        const std::string_view normalized_query) {
    const auto &amenities = config.general_amenities;

    if (!amenities.has_ramp) {
        auto customer = std::string{
                "Стационарного пандуса у входа сейчас нет, но мы обязательно поможем Вам комфортно попасть внутрь. Мы очень ценим Вашу доступность и удобство![[friendly: 🙌]]"};

        customer += "\n\n";
        if (!config.contacts.phone.empty()) {
            const auto destination =
                    config.business_type == organization_business_type_e::beauty_salon
                            ? std::string_view{"к салону"}
                            : std::string_view{"к нам"};
            customer += std::format(
                    "Пожалуйста, позвоните нам по номеру[[friendly: 📞]] {} перед выездом или когда будете подходить {}. Наш сотрудник встретит Вас на улице и бережно поможет зайти в помещение. Будем искренне рады встрече![[friendly: 😌 🌿]]",
                    config.contacts.phone,
                    destination);
        } else {
            customer +=
                    "Пожалуйста, заранее свяжитесь с нами перед выездом или когда будете подходить. Наш сотрудник встретит Вас на улице и бережно поможет зайти в помещение. Будем искренне рады встрече![[friendly: 😌 🌿]]";
        }

        auto answer = make_answer(config,
                                  "ramp",
                                  "Пандус: нет",
                                  std::move(customer));
        answer.emoji.clear();
        return answer;
    }

    auto customer = amenities.ramp_note.empty()
                            ? std::string{"Пандус расположен у основного входа"}
                            : location_note("Пандус", amenities.ramp_note);

    if (!asks_location(normalized_query)) {
        customer = sentence(std::format("Да, {}", lowercase_first(customer)));
    }

    return make_answer(config,
                       "ramp",
                       "Пандус: да",
                       std::move(customer));
}

[[nodiscard]] std::optional<organization_config_answer_s> parking_answer(
        const organization_config_s &config,
        const bool accessible_only,
        const std::string_view normalized_query) {
    const auto &parking = config.general_amenities.parking;
    (void) normalized_query;

    const auto appointment_word =
            config.business_type == organization_business_type_e::beauty_salon
                    ? std::string_view{"процедурой"}
                    : std::string_view{"визитом"};

    if (accessible_only) {
        auto customer = std::string{};

        if (!parking.accessible) {
            customer = "Отдельного парковочного места для людей с инвалидностью нет";
        } else {
            if (parking.accessible_note.contains("рядом со входом")) {
                customer =
                        "Да, рядом со входом есть отдельное парковочное место для людей с инвалидностью.[[friendly: ♿]]";
            } else if (!parking.accessible_note.empty()) {
                customer = sentence(std::format(
                        "Да, парковочное место для людей с инвалидностью {}",
                        lowercase_first(parking.accessible_note)));
                customer += "[[friendly: ♿]]";
            } else {
                customer =
                        "Да, рядом есть отдельное парковочное место для людей с инвалидностью.[[friendly: ♿]]";
            }

            customer +=
                    " Обычно оно свободно, но в часы пик может быть плотно. Если место будет занято, можно припарковаться во дворах или поставить автомобиль на платную городскую парковку. ";
            customer += std::format(
                    "Рекомендуем заложить несколько запасных минут (20-30) перед {}, чтобы спокойно припарковаться и добраться до нас.[[friendly: 😌 🌸]]",
                    appointment_word);
        }

        auto answer = make_answer(
                config,
                "accessible_parking",
                append_sentence(
                        std::format("Парковка для людей с инвалидностью: {}",
                                    parking.accessible ? "да" : "нет"),
                        parking.accessible_note),
                std::move(customer));
        answer.emoji.clear();
        return answer;
    }

    auto customer = std::string{};

    if (!parking.regular) {
        customer = "Собственной парковки нет";
    } else {
        const auto has_free_city_parking =
                parking.regular_note.contains("бесплатн") &&
                parking.regular_note.contains("городск");
        const auto is_in_front_of_building =
                parking.regular_note.contains("перед зданием");

        if (has_free_city_parking && is_in_front_of_building) {
            customer =
                    "Да, прямо перед зданием есть бесплатная городская парковка.[[friendly: 🅿️]]";
        } else if (!parking.regular_note.empty()) {
            auto short_note = parking.regular_note;
            if (const auto separator = short_note.find(';'); separator != std::string::npos) {
                short_note.erase(separator);
            }
            customer = sentence(std::format("Да, {}", lowercase_first(short_note)));
            customer += "[[friendly: 🅿️]]";
        } else {
            customer = "Да, рядом есть парковка.[[friendly: 🅿️]]";
        }

        customer +=
                " Обычно свободные места есть, но в часы пик может быть плотно. Если мест не будет, можно припарковаться во дворах или поставить автомобиль на платную городскую парковку. ";
        customer += std::format(
                "Рекомендуем заложить несколько запасных минут (10-15) перед {}, чтобы спокойно припарковаться.[[friendly: 😌 🌸]]",
                appointment_word);
    }

    auto answer = make_answer(config,
                              "parking",
                              append_sentence(
                                      std::format("Обычная парковка: {}",
                                                  parking.regular ? "да" : "нет"),
                                      parking.regular_note),
                              std::move(customer));
    if (parking.regular) {
        answer.emoji.clear();
    }
    return answer;
}

[[nodiscard]] std::optional<std::size_t> extract_centimeters(
        const std::string_view normalized_query) noexcept {
    auto words = std::vector<std::string_view>{};
    auto offset = std::size_t{0};

    while (offset < normalized_query.size()) {
        const auto end = normalized_query.find(' ', offset);
        const auto word = normalized_query.substr(
                offset,
                end == std::string_view::npos
                        ? normalized_query.size() - offset
                        : end - offset);

        if (!word.empty()) {
            words.push_back(word);
        }

        if (end == std::string_view::npos) {
            break;
        }

        offset = end + 1;
    }

    for (auto index = std::size_t{0}; index < words.size(); ++index) {
        auto value = std::size_t{0};
        const auto word = words[index];
        const auto [end, error] = std::from_chars(word.data(),
                                                  word.data() + word.size(),
                                                  value);

        if (error != std::errc{} || end != word.data() + word.size()) {
            continue;
        }

        const auto has_unit = index + 1 < words.size() &&
                              contains_any_word_prefix(words[index + 1],
                                                       {
                                                               "см",
                                                               "сантиметр",
                                                           });

        if (has_unit && value <= 300) {
            return value;
        }
    }

    return std::nullopt;
}

[[nodiscard]] std::string organization_visit_location(
        const organization_config_s &config) {
    return config.business_type == organization_business_type_e::beauty_salon
                   ? std::string{"в нашем салоне"}
                   : std::string{"у нас"};
}

[[nodiscard]] std::string pets_behavior_rule() {
    return "Животное не должно быть агрессивным и вести себя слишком вызывающе";
}

[[nodiscard]] std::string pets_service_animal_behavior_rule() {
    return
            "Собака-проводник не должна быть агрессивной и доставлять неудобства другим клиентам";
}

[[nodiscard]] std::string pets_safety_notice() {
    return
            "Дополнительно предупреждаем, что у нас нет возможности внимательно следить[[friendly: 👁👁]] за тем, не съест ли с пола питомец что-то вредное или опасное для своего здоровья";
}

[[nodiscard]] std::string pets_advance_notice(
        const organization_pets_policy_s &policy) {
    if (policy.note.empty()) {
        return {};
    }

    return
            "Если в итоге решите прийти с животным, очень просим заранее предупредить администратора, чтобы избежать недопонимания и неловких ситуаций";
}

[[nodiscard]] std::string pets_service_animal_note(
        const organization_pets_policy_s &policy,
        const bool has_size_limit) {
    if (!policy.service_animals_allowed) {
        return {};
    }

    const auto introduction = has_size_limit
                                      ? std::string{
                                                "Примечание: ограничение по размеру не распространяется на собак-проводников[[friendly: 🐶👩‍🦯]]"}
                                      : std::string{
                                                "Примечание: исключением являются только собаки-проводники[[friendly: 🐶👩‍🦯]]"};

    const auto behavior = std::string{
            "При этом собака-проводник не должна быть агрессивной и доставлять неудобства другим клиентам"};
    return append_sentence(introduction, behavior);
}

[[nodiscard]] organization_config_answer_s make_pets_answer(
        const organization_config_s &config,
        std::string fact,
        std::string customer,
        std::string emoji) {
    auto answer = make_answer(config, "pets", std::move(fact), std::move(customer));
    answer.emoji = std::move(emoji);
    return answer;
}

[[nodiscard]] std::string pets_allowed_rules(
        const organization_pets_policy_s &policy) {
    auto paragraphs = std::vector<std::string>{
            sentence(pets_behavior_rule()),
            sentence(pets_safety_notice()),
    };

    if (const auto notice = pets_advance_notice(policy); !notice.empty()) {
        paragraphs.push_back(sentence(notice));
    }

    return join_strings(paragraphs, "\n\n");
}

[[nodiscard]] std::string full_pets_policy_text(
        const organization_config_s &config,
        const organization_pets_policy_s &policy) {
    const auto location = organization_visit_location(config);
    auto paragraphs = std::vector<std::string>{};

    if (policy.allowed) {
        paragraphs.push_back(sentence(std::format(
                "О, мы очень любим животных![[friendly: 🥰]] Конечно же, мы будем рады видеть Вас {} с Вашим домашним любимцем![[friendly: 🤩 🐾]] Единственное, хотелось бы отметить, что животное, в свою очередь, не должно быть агрессивным и вести себя слишком вызывающе",
                location)));
        paragraphs.push_back(sentence(pets_safety_notice()));

        auto invitation = std::string{
                "А так, если всё хорошо, ждём не только Вас, но и Вашего друга[[friendly: 🐾😊 🌸]]"};

        if (const auto notice = pets_advance_notice(policy); !notice.empty()) {
            invitation = append_sentence(std::move(invitation), notice);
        }

        paragraphs.push_back(std::move(invitation));

        if (policy.service_animals_allowed) {
            paragraphs.push_back(sentence(
                    "Примечание: указанные правила поведения распространяются и на собак-проводников[[friendly: 🐶👩‍🦯]]"));
        }

        return join_strings(paragraphs, "\n\n");
    }

    if (policy.small_dogs_allowed) {
        const auto allowed_pet = policy.max_dog_height_cm.has_value()
                                         ? std::format("небольшой собакой высотой до {} см",
                                                       *policy.max_dog_height_cm)
                                         : std::string{"небольшой собакой"};

        paragraphs.push_back(sentence(std::format(
                "О, мы очень любим животных![[friendly: 🥰]] Конечно же, мы будем рады видеть Вас {} с Вашим домашним любимцем![[friendly: 🤩 🐾]] Однако есть некоторые ограничения, о которых важно сказать. Например, к нам можно прийти с {}. Также животное не должно быть агрессивным и вести себя слишком вызывающе",
                location,
                allowed_pet)));
        paragraphs.push_back(sentence(pets_safety_notice()));

        auto invitation = std::string{
                "А так, если всё хорошо, ждём не только Вас, но и Вашего друга[[friendly: 🐾😊 🌸]]"};

        if (const auto notice = pets_advance_notice(policy); !notice.empty()) {
            invitation = append_sentence(std::move(invitation), notice);
        }

        paragraphs.push_back(std::move(invitation));

        if (const auto note = pets_service_animal_note(policy, true); !note.empty()) {
            paragraphs.push_back(note);
        }

        return join_strings(paragraphs, "\n\n");
    }

    paragraphs.push_back(sentence(std::format(
            "О, мы очень любим животных![[friendly: 🥰]] Но, к сожалению, {} запрещено присутствие с любыми домашними любимцами в связи с тем, что это может доставить моральный и/или физический дискомфорт другим клиентам",
            location)));
    paragraphs.push_back(sentence(
            "Помимо этого, тоже довольно важный момент: у нас нет возможности внимательно следить[[friendly: 👁👁]] за тем, не съест ли с пола питомец что-то вредное или опасное для своего здоровья"));
    paragraphs.push_back(sentence(
            "Надеемся на Ваше понимание[[friendly: 😊 🌸]]"));

    if (const auto note = pets_service_animal_note(policy, false); !note.empty()) {
        paragraphs.push_back(note);
    }

    return join_strings(paragraphs, "\n\n");
}

[[nodiscard]] std::optional<organization_config_answer_s> pets_answer(
        const organization_config_s &config,
        const std::string_view normalized_query) {
    const auto &policy = config.general_amenities.pets;
    const auto asks_advance_notice =
            contains_any_word_prefix(normalized_query, {"предупред", "сообщ"}) &&
            contains_any_word_prefix(normalized_query,
                                     {"заранее", "администратор", "животн", "питом", "собак", "кошк"});

    if (asks_advance_notice && !policy.note.empty()) {
        auto customer = sentence(std::format("Да, {}",
                                             lowercase_first(pets_advance_notice(policy))));
        customer = append_sentence(std::move(customer), pets_behavior_rule());
        return make_pets_answer(config,
                                "О посещении с животным нужно заранее предупредить администратора",
                                std::move(customer),
                                "🐾 😊 🌸");
    }

    const auto requested_height = extract_centimeters(normalized_query);
    const auto asks_service_animal = contains_any_word_prefix(normalized_query,
                                                              {
                                                                      "проводник",
                                                                      "поводыр",
                                                                      "служебн",
                                                                  });
    const auto asks_small_dog = contains_any_word_prefix(normalized_query,
                                                         {
                                                                 "маленьк",
                                                                 "небольш",
                                                                 "миниатюр",
                                                             }) &&
                                contains_any_word_prefix(normalized_query,
                                                         {
                                                                 "собак",
                                                                 "собач",
                                                                 "пес",
                                                             });
    const auto asks_large_dog = contains_any_word_prefix(normalized_query,
                                                         {
                                                                 "крупн",
                                                                 "больш",
                                                             }) &&
                                contains_any_word_prefix(normalized_query,
                                                         {
                                                                 "собак",
                                                                 "собач",
                                                                 "пес",
                                                             });

    if (asks_service_animal) {
        const auto allowed = policy.allowed || policy.service_animals_allowed;
        auto customer = allowed
                                ? std::string{"Да, посещение с собакой-проводником разрешено"}
                                : std::string{
                                          "К сожалению, посещение с собакой-проводником не предусмотрено"};

        if (allowed) {
            customer = append_sentence(std::move(customer),
                                       pets_service_animal_behavior_rule());

            if (const auto notice = pets_advance_notice(policy); !notice.empty()) {
                customer += "\n\n";
                customer += sentence(notice);
            }
        }

        return make_pets_answer(config,
                                std::format("Собака-проводник: {}",
                                            allowed ? "разрешена" : "не разрешена"),
                                std::move(customer),
                                allowed ? "🐶 👩‍🦯 😊 🌸" : "😞");
    }

    if (requested_height.has_value() && policy.max_dog_height_cm.has_value()) {
        const auto allowed = policy.allowed ||
                             (policy.small_dogs_allowed &&
                              *requested_height <= *policy.max_dog_height_cm);
        auto customer = allowed
                                ? (policy.allowed
                                           ? std::format("Да, можно прийти с собакой высотой {} см",
                                                         *requested_height)
                                           : std::format(
                                                     "Да, можно прийти с собакой высотой до {} см",
                                                     *policy.max_dog_height_cm))
                                : std::format(
                                          "К сожалению, к нам можно только с небольшой собакой высотой до {} см",
                                          *policy.max_dog_height_cm);
        if (!allowed) {
            return make_pets_answer(
                    config,
                    std::format("Собака высотой {} см: не разрешена",
                                *requested_height),
                    full_pets_policy_text(config, policy),
                    {});
        }

        customer = sentence(std::move(customer));
        customer += "\n\n";
        customer += pets_allowed_rules(policy);

        return make_pets_answer(config,
                                std::format("Собака высотой {} см: разрешена",
                                            *requested_height),
                                std::move(customer),
                                "🥰 🐾 😊 🌸");
    }

    if (asks_small_dog) {
        const auto allowed = policy.allowed || policy.small_dogs_allowed;
        auto customer = std::string{};

        if (allowed) {
            customer = policy.max_dog_height_cm.has_value() && !policy.allowed
                               ? std::format(
                                         "Да, можно прийти с небольшой собакой высотой до {} см",
                                         *policy.max_dog_height_cm)
                               : std::string{"Да, можно прийти с небольшой собакой"};
            customer = sentence(std::move(customer));
            customer += "\n\n";
            customer += pets_allowed_rules(policy);
        } else {
            return make_pets_answer(config,
                                    "Небольшая собака: не разрешена",
                                    full_pets_policy_text(config, policy),
                                    {});
        }

        return make_pets_answer(config,
                                "Небольшая собака: разрешена",
                                std::move(customer),
                                "🥰 🐾 😊 🌸");
    }

    if (asks_large_dog) {
        const auto allowed = policy.allowed;
        if (!allowed) {
            return make_pets_answer(config,
                                    "Крупная собака: не разрешена",
                                    full_pets_policy_text(config, policy),
                                    {});
        }

        auto customer = sentence("Да, к нам можно с крупной собакой");
        customer += "\n\n";
        customer += pets_allowed_rules(policy);

        return make_pets_answer(config,
                                "Крупная собака: разрешена",
                                std::move(customer),
                                "🥰 🐾 😊 🌸");
    }

    auto fact = std::string{};

    if (policy.allowed) {
        fact = "Посещение с животными разрешено при соблюдении правил поведения";
    } else if (policy.small_dogs_allowed) {
        fact = policy.max_dog_height_cm.has_value()
                       ? std::format("Разрешены небольшие собаки высотой до {} см",
                                     *policy.max_dog_height_cm)
                       : std::string{"Разрешены небольшие собаки"};

        if (policy.service_animals_allowed) {
            fact = append_sentence(std::move(fact), "Собаки-проводники разрешены");
        }
    } else {
        fact = "Посещение с домашними животными запрещено";

        if (policy.service_animals_allowed) {
            fact = append_sentence(std::move(fact), "Собаки-проводники разрешены");
        }
    }

    return make_pets_answer(config,
                            std::move(fact),
                            full_pets_policy_text(config, policy),
                            {});
}

[[nodiscard]] const std::vector<organization_service_s> *active_services(
        const organization_config_s &config) noexcept {
    switch (config.business_type) {
        case organization_business_type_e::beauty_salon:
            return &config.business_details.beauty_salon.services;
        case organization_business_type_e::unknown:
        case organization_business_type_e::coffee_shop:
        case organization_business_type_e::gas_station:
        case organization_business_type_e::gym:
            return nullptr;
    }

    return nullptr;
}

[[nodiscard]] const std::vector<std::string> *active_product_categories(
        const organization_config_s &config) noexcept {
    switch (config.business_type) {
        case organization_business_type_e::beauty_salon:
            return &config.business_details.beauty_salon.product_categories;
        case organization_business_type_e::unknown:
        case organization_business_type_e::coffee_shop:
        case organization_business_type_e::gas_station:
        case organization_business_type_e::gym:
            return nullptr;
    }

    return nullptr;
}

struct beauty_direction_view_s {
    std::string_view id = {};
    std::string_view label = {};
    std::string_view related_label = {};
    std::string_view unavailable_label = {};
    bool enabled = false;
};

[[nodiscard]] std::array<beauty_direction_view_s, 10> beauty_direction_views(
        const organization_config_s &config) noexcept {
    const auto &directions = config.business_details.beauty_salon.service_directions;

    return {{
            {"manicure", "маникюр", "маникюром", "услуги маникюра", directions.manicure},
            {"pedicure", "педикюр", "педикюром", "услуги педикюра", directions.pedicure},
            {"brows", "брови", "оформлением бровей", "услуги по оформлению бровей", directions.brows},
            {"eyelashes", "ресницы", "ресницами", "услуги для ресниц", directions.eyelashes},
            {"hairdressing", "парикмахерские услуги", "волосами", "парикмахерские услуги", directions.hairdressing},
            {"cosmetology", "косметология", "косметологией", "косметологические услуги", directions.cosmetology},
            {"hair_removal", "эпиляция и депиляция", "эпиляцией и депиляцией", "услуги эпиляции и депиляции", directions.hair_removal},
            {"makeup", "макияж", "макияжем", "услуги макияжа", directions.makeup},
            {"massage", "массаж", "массажем", "услуги массажа", directions.massage},
            {"podology", "подология", "подологией", "услуги подологии", directions.podology},
    }};
}

[[nodiscard]] bool service_is_available(const organization_config_s &config,
                                        const organization_service_s &service) noexcept {
    if (config.business_type != organization_business_type_e::beauty_salon ||
        service.directions.empty()) {
        return true;
    }

    const auto &directions = config.business_details.beauty_salon.service_directions;
    auto has_primary_direction = false;

    for (const auto &direction : service.directions) {
        if (beauty_direction_is_child(direction)) {
            continue;
        }

        has_primary_direction = true;

        if (beauty_direction_enabled(directions, direction)) {
            return true;
        }
    }

    if (has_primary_direction) {
        return false;
    }

    return std::ranges::any_of(service.directions, [&](const std::string &direction) {
        return beauty_direction_enabled(directions, direction);
    });
}

[[nodiscard]] bool service_has_direction(const organization_service_s &service,
                                         const std::string_view direction) noexcept {
    return std::ranges::find(service.directions, direction) != service.directions.end();
}

[[nodiscard]] std::string service_list_label(
        const organization_service_s &service) {
    auto label = service.name;

    if (service.minor_access.has_value() && service.minor_access->allowed && service.minor_access->min_age.has_value()) {
        label += std::format(" (от {} лет)", *service.minor_access->min_age);
    }

    return label;
}

[[nodiscard]] std::string_view primary_beauty_direction_id(
        const std::string_view direction_id) noexcept {
    constexpr auto child_prefix = std::string_view{"child_"};
    return direction_id.starts_with(child_prefix)
                   ? direction_id.substr(child_prefix.size())
                   : direction_id;
}

[[nodiscard]] bool service_matches_direction_family(
        const organization_service_s &service,
        const std::string_view direction_id) noexcept {
    const auto requested_primary = primary_beauty_direction_id(direction_id);

    return std::ranges::any_of(service.directions, [&](const auto &service_direction) {
        return primary_beauty_direction_id(service_direction) == requested_primary;
    });
}

[[nodiscard]] std::vector<std::string> available_service_labels(
        const organization_config_s &config,
        const organization_service_s *excluded_service = nullptr) {
    const auto *services = active_services(config);
    auto labels = std::vector<std::string>{};

    if (services == nullptr) {
        return labels;
    }

    for (const auto &service : *services) {
        if (&service == excluded_service || !service_is_available(config, service)) {
            continue;
        }

        const auto label = service_list_label(service);
        if (std::ranges::find(labels, label) == labels.end()) {
            labels.push_back(label);
        }
    }

    return labels;
}

[[nodiscard]] std::vector<std::string> available_service_labels_for_directions(
        const organization_config_s &config,
        const std::vector<std::string_view> &direction_ids,
        const organization_service_s *excluded_service = nullptr) {
    const auto *services = active_services(config);
    auto labels = std::vector<std::string>{};

    if (services == nullptr || direction_ids.empty()) {
        return labels;
    }

    for (const auto &service : *services) {
        if (&service == excluded_service || !service_is_available(config, service)) {
            continue;
        }

        const auto matches_direction = std::ranges::any_of(
                direction_ids,
                [&](const auto direction_id) {
                    return service_matches_direction_family(service, direction_id);
                });

        if (!matches_direction) {
            continue;
        }

        const auto label = service_list_label(service);
        if (std::ranges::find(labels, label) == labels.end()) {
            labels.push_back(label);
        }
    }

    return labels;
}

[[nodiscard]] std::string append_service_alternatives(
        std::string text,
        const std::vector<std::string> &alternatives,
        const std::string_view heading) {
    text = sentence(std::move(text));

    if (alternatives.empty()) {
        return text;
    }

    text += "\n\n";
    text += text_section(heading, em_dash_list(alternatives));
    return text;
}

[[nodiscard]] std::vector<std::string> customer_service_alternative_labels(
        const std::vector<std::string> &alternatives) {
    auto labels = std::vector<std::string>{};
    labels.reserve(alternatives.size());

    for (const auto &alternative : alternatives) {
        labels.push_back(with_friendly_emoji(alternative, emoji_kind_e::services));
    }

    return labels;
}

[[nodiscard]] std::string customer_service_unavailable_text(
        std::string unavailable_text,
        const std::vector<std::string> &alternatives) {
    if (alternatives.empty()) {
        return sentence(std::move(unavailable_text));
    }

    return std::format(
            "{}, но мы можем предложить другие наши услуги:\n{}",
            unavailable_text,
            em_dash_list(customer_service_alternative_labels(alternatives)));
}

[[nodiscard]] std::string customer_service_collection(
        const std::string_view heading,
        const std::vector<std::string> &services) {
    if (services.empty()) {
        return {};
    }

    if (services.size() == 1) {
        return sentence(std::format("{} {}", heading, services.front()));
    }

    return std::format("{}:\n{}", heading, em_dash_list(services));
}

[[nodiscard]] std::optional<organization_config_answer_s> beauty_directions_answer(
        const organization_config_s &config) {
    if (config.business_type != organization_business_type_e::beauty_salon) {
        return std::nullopt;
    }

    auto labels = std::vector<std::string>{};

    for (const auto &direction : beauty_direction_views(config)) {
        if (direction.enabled) {
            labels.emplace_back(direction.label);
        }
    }

    if (labels.empty()) {
        return std::nullopt;
    }

    const auto joined = join_human_readable(labels);
    return make_answer(config,
                       "service_directions",
                       std::format("Доступные направления услуг: {}", joined),
                       std::format("В салоне представлены следующие направления: {}", joined));
}

[[nodiscard]] std::optional<organization_config_answer_s> direction_services_answer(
        const organization_config_s &config,
        const std::string_view direction_id) {
    if (config.business_type != organization_business_type_e::beauty_salon) {
        return std::nullopt;
    }

    const auto directions = beauty_direction_views(config);
    const auto direction_it = std::ranges::find_if(directions, [&](const auto &direction) {
        return direction.id == direction_id;
    });

    if (direction_it == directions.end()) {
        return std::nullopt;
    }

    if (!direction_it->enabled) {
        const auto alternatives = available_service_labels(config);
        auto fact = append_service_alternatives(
                std::format("Направление '{}' отключено в конфигурации",
                            direction_it->label),
                alternatives,
                "Доступные альтернативные услуги");
        auto customer = customer_service_unavailable_text(
                std::format("{} в нашем салоне пока не представлены",
                            capitalize_first(std::string{direction_it->unavailable_label})),
                alternatives);

        return make_answer(config,
                           "service_direction",
                           std::move(fact),
                           std::move(customer));
    }

    auto names = std::vector<std::string>{};

    for (const auto &service : config.business_details.beauty_salon.services) {
        if (service_is_available(config, service) &&
            service_matches_direction_family(service, direction_id)) {
            const auto label = service_list_label(service);
            if (std::ranges::find(names, label) == names.end()) {
                names.push_back(label);
            }
        }
    }

    if (names.empty()) {
        return make_answer(config,
                           "service_direction",
                           std::format("Направление '{}' включено, но связанные услуги не указаны",
                                       direction_it->label),
                           std::format("Мы работаем с {}, но конкретный перечень услуг пока не указан",
                                       direction_it->related_label));
    }

    const auto heading = std::format(
            "Среди услуг, связанных с {}, можем предложить",
            direction_it->related_label);
    const auto customer = customer_service_collection(heading, names);

    return make_answer(config,
                       "service_direction",
                       std::format("Услуги направления '{}':\n{}",
                                   direction_it->label,
                                   em_dash_list(names)),
                       customer);
}

[[nodiscard]] std::optional<organization_config_answer_s> directions_services_answer(
        const organization_config_s &config,
        const std::vector<std::string_view> &direction_ids) {
    auto fact_parts = std::vector<std::string>{};
    auto customer_parts = std::vector<std::string>{};

    for (const auto direction_id : direction_ids) {
        const auto answer = direction_services_answer(config, direction_id);

        if (!answer.has_value()) {
            continue;
        }

        fact_parts.push_back(answer->fact_text);
        customer_parts.push_back(answer->customer_text);
    }

    if (fact_parts.empty()) {
        return std::nullopt;
    }

    return make_answer(config,
                       direction_ids.size() == 1 ? "service_direction"
                                                 : "service_directions",
                       join_strings(fact_parts, "\n\n"),
                       join_strings(customer_parts, "\n\n"));
}

[[nodiscard]] std::optional<organization_config_answer_s> services_answer(
        const organization_config_s &config) {
    const auto *services = active_services(config);
    auto matches = std::vector<service_match_s>{};

    if (services == nullptr || services->empty()) {
        return std::nullopt;
    }

    auto display_names = std::vector<std::string>{};
    display_names.reserve(services->size());

    for (const auto &service : *services) {
        if (service_is_available(config, service)) {
            display_names.push_back(service_list_label(service));
        }
    }

    if (display_names.empty()) {
        return std::nullopt;
    }

    if (display_names.size() >= 3) {
        const auto body = em_dash_list(display_names);
        return make_answer(config,
                           "services",
                           text_section("Доступные услуги", body),
                           text_section("Мы можем предложить следующие услуги", body));
    }

    const auto joined = join_human_readable(display_names);
    return make_answer(config,
                       "services",
                       std::format("В организации доступны услуги: {}", joined),
                       std::format("Мы можем предложить следующие услуги: {}", joined));
}

[[nodiscard]] std::optional<organization_config_answer_s> products_answer(
        const organization_config_s &config) {
    const auto *products = active_product_categories(config);

    if (products == nullptr || products->empty()) {
        return std::nullopt;
    }

    const auto joined = join_human_readable(*products);

    return make_answer(config,
                       "products",
                       std::format("Категории товаров организации: {}", joined),
                       std::format("У нас представлены товары следующих категорий: {}", joined));
}

[[nodiscard]] std::vector<std::string_view> split_words(
        const std::string_view text) {
    auto result = std::vector<std::string_view>{};
    auto offset = std::size_t{0};

    while (offset < text.size()) {
        const auto end = text.find(' ', offset);
        const auto word = text.substr(offset,
                                      end == std::string_view::npos
                                              ? text.size() - offset
                                              : end - offset);

        if (!word.empty()) {
            result.push_back(word);
        }

        if (end == std::string_view::npos) {
            break;
        }

        offset = end + 1;
    }

    return result;
}

[[nodiscard]] bool is_match_stop_word(const std::string_view word) noexcept {
    return contains_any_word(word,
                             {
                                     "и",
                                     "с",
                                     "со",
                                     "без",
                                     "для",
                                     "по",
                                     "на",
                                     "в",
                                     "во",
                                     "у",
                                     "к",
                                     "из",
                                     "или",
                                 });
}

[[nodiscard]] std::size_t common_prefix_codepoints(const std::string_view lhs,
                                                    const std::string_view rhs) noexcept {
    auto lhs_offset = std::size_t{0};
    auto rhs_offset = std::size_t{0};
    auto result = std::size_t{0};

    while (lhs_offset < lhs.size() && rhs_offset < rhs.size()) {
        if (decode_utf8_codepoint(lhs, lhs_offset) !=
            decode_utf8_codepoint(rhs, rhs_offset)) {
            break;
        }

        ++result;
    }

    return result;
}

[[nodiscard]] bool words_semantically_match(const std::string_view lhs,
                                             const std::string_view rhs) noexcept {
    if (lhs == rhs) {
        return true;
    }

    if ((lhs.starts_with(rhs) || rhs.starts_with(lhs)) &&
        std::min(lhs.size(), rhs.size()) >= 6) {
        return true;
    }

    if (common_prefix_codepoints(lhs, rhs) >= 4) {
        return true;
    }

    const auto painting_synonyms =
            (lhs.starts_with("окраш") && rhs.starts_with("покрас")) ||
            (lhs.starts_with("покрас") && rhs.starts_with("окраш"));
    const auto child_synonyms =
            ((lhs.starts_with("дет") || lhs.starts_with("ребен") ||
              lhs.starts_with("подрост") || lhs.starts_with("несовершеннолет")) &&
             (rhs.starts_with("дет") || rhs.starts_with("ребен") ||
              rhs.starts_with("подрост") || rhs.starts_with("несовершеннолет")));

    return painting_synonyms || child_synonyms;
}

[[nodiscard]] bool contains_exact_word_sequence(
        const std::string_view text,
        const std::string_view phrase) {
    const auto text_words = split_words(text);
    const auto phrase_words = split_words(phrase);

    if (phrase_words.empty() || phrase_words.size() > text_words.size()) {
        return false;
    }

    for (auto offset = std::size_t{0};
         offset + phrase_words.size() <= text_words.size();
         ++offset) {
        auto matches = true;

        for (auto index = std::size_t{0}; index < phrase_words.size(); ++index) {
            if (phrase_words[index] != text_words[offset + index]) {
                matches = false;
                break;
            }
        }

        if (matches) {
            return true;
        }
    }

    return false;
}

[[nodiscard]] std::size_t fuzzy_candidate_match_score(
        const std::string_view candidate,
        const std::string_view normalized_query) {
    const auto normalized_candidate = normalize_query(candidate);

    if (normalized_candidate.empty()) {
        return 0;
    }

    if (contains_exact_word_sequence(normalized_query, normalized_candidate)) {
        return 100'000 + normalized_candidate.size();
    }

    const auto candidate_words = split_words(normalized_candidate);
    const auto query_words = split_words(normalized_query);
    auto significant_words = std::size_t{0};
    auto matched_words = std::size_t{0};

    for (const auto candidate_word : candidate_words) {
        if (is_match_stop_word(candidate_word)) {
            continue;
        }

        ++significant_words;

        if (std::ranges::any_of(query_words, [candidate_word](const auto query_word) {
                return words_semantically_match(candidate_word, query_word);
            })) {
            ++matched_words;
        }
    }

    if (significant_words == 0 || matched_words == 0) {
        return 0;
    }

    const auto minimum_matches = significant_words == 1 ? std::size_t{1}
                                                         : std::size_t{2};

    if (matched_words < minimum_matches) {
        return 0;
    }

    return matched_words * 1'000 +
           (matched_words == significant_words ? 500 : 0) +
           normalized_candidate.size();
}

[[nodiscard]] std::size_t service_match_score(const organization_service_s &service,
                                               const std::string_view normalized_query) {
    auto best_score = fuzzy_candidate_match_score(service.name, normalized_query);

    for (const auto &alias : service.aliases) {
        best_score = std::max(best_score,
                              fuzzy_candidate_match_score(alias, normalized_query));
    }

    return best_score;
}

[[nodiscard]] std::string best_exact_service_candidate(
        const organization_service_s &service,
        const std::string_view normalized_query) {
    auto result = std::string{};
    const auto inspect = [&](const std::string_view candidate) {
        const auto normalized = normalize_query(candidate);

        if (normalized.size() > result.size() &&
            contains_exact_word_sequence(normalized_query, normalized)) {
            result = normalized;
        }
    };

    inspect(service.name);
    for (const auto &alias : service.aliases) {
        inspect(alias);
    }

    return result;
}

[[nodiscard]] std::size_t service_mention_position(
        const organization_service_s &service,
        const std::string_view normalized_query) {
    auto best = std::string_view::npos;
    const auto inspect = [&](const std::string_view candidate) {
        const auto normalized_candidate = normalize_query(candidate);

        if (normalized_candidate.empty()) {
            return;
        }

        if (const auto exact = normalized_query.find(normalized_candidate);
            exact != std::string_view::npos) {
            best = std::min(best, exact);
            return;
        }

        const auto candidate_words = split_words(normalized_candidate);
        const auto query_words = split_words(normalized_query);

        for (const auto query_word : query_words) {
            const auto semantically_matches = std::ranges::any_of(
                    candidate_words,
                    [&](const auto candidate_word) {
                        return !is_match_stop_word(candidate_word) &&
                               words_semantically_match(candidate_word, query_word);
                    });

            if (!semantically_matches) {
                continue;
            }

            if (const auto position = normalized_query.find(query_word);
                position != std::string_view::npos) {
                best = std::min(best, position);
            }
        }
    };

    inspect(service.name);
    for (const auto &alias : service.aliases) {
        inspect(alias);
    }

    return best;
}

[[nodiscard]] std::vector<service_match_s> find_matching_services(
        const organization_config_s &config,
        const std::string_view normalized_query,
        const bool include_unavailable = false) {
    const auto *services = active_services(config);
    auto matches = std::vector<service_match_s>{};

    if (services == nullptr || services->empty()) {
        return matches;
    }

    for (const auto &service : *services) {
        if (!include_unavailable && !service_is_available(config, service)) {
            continue;
        }

        const auto score = service_match_score(service, normalized_query);

        if (score != 0) {
            matches.push_back(service_match_s{&service, score,
                                              service_mention_position(service, normalized_query)});
        }
    }

    std::ranges::sort(matches,
                      [](const auto &lhs, const auto &rhs) {
                          return lhs.score > rhs.score;
                      });

    if (matches.empty()) {
        return matches;
    }

    const auto has_high_confidence = std::ranges::any_of(matches, [](const auto &match) {
        return match.score >= 2'000;
    });

    if (has_high_confidence) {
        std::erase_if(matches, [](const auto &match) {
            return match.score < 2'000;
        });
    } else {
        matches.resize(1);
    }

    auto exact_candidates = std::vector<std::pair<const organization_service_s *,
                                                  std::string>>{};
    exact_candidates.reserve(matches.size());

    for (const auto &match : matches) {
        exact_candidates.emplace_back(
                match.service,
                best_exact_service_candidate(*match.service, normalized_query));
    }

    std::erase_if(matches, [&](const auto &candidate_match) {
        const auto candidate_it = std::ranges::find(
                exact_candidates,
                candidate_match.service,
                &std::pair<const organization_service_s *, std::string>::first);
        const auto &candidate = candidate_it->second;

        if (candidate.empty()) {
            return false;
        }

        return std::ranges::any_of(exact_candidates, [&](const auto &other_match) {
            if (other_match.first == candidate_match.service) {
                return false;
            }

            const auto &other = other_match.second;
            return other.size() > candidate.size() &&
                   contains_exact_word_sequence(other, candidate);
        });
    });

    const auto has_exact_candidate = std::ranges::any_of(
            exact_candidates,
            [](const auto &candidate) {
                return !candidate.second.empty();
            });

    if (has_exact_candidate) {
        auto exact_words = std::vector<std::string_view>{};

        for (const auto &[service, candidate] : exact_candidates) {
            (void) service;

            if (candidate.empty()) {
                continue;
            }

            const auto words = split_words(candidate);
            exact_words.insert(exact_words.end(), words.begin(), words.end());
        }

        const auto query_words = split_words(normalized_query);
        std::erase_if(matches, [&](const auto &match) {
            const auto candidate_it = std::ranges::find_if(
                    exact_candidates,
                    [&](const auto &candidate) {
                        return candidate.first == match.service;
                    });

            if (candidate_it != exact_candidates.end() &&
                !candidate_it->second.empty()) {
                return false;
            }

            const auto has_unique_query_match = [&](const std::string_view candidate) {
                for (const auto candidate_word : split_words(normalize_query(candidate))) {
                    if (is_match_stop_word(candidate_word)) {
                        continue;
                    }

                    for (const auto query_word : query_words) {
                        if (!words_semantically_match(candidate_word, query_word)) {
                            continue;
                        }

                        const auto covered_by_exact = std::ranges::any_of(
                                exact_words,
                                [&](const auto exact_word) {
                                    return words_semantically_match(exact_word,
                                                                    query_word);
                                });

                        if (!covered_by_exact) {
                            return true;
                        }
                    }
                }

                return false;
            };

            return !has_unique_query_match(match.service->name) &&
                   std::ranges::none_of(match.service->aliases,
                                        has_unique_query_match);
        });
    }

    std::ranges::stable_sort(matches,
                             [](const auto &lhs, const auto &rhs) {
                                 if (lhs.mention_position != rhs.mention_position) {
                                     return lhs.mention_position < rhs.mention_position;
                                 }

                                 return lhs.score > rhs.score;
                             });

    return matches;
}

[[nodiscard]] const organization_service_s *find_matching_service(
        const organization_config_s &config,
        const std::string_view normalized_query,
        const bool include_unavailable = false) noexcept {
    const auto matches = find_matching_services(config,
                                                 normalized_query,
                                                 include_unavailable);
    return matches.empty() ? nullptr : matches.front().service;
}

[[nodiscard]] const organization_service_s *find_matching_minor_service(
        const organization_config_s &config,
        const std::string_view normalized_query) noexcept {
    const auto *services = active_services(config);

    if (services == nullptr) {
        return nullptr;
    }

    const organization_service_s *result = nullptr;
    auto best_score = std::size_t{0};

    for (const auto &service : *services) {
        if (!service_is_available(config, service) || (!service.minor_access.has_value() || !service.minor_access->allowed)) {
            continue;
        }

        const auto score = service_match_score(service, normalized_query);
        if (score > best_score) {
            best_score = score;
            result = &service;
        }
    }

    return result;
}

[[nodiscard]] const organization_service_s *find_exact_matching_minor_service(
        const organization_config_s &config,
        const std::string_view normalized_query) {
    const auto *services = active_services(config);

    if (services == nullptr) {
        return nullptr;
    }

    const organization_service_s *result = nullptr;
    auto best_candidate_size = std::size_t{0};

    for (const auto &service : *services) {
        if (!service_is_available(config, service) ||
            !service.minor_access.has_value() || !service.minor_access->allowed) {
            continue;
        }

        const auto candidate = best_exact_service_candidate(service,
                                                              normalized_query);

        if (candidate.size() > best_candidate_size) {
            best_candidate_size = candidate.size();
            result = &service;
        }
    }

    return result;
}

[[nodiscard]] bool has_specific_exact_service_match(
        const organization_service_s &service,
        const std::string_view normalized_query) {
    const auto candidate_is_specific = [&](const std::string_view candidate) {
        const auto normalized = normalize_query(candidate);
        return split_words(normalized).size() >= 2 &&
               fuzzy_candidate_match_score(normalized, normalized_query) >= 2'000;
    };

    return candidate_is_specific(service.name) ||
           std::ranges::any_of(service.aliases, candidate_is_specific);
}

[[nodiscard]] bool manicure_without_coating_intent(
        const std::string_view normalized_query) noexcept {
    if (!contains_word_prefix(normalized_query, "маник")) {
        return false;
    }

    return contains_any(normalized_query,
                        {
                                "без покрытия",
                                "без геля",
                                "без гель лака",
                                "без лака",
                                "покрытие не нужно",
                                "покрытие не надо",
                                "сам маникюр",
                                "только маникюр",
                            });
}

[[nodiscard]] const organization_service_s *find_manicure_without_coating_service(
        const organization_config_s &config) noexcept {
    const auto *services = active_services(config);

    if (services == nullptr) {
        return nullptr;
    }

    const auto candidate_matches = [](const std::string_view candidate) {
        const auto normalized = normalize_query(candidate);
        return contains_any(normalized,
                            {
                                    "без покрытия",
                                    "без гель лака",
                                    "без лака",
                                    "классический маникюр",
                                    "гигиенический маникюр",
                                });
    };

    for (const auto &service : *services) {
        if (!service_is_available(config, service) ||
            !service_has_direction(service, "manicure")) {
            continue;
        }

        if (candidate_matches(service.name) ||
            std::ranges::any_of(service.aliases, candidate_matches)) {
            return &service;
        }
    }

    return nullptr;
}

[[nodiscard]] std::optional<organization_config_answer_s>
generic_unavailable_service_direction_answer(
        const organization_config_s &config,
        const organization_service_s &service,
        const std::string_view normalized_query) {
    const auto exact_candidate = best_exact_service_candidate(service,
                                                               normalized_query);

    if (exact_candidate.empty() || split_words(exact_candidate).size() != 1 ||
        exact_candidate == normalize_query(service.name)) {
        return std::nullopt;
    }

    auto disabled_directions = std::vector<std::string_view>{};

    for (const auto &direction_id : service.directions) {
        const auto views = beauty_direction_views(config);
        const auto direction = std::ranges::find(views,
                                                 direction_id,
                                                 &beauty_direction_view_s::id);

        if (direction != views.end() && !direction->enabled) {
            disabled_directions.push_back(direction->id);
        }
    }

    return disabled_directions.empty()
                   ? std::nullopt
                   : directions_services_answer(config, disabled_directions);
}

[[nodiscard]] std::optional<organization_config_answer_s> specific_service_answer(
        const organization_config_s &config,
        const std::string_view normalized_query,
        const bool allow_missing_answer) {
    const auto *services = active_services(config);
    auto matches = std::vector<service_match_s>{};

    if (services == nullptr || services->empty()) {
        return std::nullopt;
    }

    if (manicure_without_coating_intent(normalized_query)) {
        if (const auto *service = find_manicure_without_coating_service(config);
            service != nullptr) {
            const auto quoted_name = std::format("«{}»", service->name);
            return make_answer(config,
                               "service_availability",
                               std::format("Услуга {} есть в перечне доступных услуг организации",
                                           quoted_name),
                               std::format("Да, у нас можно сделать {}", quoted_name));
        }
    }

    if (const auto *service = find_matching_service(config, normalized_query);
        service != nullptr) {
        const auto quoted_name = std::format("«{}»", service->name);

        return make_answer(config,
                           "service_availability",
                           std::format("Услуга {} есть в перечне доступных услуг организации",
                                       quoted_name),
                           std::format("Да, у нас можно сделать {}", quoted_name));
    }

    if (const auto *service = find_matching_service(config, normalized_query, true);
        service != nullptr && !service_is_available(config, *service)) {
        if (auto direction_answer = generic_unavailable_service_direction_answer(
                    config,
                    *service,
                    normalized_query);
            direction_answer.has_value()) {
            return direction_answer;
        }

        auto direction_ids = std::vector<std::string_view>{};
        for (const auto &direction_id : service->directions) {
            const auto primary = primary_beauty_direction_id(direction_id);
            if (std::ranges::find(direction_ids, primary) == direction_ids.end()) {
                direction_ids.push_back(primary);
            }
        }

        auto alternatives = available_service_labels_for_directions(
                config,
                direction_ids,
                service);
        if (alternatives.empty()) {
            alternatives = available_service_labels(config, service);
        }

        const auto quoted_name = std::format("«{}»", service->name);
        auto fact = append_service_alternatives(
                std::format("Услуга {} связана только с отключённым направлением",
                            quoted_name),
                alternatives,
                "Доступные альтернативные услуги");
        auto customer = customer_service_unavailable_text(
                std::format("Услугу {} мы пока не оказываем", quoted_name),
                alternatives);

        return make_answer(config,
                           "service_availability",
                           std::move(fact),
                           std::move(customer));
    }

    if (!allow_missing_answer) {
        return std::nullopt;
    }

    const auto direction_ids = beauty_directions_from_query(normalized_query);
    auto alternatives = available_service_labels_for_directions(config,
                                                                  direction_ids);
    if (alternatives.empty()) {
        alternatives = available_service_labels(config);
    }

    auto fact = append_service_alternatives(
            "Запрошенная услуга отсутствует в настроенном перечне услуг",
            alternatives,
            "Доступные альтернативные услуги");
    auto customer = customer_service_unavailable_text(
            "Такой услуги в нашем перечне пока нет",
            alternatives);

    return make_answer(config,
                       "service_availability",
                       std::move(fact),
                       std::move(customer));
}

[[nodiscard]] std::optional<std::size_t> extract_requested_age(
        const std::string_view normalized_query) noexcept {
    const auto words = split_words(normalized_query);

    for (auto index = std::size_t{0}; index < words.size(); ++index) {
        const auto word = words[index];
        auto value = std::size_t{0};
        const auto [end, error] = std::from_chars(word.data(),
                                                  word.data() + word.size(),
                                                  value);

        if (error != std::errc{} || end != word.data() + word.size()) {
            continue;
        }

        const auto has_age_suffix = index + 1 < words.size() &&
                                    (words[index + 1].starts_with("лет") ||
                                     words[index + 1].starts_with("год"));

        if (has_age_suffix && value <= 120) {
            return value;
        }
    }

    return std::nullopt;
}

[[nodiscard]] bool parent_presence_intent(
        const std::string_view normalized_query) noexcept {
    return contains_any_word_prefix(normalized_query,
                                    {
                                            "родител",
                                            "мам",
                                            "пап",
                                            "сопровожд",
                                            "представител",
                                        }) &&
           contains_any_word_prefix(normalized_query,
                                    {
                                            "присутств",
                                            "наход",
                                            "быть",
                                            "обязател",
                                            "нуж",
                                        });
}

[[nodiscard]] std::string parent_presence_text(
        const organization_general_amenities_s &amenities) {
    return amenities.minor_parent_presence_required
                   ? "Во время процедуры требуется присутствие родителя или законного представителя"
                   : "Постоянное присутствие родителя или законного представителя во время процедуры не требуется";
}

[[nodiscard]] std::optional<organization_config_answer_s> minors_answer(
        const organization_config_s &config,
        const std::string_view normalized_query) {
    const auto &amenities = config.general_amenities;
    const auto requested_age = extract_requested_age(normalized_query);
    const auto asks_parent_presence = parent_presence_intent(normalized_query);
    const auto asks_minimum_age = contains_any(normalized_query,
                                               {
                                                       "с какого возраста",
                                                       "минимальный возраст",
                                                   });
    const auto *exact_minor_service = find_exact_matching_minor_service(
            config,
            normalized_query);
    const auto *service = exact_minor_service != nullptr
                                  ? exact_minor_service
                                  : find_matching_service(config, normalized_query);
    const auto *minor_service = find_matching_minor_service(config, normalized_query);

    if (exact_minor_service == nullptr && minor_service != nullptr &&
        (service == nullptr || !has_specific_exact_service_match(*service, normalized_query))) {
        service = minor_service;
    }

    if (service == nullptr) {
        if (!amenities.serves_minors) {
            return make_answer(config,
                               "minors",
                               "Обслуживание несовершеннолетних не предусмотрено",
                               "Услуги для несовершеннолетних не предусмотрены");
        }

        if (contains_any(normalized_query,
                         {
                                 "какие услуги",
                                 "какие детские услуги",
                                 "список услуг",
                                 "перечень услуг",
                                 "что доступно",
                             }) ||
            (contains_word_prefix(normalized_query, "детск") &&
             contains_word_prefix(normalized_query, "услуг"))) {
            auto names = std::vector<std::string>{};

            if (const auto *services = active_services(config); services != nullptr) {
                for (const auto &candidate : *services) {
                    if (service_is_available(config, candidate) &&
                        candidate.minor_access.has_value() && candidate.minor_access->allowed) {
                        names.push_back(service_list_label(candidate));
                    }
                }
            }

            if (!names.empty()) {
                const auto list = em_dash_list(names);
                return make_answer(config,
                                   "minors",
                                   std::format("Для несовершеннолетних доступны услуги:\n{}",
                                               list),
                                   std::format("Для несовершеннолетних доступны следующие услуги:\n{}",
                                               list));
            }
        }

        if (asks_parent_presence) {
            const auto text = parent_presence_text(amenities);
            return make_answer(config, "minors", text, text);
        }

        auto customer = std::string{
                "Да, некоторые услуги доступны несовершеннолетним. Возможность записи и минимальный возраст зависят от выбранной услуги"};
        customer = append_sentence(std::move(customer), parent_presence_text(amenities));
        customer = append_sentence(std::move(customer),
                                   "уточните, пожалуйста, какая услуга вас интересует");
        return make_answer(config,
                           "minors",
                           "В организации есть отдельные услуги для несовершеннолетних",
                           std::move(customer));
    }

    const auto quoted_name = std::format("«{}»", service->name);
    const auto append_parent_presence = [&](std::string text) {
        return asks_parent_presence
                       ? append_sentence(std::move(text),
                                         parent_presence_text(amenities))
                       : text;
    };

    if (requested_age.has_value() && *requested_age >= 18) {
        return make_answer(config,
                           "minors",
                           std::format("Для услуги {} возрастное ограничение для несовершеннолетних не применяется",
                                       quoted_name),
                           append_parent_presence(std::format(
                                   "Да, в {} лет услуга {} доступна",
                                   *requested_age,
                                   quoted_name)));
    }

    if (!amenities.serves_minors || (!service->minor_access.has_value() || !service->minor_access->allowed)) {
        return make_answer(config,
                           "minors",
                           std::format("Услуга {} доступна только совершеннолетним",
                                       quoted_name),
                           append_parent_presence(std::format(
                                   "К сожалению, услуга {} доступна только с 18 лет",
                                   quoted_name)));
    }

    const auto min_age = service->minor_access->min_age.value_or(18);

    if (requested_age.has_value() && *requested_age < min_age) {
        return make_answer(config,
                           "minors",
                           std::format("Минимальный возраст для услуги {} — {} лет",
                                       quoted_name,
                                       min_age),
                           append_parent_presence(std::format(
                                   "К сожалению, услуга {} доступна с {} лет",
                                   quoted_name,
                                   min_age)));
    }

    const auto available_text = std::format(
            "Услуга {} доступна несовершеннолетним с {} лет",
            quoted_name,
            min_age);
    return make_answer(config,
                       "minors",
                       available_text,
                       append_parent_presence(asks_minimum_age
                                                      ? available_text
                                                      : std::format(
                                                                "Да, услуга {} доступна несовершеннолетним с {} лет",
                                                                quoted_name,
                                                                min_age)));
}

[[nodiscard]] const std::string *find_matching_product(
        const organization_config_s &config,
        const std::string_view normalized_query) noexcept {
    const auto *products = active_product_categories(config);

    if (products == nullptr || products->empty()) {
        return nullptr;
    }

    const std::string *result = nullptr;
    auto best_score = std::size_t{0};

    for (const auto &product : *products) {
        const auto score = fuzzy_candidate_match_score(product, normalized_query);

        if (score > best_score) {
            best_score = score;
            result = &product;
        }
    }

    return result;
}

[[nodiscard]] std::optional<organization_config_answer_s> specific_product_answer(
        const organization_config_s &config,
        const std::string_view normalized_query) {
    const auto *product = find_matching_product(config, normalized_query);

    if (product == nullptr) {
        return std::nullopt;
    }

    const auto quoted_name = std::format("«{}»", *product);

    return make_answer(config,
                       "product_availability",
                       std::format("Категория товара {} указана в конфигурации организации",
                                   quoted_name),
                       std::format("Да, у нас можно приобрести {}", quoted_name));
}

[[nodiscard]] std::string negative_warranty_detail(const std::string_view note) {
    if (note.empty()) {
        return {};
    }

    const auto normalized = normalize_query(note);
    constexpr auto prefix_one = std::string_view{"гарантия не распространяется на "};
    constexpr auto prefix_two = std::string_view{"гарантия на "};
    constexpr auto suffix = std::string_view{" не распространяется"};

    if (normalized.starts_with(prefix_one)) {
        return sentence(std::format("Она не распространяется на {}",
                                    normalized.substr(prefix_one.size())));
    }

    if (normalized.starts_with(prefix_two) && normalized.ends_with(suffix) &&
        normalized.size() > prefix_two.size() + suffix.size()) {
        const auto detail = normalized.substr(prefix_two.size(),
                                              normalized.size() - prefix_two.size() - suffix.size());
        return sentence(std::format("Она не распространяется на {}", detail));
    }

    return sentence(capitalize_first(std::string{note}));
}

[[nodiscard]] std::string customer_friendly_warranty_note(
        const std::string_view note,
        const bool omit_legacy_period = false) {
    if (note.empty()) {
        return {};
    }

    const auto normalized = normalize_query(note);

    if (normalized.contains("внутренним регламентом")) {
        return "гарантия распространяется на подтверждённый скол или отслойку покрытия";
    }

    if (normalized.contains("решение принимается после фотографии") ||
        normalized.contains("проверки обстоятельств повреждения")) {
        return "для рассмотрения ситуации необходимо отправить фотографию повреждения — администратор уточнит детали и предложит дальнейшие действия";
    }

    auto result = lowercase_first(std::string{note});
    constexpr auto legacy_period_suffix =
            std::string_view{", если повреждение появилось в течение гарантийного срока"};

    if (omit_legacy_period) {
        if (const auto position = result.find(legacy_period_suffix);
            position != std::string::npos) {
            result.erase(position, legacy_period_suffix.size());
        }
    }

    return result;
}

[[nodiscard]] std::string warranty_period_phrase(
        const organization_service_warranty_s &warranty) {
    if (!warranty.period_days.has_value()) {
        return {};
    }

    const auto days = *warranty.period_days;
    const auto day_word = days % 10 == 1 && days % 100 != 11 ? "дня" : "дней";
    return std::format("в течение {} {} после оказания услуги", days, day_word);
}

[[nodiscard]] std::string warranty_condition_text(
        const organization_service_warranty_s &warranty,
        const bool customer_friendly) {
    if (warranty.note.empty()) {
        return {};
    }

    return customer_friendly
                   ? customer_friendly_warranty_note(
                             warranty.note,
                             warranty.period_days.has_value())
                   : lowercase_first(warranty.note);
}

[[nodiscard]] std::string append_warranty_note(
        std::string text,
        const organization_service_warranty_s &warranty) {
    text = sentence(std::move(text));

    if (warranty.note.empty()) {
        return text;
    }

    text.push_back(' ');
    text += sentence(std::format("Важно отметить, что {}",
                                 customer_friendly_warranty_note(
                                         warranty.note,
                                         warranty.period_days.has_value())));
    return text;
}

[[nodiscard]] bool service_mentions_coating_or_polish(
        const organization_service_s &service) {
    const auto mentions = [](const std::string_view text) {
        const auto normalized = normalize_query(text);
        return contains_any_word_prefix(normalized, {"покрыт", "гель", "лак"});
    };

    return mentions(service.name) ||
           std::ranges::any_of(service.aliases, [&](const auto &alias) {
               return mentions(alias);
           });
}

[[nodiscard]] std::optional<organization_config_answer_s> warranty_incident_answer(
        const organization_config_s &config,
        const std::string_view normalized_query,
        const bool asks_for_photo) {
    const auto *services = active_services(config);

    if (services == nullptr || services->empty()) {
        return std::nullopt;
    }

    if (asks_for_photo) {
        const organization_service_s *matched_service =
                find_matching_service(config, normalized_query);

        const auto note_mentions_photo = [](const organization_service_s &service) {
            const auto note = normalize_query(service.warranty.note);
            return contains_any_word_prefix(note, {"фото", "сним"});
        };

        if (matched_service == nullptr || !note_mentions_photo(*matched_service)) {
            const auto it = std::ranges::find_if(*services, [&](const auto &service) {
            return service_is_available(config, service) && note_mentions_photo(service);
        });
            matched_service = it == services->end() ? nullptr : &*it;
        }

        if (matched_service == nullptr) {
            return std::nullopt;
        }

        auto fact = std::string{
                "Для проверки гарантийного случая требуется фотография повреждения"};
        auto customer = std::string{
                "Да, пожалуйста, отправьте фотографию повреждённого покрытия"};

        if (const auto period = warranty_period_phrase(matched_service->warranty);
            !period.empty()) {
            fact = append_sentence(
                    std::move(fact),
                    std::format("Обратиться по гарантии можно {}", period));
            customer = append_sentence(
                    std::move(customer),
                    std::format("Обратиться по гарантии можно {}", period));
        }

        fact = append_important_note(std::move(fact),
                                     matched_service->warranty.note);
        customer = append_warranty_note(std::move(customer),
                                        matched_service->warranty);

        return make_answer(config,
                           "warranty_incident",
                           std::move(fact),
                           std::move(customer));
    }

    auto matches = find_matching_services(config, normalized_query);

    if (matches.empty() && contains_any_word_prefix(normalized_query,
                                                    {"покрыт", "гель", "лак"})) {
        for (const auto &service : *services) {
            if (service_is_available(config, service) && service.warranty.provided &&
                service_mentions_coating_or_polish(service)) {
                matches.push_back(service_match_s{&service, 1'500, std::string_view::npos});
            }
        }
    }

    if (matches.empty()) {
        return std::nullopt;
    }

    auto fact_parts = std::vector<std::string>{};
    auto customer_parts = std::vector<std::string>{};
    const auto asks_what_to_do = contains_any(normalized_query,
                                               {
                                                       "что делать",
                                                       "как поступить",
                                                       "что предпринять",
                                                   });

    if (matches.size() > 1) {
        auto fact_results = std::vector<std::string>{};
        auto customer_results = std::vector<std::string>{};
        auto fact_conditions = std::vector<std::string>{};
        auto customer_conditions = std::vector<std::string>{};

        for (const auto &match : matches) {
            const auto &service = *match.service;
            const auto quoted_name = std::format("«{}»", service.name);

            auto fact_result = std::format(
                    "{} — повреждение {} гарантийным случаем",
                    quoted_name,
                    service.warranty.provided ? "может быть" : "не является");
            auto customer_result = fact_result;

            if (service.warranty.provided) {
                if (const auto period = warranty_period_phrase(service.warranty);
                    !period.empty()) {
                    fact_result += std::format(", если повреждение появилось {}", period);
                    customer_result += std::format(", если повреждение появилось {}", period);
                }
            }

            fact_results.push_back(std::move(fact_result));
            customer_results.push_back(std::move(customer_result));

            if (const auto condition = warranty_condition_text(service.warranty, false);
                !condition.empty()) {
                fact_conditions.push_back(std::format(
                        "для {}: {}",
                        quoted_name,
                        condition));
            }

            if (const auto condition = warranty_condition_text(service.warranty, true);
                !condition.empty()) {
                customer_conditions.push_back(std::format(
                        "для {}: {}",
                        quoted_name,
                        condition));
            }
        }

        auto fact = text_section("Результат по подходящим услугам",
                                 em_dash_list(fact_results));
        auto customer = std::string{};

        if (asks_what_to_do) {
            customer = sentence(
                    "Свяжитесь, пожалуйста, с администратором и опишите ситуацию");
            customer += "\n\n";
        }

        customer += text_section("По описанию ситуации",
                                 em_dash_list(customer_results));

        if (!fact_conditions.empty()) {
            fact += "\n\n";
            fact += text_section("Условия рассмотрения",
                                 em_dash_list(fact_conditions));
        }

        if (!customer_conditions.empty()) {
            customer += "\n\n";
            customer += text_section("Условия рассмотрения",
                                     em_dash_list(customer_conditions));
        }

        return make_answer(config,
                           "warranty_incident",
                           std::move(fact),
                           std::move(customer));
    }

    for (const auto &match : matches) {
        const auto &service = *match.service;
        const auto quoted_name = std::format("«{}»", service.name);
        auto fact = service.warranty.provided
                            ? std::format("Повреждение услуги {} может относиться к гарантийному случаю",
                                          quoted_name)
                            : std::format("Повреждение услуги {} не относится к гарантийному случаю",
                                          quoted_name);
        auto customer = service.warranty.provided
                                ? std::format("Скол или отслойка покрытия после услуги {} может быть гарантийным случаем",
                                              quoted_name)
                                : std::format("К сожалению, повреждение после услуги {} не относится к гарантийному случаю",
                                              quoted_name);

        if (service.warranty.provided) {
            if (const auto period = warranty_period_phrase(service.warranty);
                !period.empty()) {
                fact += std::format(", если повреждение появилось {}", period);
                customer += std::format(", если повреждение появилось {}", period);
            }
        }

        if (service.warranty.provided && asks_what_to_do) {
            customer = std::format(
                    "Свяжитесь, пожалуйста, с администратором и опишите ситуацию. {}",
                    sentence(std::move(customer)));
        }

        fact = append_important_note(std::move(fact), service.warranty.note);
        customer = append_warranty_note(std::move(customer),
                                        service.warranty);

        fact_parts.push_back(std::move(fact));
        customer_parts.push_back(std::move(customer));
    }

    return make_answer(config,
                       "warranty_incident",
                       join_strings(fact_parts, "\n\n"),
                       join_strings(customer_parts, "\n\n"));
}

[[nodiscard]] std::optional<organization_config_answer_s> warranty_answer(
        const organization_config_s &config,
        const std::string_view normalized_query) {
    const auto *services = active_services(config);

    if (services == nullptr || services->empty()) {
        return std::nullopt;
    }

    if (const auto matches = find_matching_services(config, normalized_query);
        !matches.empty()) {
        auto fact_parts = std::vector<std::string>{};
        auto customer_parts = std::vector<std::string>{};

        auto positive_warranty_count = std::size_t{0};

        for (const auto &match : matches) {
            const auto &service = *match.service;
            const auto service_name = std::format("«{}»", service.name);
            auto fact = service.warranty.provided
                                ? std::format("На услугу {} действует гарантия",
                                              service_name)
                                : std::format("На услугу {} гарантия не распространяется",
                                              service_name);
            auto customer = std::string{};

            if (service.warranty.provided) {
                customer = std::format("На услугу {} {}действует гарантия",
                                       service_name,
                                       positive_warranty_count == 0 ? "" : "тоже ");
                ++positive_warranty_count;

                if (const auto period = warranty_period_phrase(service.warranty);
                    !period.empty()) {
                    fact += std::format(" {}", period);
                    customer += std::format(" {}", period);
                }

                customer = append_warranty_note(std::move(customer),
                                                service.warranty);
            } else {
                customer = std::format("На услугу {} гарантия не распространяется",
                                       service_name);

                if (const auto detail = negative_warranty_detail(service.warranty.note);
                    !detail.empty()) {
                    customer = append_sentence(std::move(customer), detail);
                }
            }

            fact = append_important_note(std::move(fact),
                                         service.warranty.note);
            fact_parts.push_back(std::move(fact));
            customer_parts.push_back(std::move(customer));
        }

        return make_answer(config,
                           "warranty",
                           join_strings(fact_parts, "\n\n"),
                           join_strings(customer_parts, "\n\n"));
    }

    auto warranty_services = std::vector<std::string>{};

    for (const auto &service : *services) {
        if (!service_is_available(config, service) || !service.warranty.provided) {
            continue;
        }

        auto label = service.name;

        if (service.warranty.period_days.has_value()) {
            const auto days = *service.warranty.period_days;
            const auto day_word = days % 10 == 1 && days % 100 != 11 ? "день" : "дней";
            label += std::format(" (гарантия — {} {})", days, day_word);
        }

        warranty_services.push_back(std::move(label));
    }

    if (warranty_services.empty()) {
        return make_answer(config,
                           "warranty",
                           "В конфигурации организации нет услуг, отмеченных как гарантийные",
                           "В перечне услуг нет позиций, отмеченных как гарантийные");
    }

    const auto joined = join_human_readable(warranty_services);

    return make_answer(config,
                       "warranty",
                       std::format("Как гарантийные отмечены услуги: {}", joined),
                       std::format("К гарантийным относятся следующие услуги: {}", joined));
}

[[nodiscard]] std::optional<organization_config_answer_s> coffee_menu_answer(
        const organization_config_s &config) {
    const auto &coffee = config.business_details.coffee_shop;

    if (coffee.menu_categories.empty()) {
        return std::nullopt;
    }

    const auto joined = join_human_readable(coffee.menu_categories);

    return make_answer(config,
                       "menu",
                       std::format("Основные категории меню: {}", joined),
                       std::format("В меню есть следующие категории: {}", joined));
}

[[nodiscard]] std::optional<organization_config_answer_s> alternative_milk_answer(
        const organization_config_s &config) {
    const auto &types = config.business_details.coffee_shop.alternative_milk_types;

    if (types.empty()) {
        return make_answer(config,
                           "alternative_milk",
                           "Альтернативные виды молока не указаны в конфигурации",
                           "Альтернативные виды молока сейчас не предусмотрены");
    }

    const auto joined = join_human_readable(types);

    return make_answer(config,
                       "alternative_milk",
                       std::format("Доступные альтернативные виды молока: {}", joined),
                       std::format("Можно выбрать альтернативное молоко: {}", joined));
}

[[nodiscard]] std::optional<organization_config_answer_s> fuel_types_answer(
        const organization_config_s &config) {
    const auto &fuels = config.business_details.gas_station.fuel_types;

    if (fuels.empty()) {
        return std::nullopt;
    }

    auto names = std::vector<std::string>{};
    names.reserve(fuels.size());

    for (const auto &fuel : fuels) {
        names.push_back(fuel.name);
    }

    const auto joined = join_human_readable(names);

    return make_answer(config,
                       "fuel",
                       std::format("На АЗС предусмотрены виды топлива: {}", joined),
                       std::format("У нас предусмотрены следующие виды топлива: {}", joined));
}

[[nodiscard]] std::optional<organization_config_answer_s> coaching_answer(
        const organization_config_s &config) {
    const auto &options = config.business_details.gym.coaching_options;

    if (options.empty()) {
        return std::nullopt;
    }

    auto descriptions = std::vector<std::string>{};

    for (const auto &option : options) {
        if (option.specialization.empty()) {
            continue;
        }

        if (option.qualification.empty()) {
            descriptions.push_back(option.specialization);
        } else {
            descriptions.push_back(std::format("{} — {}", option.specialization, option.qualification));
        }
    }

    if (descriptions.empty()) {
        return std::nullopt;
    }

    const auto joined = join_strings(descriptions, "; ");

    return make_answer(config,
                       "coaching",
                       std::format("Направления и квалификация тренеров: {}", joined),
                       std::format("Доступны тренировки по направлениям: {}", joined));
}

void append_unique_answer(std::vector<organization_config_answer_s> &answers,
                          std::unordered_set<std::string> &topics,
                          std::optional<organization_config_answer_s> answer) {
    if (!answer.has_value() || answer->fact_text.empty() || answer->customer_text.empty() ||
        !topics.insert(answer->topic).second) {
        return;
    }

    answers.push_back(std::move(*answer));
}

[[nodiscard]] std::size_t first_query_position(
        const std::string_view query,
        const std::initializer_list<std::string_view> markers) noexcept {
    auto result = std::string_view::npos;

    for (const auto marker : markers) {
        if (const auto position = query.find(marker);
            position != std::string_view::npos) {
            result = std::min(result, position);
        }
    }

    return result;
}

[[nodiscard]] std::size_t answer_topic_position(
        const std::string_view topic,
        const std::string_view query) noexcept {
    if (topic == "booking") {
        return first_query_position(query,
                                    {"запис", "брон", "телег", "тг", "яндекс", "сайт", "телефон"});
    }
    if (topic == "payment") {
        return first_query_position(query,
                                    {"оплат", "сбп", "qr", "карт", "налич", "сертификат", "абонемент", "предоплат"});
    }
    if (topic == "schedule") {
        return first_query_position(query,
                                    {"график", "работ", "откры", "закры", "празд", "январ", "декабр"});
    }
    if (topic == "address") {
        return first_query_position(query,
                                    {"адрес", "добрат", "пройти", "метро", "ориентир", "вход"});
    }
    if (topic == "accessible_parking" || topic == "parking") {
        return first_query_position(query, {"парков", "машин", "автомоб"});
    }
    if (topic == "ramp") {
        return first_query_position(query, {"пандус", "доступн"});
    }
    if (topic == "staff_call_button") {
        return first_query_position(query, {"кнопк", "вызов", "персонал"});
    }
    if (topic == "pets") {
        return first_query_position(query,
                                    {"живот", "питом", "собак", "кот", "кош", "проводник"});
    }
    if (topic == "warranty" || topic == "warranty_incident") {
        return first_query_position(query,
                                    {"гарант", "скол", "отсл", "повреж", "фото"});
    }
    if (topic == "minors") {
        return first_query_position(query,
                                    {"возраст", "ребен", "подрост", "несовершеннолет", "родител"});
    }
    if (topic == "gift_certificate") {
        return first_query_position(query, {"сертификат", "подароч"});
    }
    if (topic == "wifi") {
        return first_query_position(query, {"wi fi", "wifi", "вайф"});
    }
    if (topic == "coffee") {
        return first_query_position(query, {"кофе", "напит"});
    }
    if (topic == "child_zone") {
        return first_query_position(query, {"детск", "уголок", "комнат"});
    }
    if (topic == "smoking") {
        return first_query_position(query, {"кур", "сигар", "вейп", "парить"});
    }
    if (topic == "product_availability" || topic == "products") {
        return first_query_position(query, {"куп", "товар", "масл", "крем", "средств"});
    }
    if (topic == "service_availability" || topic == "services" ||
        topic == "service_direction" || topic == "service_directions") {
        return first_query_position(query,
                                    {"услуг", "маник", "педик", "бров", "ресниц", "стриж",
                                     "парикмах", "подолог", "подолг", "космет", "эпиляц",
                                     "депиляц", "макияж", "массаж"});
    }

    return std::string_view::npos;
}

[[nodiscard]] std::string join_customer_answers(
        const std::vector<std::string> &parts) {
    auto result = std::string{};

    for (auto part : parts) {
        if (part.empty()) {
            continue;
        }

        if (parts.size() > 1) {
            part = strip_leading_confirmation(std::move(part));
        }

        if (!result.empty()) {
            const auto multiline = result.contains('\n') || part.contains('\n');
            result += multiline ? "\n\n" : " ";
        }

        result += part;
    }

    return result;
}

[[nodiscard]] std::string compact_booking_method_action_label(
        const organization_booking_method_s &method) {
    if (method.id == "phone") {
        return "созвониться по телефону";
    }
    if (method.id == "yandex_maps") {
        return "через Яндекс.Карты (самостоятельная запись)";
    }
    if (method.id == "website") {
        return "на сайте";
    }

    return booking_method_label(method);
}

[[nodiscard]] std::string compact_booking_methods_text(
        const organization_config_s &config) {
    auto labels = std::vector<std::string>{};

    for (const auto &method : ordered_booking_methods(config)) {
        labels.push_back(compact_booking_method_action_label(method));
    }

    if (labels.empty()) {
        return {};
    }

    auto result = std::string{"Мы предлагаем следующие способы записи:\n"};
    result += em_dash_list(labels);
    result += "\n\n";
    result += "Уточните, пожалуйста, какой способ записи на услугу для Вас наиболее удобный? 😊";

    if (labels.size() >= 2) {
        result += "\n\n";
        result += "Примечание: наши клиенты чаще всего выбирают первые два способа 😎";
    }

    return result;
}

[[nodiscard]] std::optional<organization_config_answer_s>
booking_answer_with_service_context(
        const organization_config_s &config,
        const booking_request_s &request,
        const std::string_view normalized_query) {
    auto answer = booking_answer(config, request, normalized_query);

    if (!answer.has_value()) {
        return std::nullopt;
    }

    const auto *service = find_matching_service(config, normalized_query, true);

    if (service == nullptr) {
        return answer;
    }

    if (!service_is_available(config, *service)) {
        if (best_exact_service_candidate(*service, normalized_query).empty()) {
            return answer;
        }

        if (auto direction_answer = generic_unavailable_service_direction_answer(
                    config,
                    *service,
                    normalized_query);
            direction_answer.has_value()) {
            return direction_answer;
        }

        return specific_service_answer(config, normalized_query, false);
    }

    if (!has_specific_exact_service_match(*service, normalized_query)) {
        return answer;
    }

    const auto confirmation = std::format(
            "Да, записаться на услугу «{}» можно",
            service->name);
    answer->fact_text = append_sentence(sentence(confirmation), answer->fact_text);

    if (request.general) {
        if (const auto methods = compact_booking_methods_text(config); !methods.empty()) {
            answer->customer_text = sentence(confirmation) + "\n\n" + methods;
        } else {
            answer->customer_text = sentence(confirmation);
        }
    } else {
        answer->customer_text = append_sentence(sentence(confirmation),
                                                answer->customer_text);
    }
    return answer;
}

[[nodiscard]] bool master_selection_intent(const std::string_view query) noexcept {
    return contains_any(query,
                        {
                                "к какому мастеру",
                                "к какому специалисту",
                                "к кому из мастеров",
                                "какого мастера лучше выбрать",
                                "какого мастера выбрать",
                                "кого из мастеров выбрать",
                                "кого из мастеров посоветуете",
                                "посоветуйте мастера",
                                "порекомендуйте мастера",
                                "лучший мастер",
                            }) ||
           (contains_any_word_prefix(query, {"мастер", "специалист"}) &&
            contains_any_word_prefix(query,
                                     {
                                             "выбр",
                                             "лучш",
                                             "посовет",
                                             "рекоменд",
                                         }));
}

[[nodiscard]] organization_config_answer_s master_selection_answer(
        const organization_config_s &config) {
    const auto is_beauty_salon =
            config.business_type == organization_business_type_e::beauty_salon;
    const auto standards = is_beauty_salon
                                   ? std::string_view{"салона и индустрии"}
                                   : std::string_view{"организации и профессиональной сферы"};
    const auto service_name = is_beauty_salon
                                      ? std::string_view{"процедуру"}
                                      : std::string_view{"услугу"};

    auto customer = std::format(
            "Вы можете смело выбрать любого мастера, чьё время и дата Вам больше подходят. Мы не делим команду на категории, так как берём на работу только опытных и дипломированных профессионалов.[[friendly: 😎]] Каждый специалист работает по единым стандартам {}, поэтому качество услуги будет одинаково высоким в любом случае.[[friendly: 😊]]",
            standards);
    customer += "\n\n";
    customer += std::format(
            "Подскажите, на какую {} Вы хотите записаться? Я посмотрю свободные окошки.[[friendly: 🌿]]",
            service_name);

    auto answer = make_answer(config,
                              "master_selection",
                              "Можно выбрать любого доступного мастера: специалисты работают по единым стандартам",
                              std::move(customer));
    answer.emoji.clear();
    return answer;
}

[[nodiscard]] bool has_dynamic_request_blocker(const std::string_view query) {
    const auto has_software_context = contains_any(query,
                                                   {
                                                           "в программе",
                                                           "в системе",
                                                           "в приложении",
                                                           "в личном кабинете",
                                                           "в админке",
                                                       });
    const auto asks_to_change_configuration = contains_any_word_prefix(
            query,
            {
                    "настро",
                    "измен",
                    "редакт",
                    "созда",
                    "добав",
                    "удал",
                    "указ",
                    "сохран",
                });

    if (has_software_context && asks_to_change_configuration) {
        return true;
    }

    if (contains_any(query,
                     {
                             "сколько стоит",
                             "какая цена",
                             "какие цены",
                             "прайс",
                             "свободное время",
                             "свободные окна",
                             "кто работает",
                             "какой мастер работает",
                             "какой тренер работает",
                             "есть ли сейчас в наличии",
                             "осталось ли топливо",
                     }) ||
        (contains_any_word(query, {"сейчас", "сегодня"}) &&
         contains_any_word_prefix(query, {"налич", "остал"}))) {
        return true;
    }

    const auto mentions_employee = contains_any_word(query,
                                                     {
                                                             "мастер",
                                                             "мастера",
                                                             "мастеру",
                                                             "тренер",
                                                             "тренера",
                                                             "тренеру",
                                                             "сотрудник",
                                                             "сотрудника",
                                                             "сотруднику",
                                                             "бариста",
                                                             "администратор",
                                                             "администратора",
                                                             "парикмахер",
                                                             "парикмахера",
                                                     });
    const auto asks_employee_schedule = contains_any_word_prefix(query,
                                                                 {
                                                                         "работ",
                                                                         "график",
                                                                         "смен",
                                                                         "время",
                                                                 });

    return mentions_employee && asks_employee_schedule;
}

[[nodiscard]] bool organization_noun_intent(const std::string_view query) {
    return contains_any_word(query,
                             {
                                     "организация",
                                     "организации",
                                     "организацию",
                                     "организацией",
                                     "компания",
                                     "компании",
                                     "компанию",
                                     "салон",
                                     "салона",
                                     "салону",
                                     "салоне",
                                     "салоном",
                                     "студия",
                                     "студии",
                                     "студию",
                                     "кофейня",
                                     "кофейни",
                                     "кофейню",
                                     "парикмахерская",
                                     "парикмахерской",
                                     "парикмахерскую",
                                     "азс",
                                     "заправка",
                                     "заправки",
                                     "заправку",
                                     "спортзал",
                                     "спортзала",
                                     "спортзале",
                                     "фитнес",
                             });
}

[[nodiscard]] schedule_request_s detect_schedule_request(
        const std::string_view query) noexcept {
    auto request = schedule_request_s{};

    const auto asks_current_status = contains_any_word(query,
                                                       {
                                                               "сегодня",
                                                               "сейчас",
                                                               "теперь",
                                                           }) &&
                                     contains_any_word_prefix(query,
                                                              {
                                                                      "открыт",
                                                                      "закрыт",
                                                                      "работает",
                                                                  });

    if (asks_current_status) {
        return request;
    }

    request.december_31 = contains_any(query,
                                       {
                                               "31 декабря",
                                               "тридцать первого декабря",
                                           });
    request.january_1_or_2 = contains_any(query,
                                          {
                                                  "1 января",
                                                  "2 января",
                                                  "1 и 2 января",
                                                  "первого января",
                                                  "второго января",
                                                  "первого и второго января",
                                              });
    request.new_year = request.december_31 || request.january_1_or_2 ||
                       contains_any(query,
                                    {
                                            "новогодние праздники",
                                            "новый год",
                                            "январские праздники",
                                        });
    request.holidays = !request.new_year &&
                       contains_any(query,
                                    {
                                            "праздничные дни",
                                            "в праздники",
                                            "на праздники",
                                            "по праздникам",
                                        });

    request.closing_time = contains_any(query,
                                        {
                                                "до скольки",
                                                "до какого времени",
                                                "когда закрываетесь",
                                                "когда закрывается",
                                                "во сколько закрываетесь",
                                                "во сколько закрывается",
                                                "время закрытия",
                                            });
    request.opening_time = contains_any(query,
                                        {
                                                "со скольки",
                                                "с какого времени",
                                                "когда открываетесь",
                                                "когда открывается",
                                                "во сколько открываетесь",
                                                "во сколько открывается",
                                                "время открытия",
                                            });
    request.days = contains_any(query,
                                {
                                        "в какие дни",
                                        "по каким дням",
                                        "какие дни недели",
                                        "дни недели вы работаете",
                                        "рабочие дни",
                                        "по выходным",
                                    });
    request.full = contains_any(query,
                                {
                                        "график работы",
                                        "режим работы",
                                        "как вы работаете",
                                        "как мы работаем",
                                        "какой у вас график",
                                        "какой у нас график",
                                    });

    const auto weekday_prefixes = std::array{
            std::pair{std::string_view{"понедель"}, std::string_view{"monday"}},
            std::pair{std::string_view{"вторник"}, std::string_view{"tuesday"}},
            std::pair{std::string_view{"четверг"}, std::string_view{"thursday"}},
            std::pair{std::string_view{"пятниц"}, std::string_view{"friday"}},
            std::pair{std::string_view{"суббот"}, std::string_view{"saturday"}},
            std::pair{std::string_view{"воскрес"}, std::string_view{"sunday"}},
    };

    for (const auto &[prefix, id] : weekday_prefixes) {
        if (contains_word_prefix(query, prefix)) {
            request.weekdays.push_back(id);
        }
    }

    if (contains_any_word(query,
                          {
                                  "среда",
                                  "среды",
                                  "среду",
                                  "среде",
                                  "средой",
                                  "средам",
                                  "средами",
                                  "средах",
                              })) {
        request.weekdays.push_back("wednesday");
    }

    if (!request.weekdays.empty()) {
        request.days = false;
        request.full = false;
    }

    if ((request.holidays || request.new_year) &&
        !contains_any(query, {"обычный график", "основной график"})) {
        request.full = false;
    }

    if (request.matched()) {
        return request;
    }

    const auto has_work_state = contains_any_word_prefix(query,
                                                         {
                                                                 "работ",
                                                                 "открыва",
                                                                 "закрыва",
                                                             });
    const auto asks_days = contains_any_word_prefix(query, {"дн", "недел"});
    const auto asks_time = contains_any_word_prefix(query,
                                                    {
                                                            "когда",
                                                            "скольк",
                                                            "время",
                                                            "час",
                                                        });

    if (has_work_state && asks_days) {
        request.days = true;
    } else if (has_work_state && asks_time) {
        request.full = true;
    }

    return request;
}

[[nodiscard]] address_request_s detect_address_request(
        const std::string_view query) {
    auto request = address_request_s{};

    const auto asks_location = contains_any_word_prefix(query, {"наход", "располож"}) &&
                               (organization_noun_intent(query) ||
                                contains_any_word(query, {"вы", "вас", "вам"}));
    const auto asks_route = contains_any_word_prefix(query,
                                                     {
                                                             "добрат",
                                                             "проезд",
                                                             "маршрут",
                                                             "пройт",
                                                             "идти",
                                                             "ехать",
                                                         }) &&
                            (contains_any(query, {"к вам", "до вас", "до салона"}) ||
                             organization_noun_intent(query) ||
                             contains_any_word_prefix(query, {"метро", "останов"}));
    const auto asks_route_duration = contains_any(query,
                                                  {
                                                          "сколько идти",
                                                          "сколько ехать",
                                                          "сколько минут идти",
                                                          "далеко от метро",
                                                      });
    const auto asks_entrance = contains_word_prefix(query, "вход") &&
                               (contains_any_word_prefix(query,
                                                         {
                                                                 "где",
                                                                 "как",
                                                                 "найт",
                                                                 "отдельн",
                                                                 "сторон",
                                                             }) ||
                                contains_any(query,
                                             {
                                                     "где находится вход",
                                                     "как найти вход",
                                                     "с какой стороны вход",
                                                 }));

    request.address = contains_word_prefix(query, "адрес") || asks_location;
    request.directions = asks_route || asks_route_duration;
    request.landmark = contains_word_prefix(query, "ориентир") ||
                       (request.directions && !asks_route_duration);
    request.entrance = asks_entrance ||
                       (request.directions && !asks_route_duration);

    return request;
}

[[nodiscard]] payment_request_s detect_payment_request(
        const std::string_view query) noexcept {
    auto request = payment_request_s{};

    const auto asks_payment = contains_any_word_prefix(query,
                                                       {
                                                               "оплат",
                                                               "платеж",
                                                               "рассчит",
                                                               "расч",
                                                               "предоплат",
                                                           });
    const auto asks_availability = contains_any_word_prefix(query,
                                                            {
                                                                    "мож",
                                                                    "принима",
                                                                    "доступ",
                                                                    "есть",
                                                                    "нуж",
                                                                    "треб",
                                                                    "примет",
                                                                    "долж",
                                                                });
    const auto asks_general_methods = contains_any(query,
                                                   {
                                                           "способы оплаты",
                                                           "методы оплаты",
                                                           "варианты оплаты",
                                                           "формы оплаты",
                                                           "виды оплаты",
                                                           "варианты расчета",
                                                           "варианты расчёта",
                                                           "формы расчета",
                                                           "формы расчёта",
                                                           "как можно оплатить",
                                                           "как у вас можно оплатить",
                                                           "как у вас оплатить",
                                                           "как оплатить",
                                                           "чем можно оплатить",
                                                           "чем можно расплатиться",
                                                           "чем можно рассчитаться",
                                                           "какую оплату принимаете",
                                                           "какие варианты расчета",
                                                           "какие варианты расчёта",
                                                       }) ||
                                     (asks_payment &&
                                      contains_any_word_prefix(query,
                                                               {
                                                                       "способ",
                                                                       "метод",
                                                                       "вариант",
                                                                       "форм",
                                                                       "вид",
                                                                       "чем",
                                                                   }));

    const auto mentions_advance_transfer =
            contains_word_prefix(query, "заранее") &&
            contains_word_prefix(query, "перевод");
    const auto mentions_paid_advance =
            (contains_any_word_prefix(query,
                                      {
                                              "перевела",
                                              "перевел",
                                              "переводила",
                                              "переводил",
                                              "внесла",
                                              "внес",
                                              "вносила",
                                              "вносил",
                                          }) &&
             contains_any_word_prefix(query,
                                      {
                                              "заранее",
                                              "деньг",
                                              "предоплат",
                                          })) ||
            contains_any(query,
                         {
                                 "предоплата была",
                                 "уже внесла предоплату",
                                 "уже внес предоплату",
                                 "уже вносила предоплату",
                                 "уже вносил предоплату",
                             });

    request.general = asks_general_methods;
    request.prepayment = contains_any_word_prefix(query, {"предоплат"}) ||
                         (mentions_advance_transfer && !mentions_paid_advance);
    request.prepayment_paid = mentions_paid_advance;
    request.postpayment = asks_payment &&
                          contains_any(query,
                                       {
                                               "после услуги",
                                               "после оказания услуги",
                                               "после процедуры",
                                               "оплата после",
                                               "оплатить после",
                                               "платить после",
                                               "в конце визита",
                                               "в конце посещения",
                                               "в конце процедуры",
                                               "в конце приема",
                                               "в конце приёма",
                                           });

    const auto mentions_cash = contains_any_word_prefix(query, {"налич"});
    const auto mentions_credit_card =
            contains_any_word_prefix(query, {"кредитн", "кредитк"}) &&
            (contains_word_prefix(query, "карт") ||
             contains_any_word_prefix(query, {"кредитк"}));
    const auto mentions_regular_card =
            (contains_any_word(query,
                               {
                                       "картой",
                                       "карта",
                                       "карты",
                                       "карту",
                                       "карточкой",
                                       "банковская",
                                       "банковской",
                                       "банковскую",
                                   }) ||
             (asks_payment && contains_word_prefix(query, "карт"))) &&
            (!mentions_credit_card || contains_any_word_prefix(query, {"обычн", "дебетов"}));
    const auto mentions_qr =
            contains_any_word(query, {"qr", "куар"}) ||
            contains_any(query, {"qr код", "qr code", "по qr"}) ||
            (asks_payment && contains_word_prefix(query, "код") &&
             contains_any_word_prefix(query, {"камер", "счит", "скан"}));
    const auto mentions_sbp = contains_any_word(query, {"сбп"}) ||
                              contains_any(query, {"система быстрых платежей"});
    const auto mentions_online = contains_any_word_prefix(query, {"онлайн"});
    const auto mentions_gift_certificate = contains_any_word_prefix(query,
                                                                    {
                                                                            "сертификат",
                                                                            "абонемент",
                                                                        });
    const auto asks_certificate_validity =
            mentions_gift_certificate &&
            contains_any_word_prefix(query,
                                     {
                                             "действующ",
                                             "валидн",
                                             "просроч",
                                             "срок",
                                         });
    const auto mentions_on_site =
            (asks_payment || contains_word_prefix(query, "расч")) &&
            contains_any(query,
                         {
                                 "на месте",
                                 "у администратора",
                                 "в салоне",
                             });
    const auto mentions_map_platform = contains_any_word_prefix(query,
                                                                 {
                                                                         "яндекс",
                                                                         "google",
                                                                         "гугл",
                                                                         "2гис",
                                                                         "дубльгис",
                                                                     });
    const auto card_is_unambiguous = mentions_regular_card &&
                                     (asks_payment ||
                                      (!mentions_map_platform &&
                                       contains_any_word_prefix(query,
                                                                {
                                                                        "мож",
                                                                        "принима",
                                                                    })));
    const auto has_unambiguous_method = mentions_cash || card_is_unambiguous ||
                                        mentions_credit_card || mentions_qr || mentions_sbp ||
                                        mentions_gift_certificate;

    if (asks_payment || (asks_availability && has_unambiguous_method) ||
        asks_certificate_validity || mentions_on_site ||
        request.prepayment || request.prepayment_paid || request.postpayment) {
        request.cash = mentions_cash;
        request.card = mentions_regular_card;
        request.credit_card = mentions_credit_card;
        request.qr_code = mentions_qr;
        request.sbp = mentions_sbp;
        request.online = asks_payment && mentions_online;
        request.gift_certificate = mentions_gift_certificate;
        request.on_site = mentions_on_site;
        request.certificate_validity = asks_certificate_validity;
    }

    return request;
}

[[nodiscard]] booking_request_s detect_booking_request(
        const organization_config_s &config,
        const std::string_view query) {
    auto request = booking_request_s{};

    if (contains_any_word_prefix(query,
                                 {
                                         "отмен",
                                         "перенес",
                                         "измен",
                                         "удал",
                                         "опазд",
                                         "предоплат",
                                 }) ||
        contains_any(query,
                     {
                             "запишите меня",
                             "запиши меня",
                             "хочу записаться",
                             "хотела записаться",
                             "хотел записаться",
                             "свободное время",
                             "свободные окна",
                             "ближайшее время",
                             "ближайшая запись",
                             "на сегодня",
                             "на завтра",
                             "записать клиента",
                             "записывать клиента",
                             "создать запись",
                             "добавить запись",
                             "в программе",
                             "в системе",
                             "в журнале",
                     })) {
        return request;
    }

    const auto asks_booking = contains_word_prefix(query, "запис") ||
                              contains_word_prefix(query, "брон") ||
                              contains_word_prefix(query, "заброн") ||
                              contains_any(query,
                                           {
                                                   "онлайн запись",
                                                   "способы бронирования",
                                                   "как забронировать",
                                               });

    if (!asks_booking) {
        return request;
    }

    const auto request_method = [&](const bool mentioned,
                                    const std::string_view method_id) {
        if (mentioned) {
            append_requested_booking_method(request, method_id);
        }
    };

    request_method(contains_any_word_prefix(query,
                                            {
                                                    "телефон",
                                                    "позвон",
                                                    "звонк",
                                                    "номер",
                                                }),
                   "phone");
    request_method(contains_any_word_prefix(query, {"сайт", "web", "веб"}) ||
                           contains_any(query,
                                        {
                                                "через интернет",
                                                "на странице сайта",
                                            }),
                   "website");
    request_method(contains_any_word_prefix(query,
                                            {
                                                    "телег",
                                                    "telegram",
                                                }) ||
                           contains_any_word(query, {"тг", "tg"}),
                   "telegram");
    request_method(contains_any_word_prefix(query,
                                            {
                                                    "whatsapp",
                                                    "ватсап",
                                                    "вацап",
                                                    "вотсап",
                                                }) ||
                           contains_any(query, {"whats app", "ватс апп"}),
                   "whatsapp");
    request_method(contains_any_word(query,
                                     {
                                             "max",
                                             "макс",
                                             "мах",
                                             "госмессенджер",
                                             "говномессенджер",
                                         }),
                   "max");
    request_method(contains_any_word_prefix(query, {"яндекс"}) ||
                           contains_any(query, {"я карты", "я картах"}),
                   "yandex_maps");
    request_method(contains_any_word_prefix(query, {"2гис", "дубльгис"}),
                   "2gis");
    request_method(contains_any_word_prefix(query, {"google", "гугл"}),
                   "google_maps");

    /*
     * Besides well-known aliases, match identifiers and labels stored in the
     * config. This lets a future settings page add a booking channel without
     * requiring a new hard-coded phrase for its exact display name.
     */
    for (const auto &method : config.booking_methods) {
        const auto normalized_id = normalize_query(method.id);
        const auto normalized_label = normalize_query(method.label);

        if ((!normalized_id.empty() && query.contains(normalized_id)) ||
            (!normalized_label.empty() && query.contains(normalized_label))) {
            append_requested_booking_method(request, method.id);
        }
    }

    if (request.has_specific_methods()) {
        return request;
    }

    request.general = contains_any(query,
                                   {
                                           "как записаться",
                                           "где записаться",
                                           "способы записи",
                                           "варианты записи",
                                           "через что записаться",
                                           "как можно записаться",
                                           "как я могу записаться",
                                           "можно ли записаться",
                                           "онлайн запись",
                                       }) ||
                      contains_any_word(query, {"как", "где"}) ||
                      contains_any_word_prefix(query,
                                               {
                                                       "способ",
                                                       "вариант",
                                                       "онлайн",
                                                   });

    return request;
}

[[nodiscard]] bool identity_intent(const std::string_view query) noexcept {
    if (contains_any(query,
                     {
                             "как вы называетесь",
                             "ваше название",
                             "ваше наименование",
                     })) {
        return true;
    }

    const auto asks_name = contains_any_word_prefix(query,
                                                    {
                                                            "называ",
                                                            "назван",
                                                            "наименован",
                                                            "бренд",
                                                    });

    return asks_name && organization_noun_intent(query);
}

[[nodiscard]] bool general_contacts_intent(const std::string_view query) noexcept {
    return contains_any(query,
                        {
                                "как с вами связаться",
                                "как с вами можно связаться",
                                "ваши контакты",
                                "контакты организации",
                                "контактные данные",
                            });
}

[[nodiscard]] bool phone_intent(const std::string_view query) noexcept {
    if (contains_word_prefix(query, "запис")) {
        return false;
    }

    return contains_any(query,
                        {
                                "номер телефона",
                                "ваш телефон",
                                "ваш номер",
                                "по какому номеру",
                                "куда позвонить",
                                "как позвонить",
                            }) ||
           (contains_word_prefix(query, "позвон") &&
            (contains_any_word(query, {"как", "куда", "вам", "вас", "вы"}) ||
             contains_word_prefix(query, "мож"))) ||
           (contains_word_prefix(query, "телефон") &&
            contains_any_word_prefix(query, {"номер", "контакт", "как", "какой"}));
}

[[nodiscard]] bool has_messenger_request_cue(const std::string_view query) noexcept {
    return contains_any_word_prefix(query,
                                    {
                                            "есть",
                                            "ваш",
                                            "наш",
                                            "контакт",
                                            "сообщ",
                                            "принима",
                                            "напис",
                                            "связ",
                                            "адрес",
                                            "аккаунт",
                                            "мессенджер",
                                            "мож",
                                        }) ||
           contains_any(query, {"у вас", "у нас"});
}

[[nodiscard]] bool telegram_intent(const std::string_view query) noexcept {
    if (contains_word_prefix(query, "запис")) {
        return false;
    }

    const auto mentions_telegram = contains_any_word_prefix(query,
                                                            {
                                                                    "телеграм",
                                                                    "telegram",
                                                                }) ||
                                   contains_any_word(query, {"телега", "тг", "tg"});

    return mentions_telegram && has_messenger_request_cue(query);
}

[[nodiscard]] bool whatsapp_intent(const std::string_view query) noexcept {
    if (contains_word_prefix(query, "запис")) {
        return false;
    }

    const auto mentions_whatsapp = contains_any_word_prefix(query,
                                                            {
                                                                    "whatsapp",
                                                                    "ватсап",
                                                                    "вацап",
                                                                    "вотсап",
                                                                }) ||
                                   contains_any(query, {"whats app", "ватс апп"});

    return mentions_whatsapp && has_messenger_request_cue(query);
}

[[nodiscard]] bool website_intent(const std::string_view query) noexcept {
    if (contains_word_prefix(query, "запис") ||
        !contains_any_word_prefix(query, {"сайт", "web", "веб"})) {
        return false;
    }

    return contains_any_word_prefix(query,
                                    {
                                            "есть",
                                            "ваш",
                                            "наш",
                                            "адрес",
                                            "ссыл",
                                            "официальн",
                                            "найт",
                                        });
}

[[nodiscard]] bool max_messenger_intent(const std::string_view query) noexcept {
    if (contains_word_prefix(query, "запис")) {
        return false;
    }

    const auto mentions_max = contains_any_word(query,
                                                {
                                                        "max",
                                                        "макс",
                                                        "мах",
                                                        "госмессенджер",
                                                        "говномессенджер",
                                                    });

    return mentions_max && has_messenger_request_cue(query);
}

[[nodiscard]] bool map_listing_intent(const std::string_view query,
                                      const std::initializer_list<std::string_view> platform_prefixes) noexcept {
    if (contains_word_prefix(query, "запис") ||
        !contains_any_word_prefix(query, platform_prefixes)) {
        return false;
    }

    return contains_any_word_prefix(query,
                                    {
                                            "есть",
                                            "точк",
                                            "карточ",
                                            "профил",
                                            "страниц",
                                            "найт",
                                            "представлен",
                                        }) ||
           contains_any(query,
                        {
                                "вы на картах",
                                "вы есть на картах",
                            });
}

[[nodiscard]] bool contains_any_direction_stem(
        const std::string_view query,
        const std::initializer_list<std::string_view> stems) {
    const auto words = split_words(query);

    for (const auto word : words) {
        for (const auto stem : stems) {
            if (word.starts_with(stem) ||
                (word.size() >= 6 && stem.size() >= 6 && common_prefix_codepoints(word, stem) >= 4)) {
                return true;
            }
        }
    }

    return false;
}

[[nodiscard]] std::vector<std::string_view> beauty_directions_from_query(
        const std::string_view query) {
    auto result = std::vector<std::string_view>{};
    const auto append = [&](const bool matched, const std::string_view direction) {
        if (matched && std::ranges::find(result, direction) == result.end()) {
            result.push_back(direction);
        }
    };

    const auto mentions_manicure =
            contains_any_direction_stem(query, {"маник"}) ||
            contains_any_word_prefix(query, {"рук", "кист"});
    const auto mentions_pedicure =
            contains_any_direction_stem(query,
                                        {"педик", "стоп", "пятк", "ногах", "ногам"});
    const auto mentions_generic_nails =
            contains_any_word_prefix(query, {"ногт", "ногот"});

    if (mentions_generic_nails && !mentions_manicure && !mentions_pedicure) {
        append(true, "manicure");
        append(true, "pedicure");
    } else {
        append(mentions_manicure, "manicure");
        append(mentions_pedicure, "pedicure");
    }

    append(contains_any_direction_stem(query,
                                       {"бров", "бровк", "бровист", "архитектур"}),
           "brows");
    append(contains_any_direction_stem(query,
                                       {"ресниц", "реснич", "lash", "леш"}),
           "eyelashes");
    append(contains_any_direction_stem(query,
                                       {"парикмах", "стриж", "подстр", "уклад", "причес", "причёс",
                                        "волос", "окрашивание волос", "колорист", "барбер", "челк"}),
           "hairdressing");
    append(contains_any_direction_stem(query,
                                       {"косметолог", "косметич", "пилинг", "чистк", "уход", "anti",
                                        "антиэйдж", "омолож"}) ||
                   contains_any(query, {"anti age", "анти эйдж"}),
           "cosmetology");
    append(contains_any_direction_stem(query,
                                       {"эпиляц", "депиляц", "шугар", "лазерн"}) ||
                   contains_any_word(query, {"воск", "воском"}) ||
                   contains_word_prefix(query, "восков"),
           "hair_removal");
    append(contains_any_direction_stem(query,
                                       {"макияж", "мейк", "makeup", "визаж", "смоки"}),
           "makeup");
    append(contains_any_direction_stem(query,
                                       {"массаж", "массажист", "спин", "шейн", "релакс"}),
           "massage");
    append(contains_any_direction_stem(query,
                                       {"подолог", "подолг", "подологичес", "медицинск", "вросш",
                                        "мозол", "натоптыш", "трещин", "грибок"}),
           "podology");

    return result;
}

[[nodiscard]] bool beauty_directions_intent(const std::string_view query) noexcept {
    return contains_any(query,
                        {
                                "направления услуг",
                                "направления салона",
                                "профиль салона",
                                "какими направлениями",
                                "какие направления",
                                "что у вас за салон",
                            });
}

[[nodiscard]] bool direction_services_intent(const std::string_view query) noexcept {
    return contains_any(query,
                        {
                                "какие услуги",
                                "перечень услуг",
                                "список услуг",
                                "услуги связанные",
                                "услуги по",
                                "услуги",
                                "услуг",
                                "услуга",
                                "услуга по",
                                "направление",
                                "что есть по",
                                "что делаете по",
                                "какие процедуры",
                            });
}

[[nodiscard]] bool minors_intent(const std::string_view query) noexcept {
    if (contains_any(query,
                     {
                             "детский уголок",
                             "детская зона",
                             "детская комната",
                         })) {
        return false;
    }

    return contains_any(query,
                        {
                                "с какого возраста",
                                "с какого возраста можно",
                                "для детей",
                            }) ||
           parent_presence_intent(query) ||
           contains_any_word_prefix(query,
                                    {
                                            "детск",
                                            "ребен",
                                            "подрост",
                                            "несовершеннолет",
                                        }) ||
           contains_any_word(query, {"дети", "детей", "детям"}) ||
           (extract_requested_age(query).has_value() &&
            contains_any_word_prefix(query, {"лет", "год"}));
}

[[nodiscard]] bool accessible_parking_intent(const std::string_view query) noexcept {
    const auto has_accessibility_marker =
            contains_any_word_prefix(query,
                                     {
                                             "инвалид",
                                             "маломобил",
                                             "колясоч",
                                         }) ||
            contains_any(query,
                         {
                                 "ограниченными возможностями",
                                 "для мгн",
                             });

    return has_accessibility_marker &&
           contains_any_word_prefix(query, {"парков", "мест"});
}

[[nodiscard]] bool gift_certificate_intent(const std::string_view query) noexcept {
    return contains_any_word_prefix(query, {"сертификат"}) &&
           contains_any_word_prefix(query, {"подар", "куп", "оформ", "есть"});
}

[[nodiscard]] bool staff_call_button_intent(const std::string_view query) noexcept {
    return contains_any(query,
                        {
                                "кнопка вызова персонала",
                                "кнопка вызова сотрудника",
                                "кнопка помощи",
                            }) ||
           (contains_word_prefix(query, "кнопк") &&
            contains_any_word_prefix(query, {"персонал", "сотрудник", "вызов"}));
}

[[nodiscard]] bool ramp_intent(const std::string_view query) noexcept {
    return contains_any_word_prefix(query, {"пандус", "рампа"}) ||
           contains_any(query,
                        {
                                "безбарьерный вход",
                                "доступный вход",
                            });
}

[[nodiscard]] bool services_intent(const std::string_view query) noexcept {
    return contains_any(query,
                        {
                                "какие услуги",
                                "список услуг",
                                "перечень услуг",
                                "что входит в услуги",
                                "что из процедур",
                                "какие процедуры",
                                "список процедур",
                                "что мастера делают",
                                "что делают мастера",
                                "что мастер делает",
                            }) ||
           (contains_any_word_prefix(query, {"список", "перечень"}) &&
            contains_any_word_prefix(query, {"дела", "выполня"}) &&
            contains_any_word_prefix(query, {"мастер", "процедур", "услуг"}));
}

[[nodiscard]] bool wifi_intent(const std::string_view query) noexcept {
    return contains_any(query, {"wi fi", "вай фай"}) ||
           contains_any_word_prefix(query,
                                    {
                                            "wifi",
                                            "вайфай",
                                        });
}

[[nodiscard]] bool parking_intent(const std::string_view query) noexcept {
    return contains_any_word_prefix(query,
                                    {
                                            "парков",
                                            "автопарков",
                                            "припарков",
                                        });
}

[[nodiscard]] bool service_availability_intent(const std::string_view query) noexcept {
    if (contains_any(query,
                     {
                             "что делать",
                             "как поступить",
                             "что предпринять",
                         })) {
        return false;
    }

    return contains_any(query,
                        {
                                "делаете ли",
                                "делаете вы",
                                "можно ли у вас",
                                "есть ли у вас",
                                "оказываете ли",
                                "предлагаете ли",
                                "выполняете ли",
                                "принимает ли",
                                "работает ли",
                                "у вас есть",
                                "есть у вас",
                                "есть в салоне",
                                "можно к вам",
                            }) ||
           contains_any_word_prefix(query,
                                    {
                                            "дела",
                                            "сдел",
                                            "оказыва",
                                            "предлага",
                                            "выполня",
                                            "принима",
                                            "работа",
                                            "доступ",
                                            "оформ",
                                            "наращ",
                                            "покрас",
                                        });
}

[[nodiscard]] bool explicit_unknown_service_intent(const std::string_view query) noexcept {
    return contains_any_word_prefix(query,
                                    {
                                            "услуг",
                                            "парикмахер",
                                            "стриж",
                                            "уклад",
                                            "маник",
                                            "педик",
                                            "бров",
                                            "ресниц",
                                            "косметолог",
                                            "массаж",
                                            "депиляц",
                                            "эпиляц",
                                            "подолог",
                                            "подолг",
                                            "макияж",
                                            "визаж",
                                        });
}

[[nodiscard]] bool product_purchase_intent(const std::string_view query) noexcept {
    return contains_any_word_prefix(query,
                                    {
                                            "куп",
                                            "прода",
                                            "приобрест",
                                        }) ||
           contains_any(query,
                        {
                                "есть ли у вас",
                                "можно ли купить",
                                "можно приобрести",
                            });
}

[[nodiscard]] bool warranty_damage_intent(const std::string_view query) noexcept {
    const auto mentions_damage = contains_any_word_prefix(query,
                                                           {
                                                                   "скол",
                                                                   "отсло",
                                                                   "отсла",
                                                                   "поврежд",
                                                                   "механ",
                                                                   "трес",
                                                                   "слом",
                                                                   "отвал",
                                                                   "отход",
                                                                   "сходит",
                                                                   "сош",
                                                                   "слез",
                                                                   "облез",
                                                                   "облуп",
                                                               });
    const auto mentions_service_result = contains_any_word_prefix(query,
                                                                   {
                                                                           "покрыт",
                                                                           "гель",
                                                                           "лак",
                                                                           "ногт",
                                                                       });

    return mentions_damage && mentions_service_result;
}

[[nodiscard]] bool warranty_photo_intent(const std::string_view query) noexcept {
    const auto mentions_photo = contains_any_word_prefix(query, {"фото", "сним"});
    const auto mentions_case = contains_any_word_prefix(query,
                                                         {
                                                                 "поврежд",
                                                                 "покрыт",
                                                                 "гарант",
                                                                 "скол",
                                                                 "отсло",
                                                             });

    return mentions_photo && mentions_case;
}

[[nodiscard]] bool smoking_intent(const std::string_view query) noexcept {
    return contains_any_word_prefix(query,
                                    {
                                            "кур",
                                            "сигарет",
                                            "вейп",
                                            "парить",
                                            "электронк",
                                        }) ||
           contains_any(query,
                        {
                                "электронные сигареты",
                                "электронная сигарета",
                            });
}

[[nodiscard]] bool warranty_intent(const std::string_view query) noexcept {
    return contains_word_prefix(query, "гарант") ||
           contains_any(query,
                        {
                                "по гарантии",
                                "гарантийный случай",
                                "гарантийные случаи",
                            });
}

[[nodiscard]] bool pets_intent(const std::string_view query) noexcept {
    const auto mentions_pet = contains_any_word_prefix(query,
                                                       {
                                                               "животн",
                                                               "питом",
                                                               "собак",
                                                               "собач",
                                                               "кошк",
                                                               "щен",
                                                           }) ||
                              contains_any_word(query,
                                                {
                                                        "кот",
                                                        "кота",
                                                        "коту",
                                                        "котом",
                                                        "коты",
                                                        "котик",
                                                        "котика",
                                                        "котиком",
                                                        "кошак",
                                                        "кошака",
                                                        "кошаком",
                                                        "пес",
                                                        "пса",
                                                        "псом",
                                                        "песик",
                                                        "песика",
                                                    });

    if (!mentions_pet) {
        return false;
    }

    return contains_any_word_prefix(query,
                                    {
                                            "мож",
                                            "пуска",
                                            "пуст",
                                            "допуска",
                                            "допуст",
                                            "прийт",
                                            "приход",
                                            "посет",
                                            "разреш",
                                            "вход",
                                            "предупред",
                                            "сообщ",
                                        }) ||
           contains_any(query,
                        {
                                "к вам с",
                                "у вас с",
                                "с собой",
                            });
}

[[nodiscard]] bool coffee_for_guests_intent(const std::string_view query) noexcept {
    if (!contains_word_prefix(query, "коф")) {
        return false;
    }

    return contains_any_word_prefix(query,
                                    {
                                            "налива",
                                            "угоща",
                                            "предлага",
                                            "бесплат",
                                            "даете",
                                            "дают",
                                            "есть",
                                        }) ||
           contains_any(query,
                        {
                                "кофе для клиентов",
                                "кофе клиентам",
                                "кофе для гостей",
                                "кофе гостям",
                            });
}

[[nodiscard]] bool fuel_types_intent(const std::string_view query) noexcept {
    if (contains_any(query,
                     {
                             "сейчас в наличии",
                             "есть ли сейчас",
                             "осталось ли",
                             "закончился ли",
                     })) {
        return false;
    }

    return contains_any(query,
                        {
                                "какие виды топлива",
                                "какое топливо",
                                "какой бензин",
                                "чем можно заправиться",
                                "виды бензина",
                        });
}

} // namespace

std::string_view to_string(const organization_business_type_e type) noexcept {
    switch (type) {
        case organization_business_type_e::unknown: return "unknown";
        case organization_business_type_e::beauty_salon: return "beauty_salon";
        case organization_business_type_e::coffee_shop: return "coffee_shop";
        case organization_business_type_e::gas_station: return "gas_station";
        case organization_business_type_e::gym: return "gym";
    }

    return "unknown";
}

organization_business_type_e organization_business_type_from_string(
        const std::string_view value) noexcept {
    if (value == "beauty_salon") {
        return organization_business_type_e::beauty_salon;
    }
    if (value == "coffee_shop") {
        return organization_business_type_e::coffee_shop;
    }
    if (value == "hair_salon" || value == "nail_salon" ||
        value == "barbershop" || value == "brow_bar" ||
        value == "lash_studio") {
        return organization_business_type_e::beauty_salon;
    }
    if (value == "gas_station") {
        return organization_business_type_e::gas_station;
    }
    if (value == "gym") {
        return organization_business_type_e::gym;
    }

    return organization_business_type_e::unknown;
}

organization_config_s load_organization_config(const std::filesystem::path &filename) {
    const auto root = json::parse(read_text_file(filename));

    if (!root.is_object()) {
        throw std::runtime_error{"Organization config root must be a JSON object"};
    }

    auto config = organization_config_s{};
    config.schema_version = size_value(root, "schema_version", 1);
    config.enabled = bool_value(root, "enabled", true);
    config.business_type = organization_business_type_from_string(string_value(root, "business_type"));
    config.brand_name = string_value(root, "brand_name");
    config.source_file = filename;

    const auto &contacts = object_value(root, "contacts");
    const auto &address = object_value(contacts, "address");
    config.contacts = organization_contacts_s{
            .address = organization_address_s{
                    .formatted = string_value(address, "formatted"),
                    .directions = string_value(address, "directions"),
                    .landmark = string_value(address, "landmark"),
                    .entrance = string_value(address, "entrance"),
            },
            .phone = string_value(contacts, "phone"),
            .website = string_value(contacts, "website"),
            .telegram = string_value(contacts, "telegram"),
            .whatsapp = string_value(contacts, "whatsapp"),
            .max = string_value(contacts, "max"),
    };

    config.schedule = parse_schedule(object_value(root, "schedule"));
    config.booking_methods = parse_booking_methods(array_value(root, "booking_methods"));

    const auto &payment = object_value(root, "payment_methods");
    config.payment_methods = organization_payment_methods_s{
            .cash = bool_value(payment, "cash"),
            .card = bool_value(payment, "card"),
            .credit_card = bool_value(payment, "credit_card"),
            .qr_code = bool_value(payment, "qr_code"),
            .sbp = bool_value(payment, "sbp"),
            .online = bool_value(payment, "online"),
            .gift_certificate = bool_value_any(payment,
                                               {"gift_certificate",
                                                "certificate",
                                                "certificate_or_subscription",
                                                "gift_certificate_or_subscription"}),
            .on_site = bool_value(payment, "on_site", true),
            .prepayment_required = bool_value(payment, "prepayment_required"),
            .gift_certificate_note = string_value(payment, "gift_certificate_note"),
            .note = string_value(payment, "note"),
    };

    const auto &amenities = object_value(root, "general_amenities");
    const auto &parking = object_value(amenities, "parking");
    const auto &pets = object_value(amenities, "pets");
    config.general_amenities = organization_general_amenities_s{
            .has_wifi = bool_value(amenities, "has_wifi"),
            .has_free_coffee = bool_value(amenities, "has_free_coffee"),
            .has_child_zone = bool_value(amenities, "has_child_zone"),
            .serves_minors = bool_value(amenities, "serves_minors"),
            .minor_parent_presence_required = bool_value(
                    amenities,
                    "minor_parent_presence_required"),
            .has_gift_certificates = bool_value(amenities, "has_gift_certificates"),
            .has_staff_call_button = bool_value(amenities, "has_staff_call_button"),
            .has_ramp = bool_value(amenities, "has_ramp"),
            .smoking_allowed = bool_value(amenities, "smoking_allowed"),
            .smoke_breaks_allowed = bool_value(amenities, "smoke_breaks_allowed"),
            .parking = organization_parking_s{
                    .regular = parking.empty()
                                       ? bool_value(amenities, "has_parking")
                                       : bool_value(parking, "regular"),
                    .accessible = bool_value(parking, "accessible"),
                    .regular_note = parking.empty()
                                            ? string_value(amenities, "parking_note")
                                            : string_value(parking, "regular_note"),
                    .accessible_note = string_value(parking, "accessible_note"),
            },
            .pets = organization_pets_policy_s{
                    .allowed = pets.empty()
                                       ? bool_value(amenities, "pets_allowed")
                                       : bool_value(pets, "allowed"),
                    .small_dogs_allowed = bool_value(pets,
                                                     "small_dogs_allowed"),
                    .max_dog_height_cm = optional_size_value(pets,
                                                             "max_dog_height_cm"),
                    .service_animals_allowed = bool_value(
                            pets,
                            "service_animals_allowed"),
                    .note = pets.empty()
                                    ? string_value(amenities, "pets_note")
                                    : string_value(pets, "note"),
            },
            .wifi_note = string_value(amenities, "wifi_note"),
            .coffee_note = string_value(amenities, "coffee_note"),
            .child_zone_note = string_value(amenities, "child_zone_note"),
            .minors_note = string_value(amenities, "minors_note"),
            .gift_certificates_note = string_value(amenities, "gift_certificates_note"),
            .staff_call_button_note = string_value(amenities, "staff_call_button_note"),
            .ramp_note = string_value(amenities, "ramp_note"),
            .smoking_note = string_value(amenities, "smoking_note"),
    };

    config.business_details = parse_business_details(object_value(root, "business_details"));
    if (config.schema_version < 1 || config.schema_version > 3) {
        throw std::runtime_error{std::format("Unsupported organization config schema version: {}",
                                             config.schema_version)};
    }

    if (!config.general_amenities.serves_minors &&
        std::ranges::any_of(config.business_details.beauty_salon.services,
                            [](const auto &service) {
                                return service.minor_access.has_value() && service.minor_access->allowed;
                            })) {
        throw std::runtime_error{
                "Minor-friendly services require general_amenities.serves_minors=true"};
    }

    if (config.enabled && config.business_type == organization_business_type_e::unknown) {
        throw std::runtime_error{"Enabled organization config has an unknown business_type"};
    }

    return config;
}

std::optional<organization_config_answer_s> answer_from_organization_config(
        const organization_config_s &config,
        const std::string_view user_text) {
    if (!config.enabled || user_text.empty()) {
        return std::nullopt;
    }

    const auto query = normalize_query(user_text);

    if (query.empty()) {
        return std::nullopt;
    }

    if (master_selection_intent(query)) {
        return master_selection_answer(config);
    }

    if (has_dynamic_request_blocker(query)) {
        return std::nullopt;
    }

    auto answers = std::vector<organization_config_answer_s>{};
    auto topics = std::unordered_set<std::string>{};
    const auto schedule_request = detect_schedule_request(query);
    const auto payment_request = detect_payment_request(query);
    const auto booking_request = detect_booking_request(config, query);
    const auto specific_product = product_purchase_intent(query)
                                          ? specific_product_answer(config, query)
                                          : std::nullopt;
    const auto has_specific_product = specific_product.has_value();

    if (!has_specific_product && schedule_request.matched()) {
        append_unique_answer(answers, topics, schedule_answer(config, schedule_request));
    }

    if (const auto address_request = detect_address_request(query);
        address_request.matched()) {
        append_unique_answer(answers,
                             topics,
                             address_answer(config, address_request));
    }

    if (payment_request.matched()) {
        append_unique_answer(answers, topics, payment_answer(config, payment_request));
    }

    if (booking_request.matched()) {
        append_unique_answer(answers,
                             topics,
                             booking_answer_with_service_context(config,
                                                                 booking_request,
                                                                 query));
    }

    if (identity_intent(query)) {
        append_unique_answer(answers, topics, identity_answer(config));
    }

    if (general_contacts_intent(query)) {
        append_unique_answer(answers, topics, contacts_answer(config));
    }

    if (phone_intent(query)) {
        append_unique_answer(answers, topics, phone_answer(config));
    }

    if (telegram_intent(query)) {
        append_unique_answer(answers,
                             topics,
                             messenger_answer(config,
                                              "telegram",
                                              "Telegram",
                                              config.contacts.telegram));
    }

    if (whatsapp_intent(query)) {
        append_unique_answer(answers,
                             topics,
                             messenger_answer(config,
                                              "whatsapp",
                                              "WhatsApp",
                                              config.contacts.whatsapp));
    }

    if (max_messenger_intent(query)) {
        append_unique_answer(answers,
                             topics,
                             messenger_answer(config,
                                              "max",
                                              "MAX",
                                              config.contacts.max));
    }

    if (map_listing_intent(query, {"яндекс"})) {
        append_unique_answer(answers,
                             topics,
                             map_listing_answer(config,
                                                "yandex_maps",
                                                "на Яндекс Картах"));
    }

    if (map_listing_intent(query, {"2гис", "дубльгис"})) {
        append_unique_answer(answers,
                             topics,
                             map_listing_answer(config,
                                                "2gis",
                                                "в 2ГИС"));
    }

    if (map_listing_intent(query, {"google", "гугл"})) {
        append_unique_answer(answers,
                             topics,
                             map_listing_answer(config,
                                                "google_maps",
                                                "на Google Картах"));
    }

    if (website_intent(query)) {
        append_unique_answer(answers, topics, website_answer(config));
    }

    const auto &amenities = config.general_amenities;

    if (parking_intent(query)) {
        append_unique_answer(answers,
                             topics,
                             parking_answer(config, accessible_parking_intent(query), query));
    }

    if (wifi_intent(query)) {
        append_unique_answer(answers,
                             topics,
                             boolean_feature_answer(
                                     config,
                                     "wifi",
                                     amenities.has_wifi,
                                     "Wi-Fi",
                                     "Да, у нас есть Wi-Fi",
                                     "Wi-Fi для гостей не предусмотрен",
                                     amenities.has_wifi
                                             ? std::string_view{amenities.wifi_note}
                                             : std::string_view{}));
    }

    if (coffee_for_guests_intent(query)) {
        append_unique_answer(answers, topics, coffee_for_guests_answer(config));
    }

    if (contains_any(query, {"детский уголок", "детская зона", "детская комната"})) {
        append_unique_answer(answers, topics, child_zone_answer(config));
    }

    if (gift_certificate_intent(query)) {
        append_unique_answer(answers, topics, gift_certificate_answer(config));
    }

    if (staff_call_button_intent(query)) {
        append_unique_answer(answers,
                             topics,
                             staff_call_button_answer(config, query));
    }

    if (ramp_intent(query)) {
        append_unique_answer(answers, topics, ramp_answer(config, query));
    }

    if (pets_intent(query)) {
        append_unique_answer(answers, topics, pets_answer(config, query));
    }

    if (smoking_intent(query)) {
        append_unique_answer(answers, topics, smoking_answer(config));
    }

    const auto requested_directions = beauty_directions_from_query(query);
    const auto asks_warranty_photo = warranty_photo_intent(query);
    const auto asks_warranty_damage = warranty_damage_intent(query);
    const auto asks_warranty = warranty_intent(query);
    const auto has_warranty_request = asks_warranty_photo ||
                                      asks_warranty_damage ||
                                      asks_warranty;
    const auto asks_minors = !has_warranty_request && minors_intent(query);
    const auto asks_general_services = !payment_request.matched() &&
                                       !asks_minors && services_intent(query);
    const auto matched_any_service = find_matching_service(config, query, true);
    const auto has_specific_service =
            manicure_without_coating_intent(query) ||
            (matched_any_service != nullptr &&
             has_specific_exact_service_match(*matched_any_service, query));
    const auto asks_service_availability =
            !payment_request.matched() &&
            !schedule_request.matched() &&
            (service_availability_intent(query) ||
             (contains_word(query, "есть") &&
              (has_specific_service || !requested_directions.empty())));
    const auto asks_direction_services = !requested_directions.empty() &&
                                         direction_services_intent(query);
    const auto asks_direction_availability =
            !requested_directions.empty() && !asks_minors &&
            !has_warranty_request && !asks_general_services &&
            asks_service_availability && !has_specific_service;

    if (!has_specific_product && !booking_request.matched() &&
        beauty_directions_intent(query)) {
        append_unique_answer(answers, topics, beauty_directions_answer(config));
    }

    if (has_specific_product || booking_request.matched()) {
        // Product and booking requests have already been resolved above. Do
        // not let generic service/direction cues add unrelated sections.
    } else if (has_warranty_request) {
        if (asks_warranty_photo || asks_warranty_damage) {
            append_unique_answer(answers,
                                 topics,
                                 warranty_incident_answer(config,
                                                          query,
                                                          asks_warranty_photo));
        } else {
            append_unique_answer(answers, topics, warranty_answer(config, query));
        }
    } else if (asks_minors) {
        append_unique_answer(answers, topics, minors_answer(config, query));
    } else if (asks_direction_services || asks_direction_availability) {
        append_unique_answer(answers,
                             topics,
                             directions_services_answer(config,
                                                        requested_directions));
    } else if (asks_general_services) {
        append_unique_answer(answers, topics, services_answer(config));
    } else if (asks_service_availability) {
        append_unique_answer(
                answers,
                topics,
                specific_service_answer(config,
                                        query,
                                        explicit_unknown_service_intent(query)));
    }

    if (contains_any(query, {"какие товары", "продаете товары", "что можно купить", "категории товаров"})) {
        append_unique_answer(answers, topics, products_answer(config));
    }

    if (specific_product.has_value()) {
        append_unique_answer(answers, topics, specific_product);
    }

    if (config.business_type == organization_business_type_e::coffee_shop) {
        const auto &coffee = config.business_details.coffee_shop;

        if (contains_any(query, {"какое меню", "что есть в меню", "категории меню"})) {
            append_unique_answer(answers, topics, coffee_menu_answer(config));
        }

        if (contains_any(query, {"альтернативное молоко", "растительное молоко", "какое молоко"})) {
            append_unique_answer(answers, topics, alternative_milk_answer(config));
        }

        if (contains_any(query, {"кофе без кофеина", "есть декаф", "есть decaf"})) {
            append_unique_answer(answers,
                                 topics,
                                 boolean_feature_answer(config,
                                                        "decaf",
                                                        coffee.decaf_available,
                                                        "Кофе без кофеина",
                                                        "Да, у нас есть кофе без кофеина",
                                                        "Кофе без кофеина не предусмотрен",
                                                        {}));
        }

        if (contains_any(query, {"кофе с собой", "напиток с собой", "на вынос"})) {
            append_unique_answer(answers,
                                 topics,
                                 boolean_feature_answer(config,
                                                        "takeaway",
                                                        coffee.takeaway_available,
                                                        "Заказы навынос",
                                                        "Да, напитки можно взять с собой",
                                                        "Заказы навынос не предусмотрены",
                                                        {}));
        }

        if (contains_any(query, {"есть доставка", "доставка кофе", "доставка напитков"})) {
            append_unique_answer(answers,
                                 topics,
                                 boolean_feature_answer(config,
                                                        "delivery",
                                                        coffee.delivery_available,
                                                        "Доставка",
                                                        "Да, у нас доступна доставка",
                                                        "Доставка не предусмотрена",
                                                        {}));
        }
    }

    if (config.business_type == organization_business_type_e::gas_station) {
        const auto &station = config.business_details.gas_station;

        if (fuel_types_intent(query)) {
            append_unique_answer(answers, topics, fuel_types_answer(config));
        }

        if (contains_any(query,
                         {"залить в канистру", "заливать в канистру", "можно ли заливать в канистру",
                          "заправить канистру", "бензин в канистру"})) {
            auto note = station.accepted_canister_types.empty()
                                ? std::string{}
                                : std::format("Допустимые ёмкости: {}",
                                              join_human_readable(station.accepted_canister_types));
            append_unique_answer(answers,
                                 topics,
                                 boolean_feature_answer(config,
                                                        "canister",
                                                        station.canister_refueling_allowed,
                                                        "Заправка топлива в канистру",
                                                        "Да, топливо можно заправить в подходящую канистру",
                                                        "Заправка топлива в канистру не разрешена",
                                                        note));
        }

        if (contains_any(query, {"есть магазин", "магазин на азс"})) {
            append_unique_answer(answers,
                                 topics,
                                 boolean_feature_answer(config,
                                                        "shop",
                                                        station.has_shop,
                                                        "Магазин на АЗС",
                                                        "Да, на АЗС есть магазин",
                                                        "Магазина на АЗС нет",
                                                        {}));
        }

        if (contains_any(query, {"есть туалет", "туалет на азс"})) {
            append_unique_answer(answers,
                                 topics,
                                 boolean_feature_answer(config,
                                                        "toilet",
                                                        station.has_toilet,
                                                        "Туалет на АЗС",
                                                        "Да, на АЗС есть туалет",
                                                        "Туалета для посетителей нет",
                                                        {}));
        }
    }

    if (config.business_type == organization_business_type_e::gym) {
        const auto &gym = config.business_details.gym;

        if (contains_any(query, {"есть душ", "есть ли душ", "можно принять душ", "можно ли принять душ", "душевая"})) {
            append_unique_answer(answers,
                                 topics,
                                 boolean_feature_answer(config,
                                                        "shower",
                                                        gym.has_shower,
                                                        "Душ",
                                                        "Да, в спортзале есть душ",
                                                        "Душевой зоны нет",
                                                        {}));
        }

        if (contains_any(query,
                         {"выдаете полотенца", "выдают полотенца", "выдают ли полотенца",
                          "есть полотенца", "есть ли полотенца", "дают полотенце"})) {
            append_unique_answer(answers,
                                 topics,
                                 boolean_feature_answer(config,
                                                        "towels",
                                                        gym.towels_provided,
                                                        "Полотенца",
                                                        "Да, полотенца предоставляются",
                                                        "Полотенца нужно принести с собой",
                                                        {}));
        }

        if (contains_any(query, {"есть шкафчики", "есть локеры", "куда убрать вещи"})) {
            append_unique_answer(answers,
                                 topics,
                                 boolean_feature_answer(config,
                                                        "lockers",
                                                        gym.has_lockers,
                                                        "Шкафчики",
                                                        "Да, для вещей есть шкафчики",
                                                        "Отдельные шкафчики не предусмотрены",
                                                        {}));
        }

        if (contains_any(query, {"есть сауна", "сауна в спортзале"})) {
            append_unique_answer(answers,
                                 topics,
                                 boolean_feature_answer(config,
                                                        "sauna",
                                                        gym.has_sauna,
                                                        "Сауна",
                                                        "Да, в спортзале есть сауна",
                                                        "Сауны в спортзале нет",
                                                        {}));
        }

        if (contains_any(query, {"пробная тренировка", "первое пробное занятие"})) {
            append_unique_answer(answers,
                                 topics,
                                 boolean_feature_answer(config,
                                                        "trial_workout",
                                                        gym.trial_workout_available,
                                                        "Пробная тренировка",
                                                        "Да, доступна пробная тренировка",
                                                        "Пробная тренировка не предусмотрена",
                                                        {}));
        }

        if (contains_any(query, {"какие тренеры", "квалификация тренеров", "специализация тренеров"})) {
            append_unique_answer(answers, topics, coaching_answer(config));
        }
    }

    if (answers.empty()) {
        return std::nullopt;
    }

    std::ranges::stable_sort(answers,
                             [&](const auto &lhs, const auto &rhs) {
                                 return answer_topic_position(lhs.topic, query) <
                                        answer_topic_position(rhs.topic, query);
                             });

    auto result = organization_config_answer_s{};
    auto topics_list = std::vector<std::string>{};
    auto facts = std::vector<std::string>{};
    auto customer_parts = std::vector<std::string>{};

    topics_list.reserve(answers.size());
    facts.reserve(answers.size());
    customer_parts.reserve(answers.size());

    for (auto &answer : answers) {
        topics_list.push_back(answer.topic);
        facts.push_back(std::move(answer.fact_text));
        customer_parts.push_back(std::move(answer.customer_text));
    }

    result.topic = join_strings(topics_list, ",");
    result.fact_text = join_strings(facts, "\n\n");
    result.customer_text = join_customer_answers(customer_parts);

    if (const auto emoji_answer = std::ranges::find_if(
                answers,
                [](const organization_config_answer_s &answer) {
                    return !answer.emoji.empty();
                });
        emoji_answer != answers.end()) {
        result.emoji = emoji_answer->emoji;
    }

    return result;
}

} // namespace stz::intern
