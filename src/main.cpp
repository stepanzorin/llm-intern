#include <stdexcept>

#include <spdlog/spdlog.h>

int main() try {
    return EXIT_SUCCESS;
} catch (const std::exception &ex) {
    spdlog::error(std::format("Unhandled exception caught: <{}>", ex.what()));
    return EXIT_FAILURE;
} catch (...) {
    spdlog::error("Unhandled exception caught: <Unknown error>");
    return EXIT_FAILURE;
}