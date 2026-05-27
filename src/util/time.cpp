#include "time.hpp"

#include <array>
#include <ctime>

#include "platform.hpp"

namespace stz::intern::util {

std::string make_local_timestamp() {
    const auto now = std::time(nullptr);

    auto tm = std::tm{};

#ifdef STZ_INTERN_PLATFORM_WINDOWS
    localtime_s(&tm, &now);
#else
    localtime_r(&tm, &now);
#endif

    auto buffer = std::array<char, 32>{};
    std::strftime(buffer.data(), buffer.size(), dmy_hms_format, &tm);

    return buffer.data();
}

} // namespace stz::intern::util