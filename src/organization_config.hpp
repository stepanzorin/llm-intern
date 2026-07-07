// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace stz::intern {

enum class organization_business_type_e {
    unknown,
    beauty_salon,
    coffee_shop,
    gas_station,
    gym,
};

[[nodiscard]] std::string_view to_string(organization_business_type_e type) noexcept;

[[nodiscard]] organization_business_type_e organization_business_type_from_string(
        std::string_view value) noexcept;

struct organization_address_s {
    std::string formatted = {};
    std::string directions = {};
    std::string landmark = {};
    std::string entrance = {};
};

struct organization_contacts_s {
    organization_address_s address = {};

    std::string phone = {};
    std::string website = {};
    std::string telegram = {};
    std::string whatsapp = {};
    std::string max = {};
};

struct organization_schedule_rule_s {
    std::string label = {};
    std::vector<std::string> days = {};
    std::string opens = {};
    std::string closes = {};
};

struct organization_schedule_exception_s {
    std::string policy = {};
    std::string note = {};
};

struct organization_schedule_s {
    std::string timezone = {};
    std::vector<organization_schedule_rule_s> regular = {};
    organization_schedule_exception_s holidays = {};
    organization_schedule_exception_s new_year = {};
};

struct organization_booking_method_s {
    std::string id = {};
    bool enabled = false;
    std::string label = {};
    std::string value = {};
    std::string instructions = {};
};

struct organization_payment_methods_s {
    bool cash = false;
    bool card = false;
    bool credit_card = false;
    bool qr_code = false;
    bool sbp = false;
    bool online = false;
    bool on_site = true;
    bool prepayment_required = false;
    std::string note = {};
};

struct organization_parking_s {
    bool regular = false;
    bool accessible = false;

    std::string regular_note = {};
    std::string accessible_note = {};
};

struct organization_pets_policy_s {
    bool allowed = false;
    bool small_dogs_allowed = false;
    std::optional<std::size_t> max_dog_height_cm = std::nullopt;
    bool service_animals_allowed = false;

    std::string note = {};
};

struct organization_general_amenities_s {
    bool has_wifi = false;
    bool has_free_coffee = false;
    bool has_child_zone = false;
    bool serves_minors = false;
    bool minor_parent_presence_required = false;
    bool has_gift_certificates = false;
    bool has_staff_call_button = false;
    bool has_ramp = false;
    bool smoking_allowed = false;

    organization_parking_s parking = {};
    organization_pets_policy_s pets = {};

    std::string wifi_note = {};
    std::string coffee_note = {};
    std::string child_zone_note = {};
    std::string minors_note = {};
    std::string gift_certificates_note = {};
    std::string staff_call_button_note = {};
    std::string ramp_note = {};
    std::string smoking_note = {};
};

struct organization_service_minor_access_s {
    bool allowed = false;
    std::optional<std::size_t> min_age = std::nullopt;
};

struct organization_service_s {
    std::string name = {};
    std::vector<std::string> aliases = {};
    std::vector<std::string> directions = {};
    organization_service_minor_access_s minor_access = {};
    bool warranty_case = false;
    std::string warranty_note = {};
};

struct beauty_salon_service_directions_s {
    bool manicure = false;
    bool pedicure = false;
    bool brows = false;
    bool eyelashes = false;
    bool hairdressing = false;
    bool cosmetology = false;
    bool hair_removal = false;
    bool makeup = false;
    bool massage = false;
    bool podology = false;
};

struct beauty_salon_config_s {
    beauty_salon_service_directions_s service_directions = {};
    std::vector<organization_service_s> services = {};
    std::vector<std::string> product_categories = {};
};

struct coffee_shop_config_s {
    std::vector<std::string> menu_categories = {};
    std::vector<std::string> alternative_milk_types = {};

    bool takeaway_available = false;
    bool delivery_available = false;
    bool decaf_available = false;
    bool reusable_cup_allowed = false;
};

struct gas_station_fuel_s {
    std::string name = {};
    std::vector<std::string> aliases = {};
};

struct gas_station_config_s {
    std::vector<gas_station_fuel_s> fuel_types = {};
    bool canister_refueling_allowed = false;
    std::vector<std::string> accepted_canister_types = {};

    bool has_shop = false;
    bool has_toilet = false;
    bool has_tire_inflation = false;
};

struct gym_coaching_option_s {
    std::string qualification = {};
    std::string specialization = {};
};

struct gym_config_s {
    bool has_shower = false;
    bool towels_provided = false;
    bool has_lockers = false;
    bool has_sauna = false;
    bool trial_workout_available = false;

    std::vector<gym_coaching_option_s> coaching_options = {};
};

struct organization_business_details_s {
    beauty_salon_config_s beauty_salon = {};
    coffee_shop_config_s coffee_shop = {};
    gas_station_config_s gas_station = {};
    gym_config_s gym = {};
};

struct organization_config_s {
    std::size_t schema_version = 3;
    bool enabled = true;

    organization_business_type_e business_type = organization_business_type_e::unknown;
    std::string brand_name = {};

    organization_contacts_s contacts = {};
    organization_schedule_s schedule = {};
    std::vector<organization_booking_method_s> booking_methods = {};
    organization_payment_methods_s payment_methods = {};
    organization_general_amenities_s general_amenities = {};
    organization_business_details_s business_details = {};

    std::unordered_map<std::string, std::string> emojis = {};

    std::filesystem::path source_file = {};
};

struct organization_config_answer_s {
    std::string topic = {};
    std::string fact_text = {};
    std::string customer_text = {};
    std::string emoji = {};
};

[[nodiscard]] organization_config_s load_organization_config(
        const std::filesystem::path &filename);

/*
 * Returns a direct answer only for high-confidence requests about static
 * organization data. An empty result means that the normal workflow/texting
 * pipeline must continue unchanged.
 */
[[nodiscard]] std::optional<organization_config_answer_s> answer_from_organization_config(
        const organization_config_s &config,
        std::string_view user_text);

} // namespace stz::intern
