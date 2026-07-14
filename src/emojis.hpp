// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <string>
#include <string_view>

namespace stz::intern {

enum class texting_style_e;

enum class emoji_kind_e {
    name,
    address,
    schedule,
    payment,
    cash,
    card,
    email,
    booking,
    phone,
    website,
    parking,
    wifi,
    coffee,
    child_zone,
    pets,
    smoking,
    services,
    products,
    warranty,
    menu,
    alternative_milk,
    decaf,
    takeaway,
    delivery,
    fuel,
    canister,
    shop,
    toilet,
    shower,
    lockers,
    sauna,
    trial_workout,
    coaching,
    telegram,
    whatsapp,
    max,
    maps,
    service_directions,
    service_direction,
    minors,
    gift_certificate,
    staff_call_button,
    ramp,
    accessible_parking,
    smile,
    tada,
    confetti_ball,
    birthday,
    disappointment,
};

[[nodiscard]] std::string_view put_emoji(emoji_kind_e kind) noexcept;
[[nodiscard]] std::string_view put_emoji(emoji_kind_e kind, texting_style_e style) noexcept;

[[nodiscard]] std::string push_front_emoji(const std::string &text, emoji_kind_e kind);
[[nodiscard]] std::string push_front_emoji(const std::string &text,
                                           emoji_kind_e kind,
                                           texting_style_e style);

[[nodiscard]] std::string push_back_emoji(const std::string &text, emoji_kind_e kind);
[[nodiscard]] std::string push_back_emoji(const std::string &text,
                                          emoji_kind_e kind,
                                          texting_style_e style);

} // namespace stz::intern
