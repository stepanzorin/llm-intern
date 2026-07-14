// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#include "emojis.hpp"

#include <map>

#include "assistant_profile.hpp"
#include "knowledge_storage.hpp"

namespace stz::intern {

namespace {

const auto emojis = std::map<emoji_kind_e, std::string_view>{
        {emoji_kind_e::name, "🏢"},
        {emoji_kind_e::address, "📍"},
        {emoji_kind_e::schedule, "🕒"},
        {emoji_kind_e::payment, "💰"},
        {emoji_kind_e::cash, "💵"},
        {emoji_kind_e::card, "💳"},
        {emoji_kind_e::email, "✉️"},
        {emoji_kind_e::booking, "🗓️"},
        {emoji_kind_e::phone, "☎️"},
        {emoji_kind_e::website, "🌐"},
        {emoji_kind_e::parking, "🅿️"},
        {emoji_kind_e::wifi, "📶"},
        {emoji_kind_e::coffee, "☕"},
        {emoji_kind_e::child_zone, "🧸"},
        {emoji_kind_e::pets, "🐾"},
        {emoji_kind_e::smoking, "🚭"},
        {emoji_kind_e::services, "💅"},
        {emoji_kind_e::products, "🛍️"},
        {emoji_kind_e::warranty, "🛡️"},
        {emoji_kind_e::menu, "☕"},
        {emoji_kind_e::alternative_milk, "🥛"},
        {emoji_kind_e::decaf, "☕"},
        {emoji_kind_e::takeaway, "🥤"},
        {emoji_kind_e::delivery, "🚚"},
        {emoji_kind_e::fuel, "⛽"},
        {emoji_kind_e::canister, "🛢️"},
        {emoji_kind_e::shop, "🛒"},
        {emoji_kind_e::toilet, "🚻"},
        {emoji_kind_e::shower, "🚿"},
        {emoji_kind_e::lockers, "🔐"},
        {emoji_kind_e::sauna, "♨️"},
        {emoji_kind_e::trial_workout, "🏋️"},
        {emoji_kind_e::coaching, "🏋️"},
        {emoji_kind_e::telegram, "💬"},
        {emoji_kind_e::whatsapp, "💬"},
        {emoji_kind_e::max, "💬"},
        {emoji_kind_e::maps, "🗺️"},
        {emoji_kind_e::service_directions, "💅"},
        {emoji_kind_e::service_direction, "💅"},
        {emoji_kind_e::minors, "👶"},
        {emoji_kind_e::gift_certificate, "🎁"},
        {emoji_kind_e::staff_call_button, "🔔"},
        {emoji_kind_e::ramp, "♿"},
        {emoji_kind_e::accessible_parking, "♿"},
        {emoji_kind_e::smile, "😊"},
        {emoji_kind_e::tada, "🎉"},
        {emoji_kind_e::confetti_ball, "🎊"},
        {emoji_kind_e::birthday, "🎂"},
        {emoji_kind_e::disappointment, "😞"},
};

} // namespace

std::string_view put_emoji(const emoji_kind_e kind) noexcept {
    const auto it = emojis.find(kind);
    return it == emojis.end() ? std::string_view{} : it->second;
}

std::string_view put_emoji(const emoji_kind_e kind, const texting_style_e style) noexcept {
    return style == texting_style_e::friendly ? put_emoji(kind) : std::string_view{};
}

std::string push_front_emoji(const std::string &text, const emoji_kind_e kind) {
    const auto emoji = put_emoji(kind);

    if (emoji.empty() || text.empty()) {
        return text;
    }

    return std::string{emoji} + " " + text;
}

std::string push_front_emoji(const std::string &text,
                             const emoji_kind_e kind,
                             const texting_style_e style) {
    const auto emoji = put_emoji(kind, style);

    if (emoji.empty() || text.empty()) {
        return text;
    }

    return std::string{emoji} + " " + text;
}

std::string push_back_emoji(const std::string &text, const emoji_kind_e kind) {
    const auto emoji = put_emoji(kind);

    if (emoji.empty() || text.empty()) {
        return text;
    }

    return text + " " + std::string{emoji};
}

std::string push_back_emoji(const std::string &text,
                            const emoji_kind_e kind,
                            const texting_style_e style) {
    const auto emoji = put_emoji(kind, style);

    if (emoji.empty() || text.empty()) {
        return text;
    }

    return text + " " + std::string{emoji};
}

} // namespace stz::intern
