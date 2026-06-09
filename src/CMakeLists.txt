add_subdirectory(util)
add_subdirectory(llm)

target_sources(
        llm_intern PRIVATE
        application.cpp
        application.hpp
        chat_history.cpp
        chat_history.hpp
        config.cpp
        config.hpp
        engine.cpp
        engine.hpp
        knowledge_storage.cpp
        knowledge_storage.hpp
        platform.hpp
        web_server.cpp
        web_server.hpp
        web_server_config.hpp
)