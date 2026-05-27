add_subdirectory(util)

target_sources(
        llm_intern PRIVATE
        llm/llama_client.cpp
        llm/llama_client.hpp
)

target_sources(
        llm_intern PRIVATE
        chat_history.cpp
        chat_history.hpp
        config.cpp
        config.hpp
        engine.cpp
        engine.hpp
        knowledge_storage.cpp
        knowledge_storage.hpp
        platform.hpp
)