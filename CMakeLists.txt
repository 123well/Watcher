cmake_minimum_required(VERSION 3.20)
project(Watcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# AUTOMOC/AUTORCC НЕ включаем глобально — иначе MOC лезет в чужие цели.
# Включаем per-target ниже.

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick Qml)
find_package(SQLite3 REQUIRED)              # системный sqlite3 с FTS5
find_package(nlohmann_json QUIET)           # header-only; заголовок и так на пути

# ── llama.cpp НЕ компилируем. Он нужен только как рантайм-бинарник llama-server, ──
# ── который LlamaServerEmbeddingProvider поднимает ребёнком на время батча. ──
# ── Поэтому здесь нет add_subdirectory, нет GGML_*/LLAMA_*, нет force-include. ──

# ── RAG-модуль ────────────────────────────────────────────────────────────────
add_library(watcher_rag STATIC
    src/rag/rag_util.cpp
    src/rag/embedding_provider.cpp
    src/rag/embedding_llamaserver.cpp
    src/rag/chunker.cpp
    src/rag/store.cpp
    src/rag/vector_index.cpp
    src/rag/searcher.cpp
    src/rag/rag_engine.cpp
    src/rag/rag_controller.cpp
)

set_target_properties(watcher_rag PROPERTIES AUTOMOC ON)   # RagEngine — QObject

target_include_directories(watcher_rag PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(watcher_rag PUBLIC
    Qt6::Core
    SQLite3::SQLite3
)
if(nlohmann_json_FOUND)
    target_link_libraries(watcher_rag PUBLIC nlohmann_json::nlohmann_json)
endif()

add_executable(rag-cli src/rag_cli.cpp)
target_link_libraries(rag-cli PRIVATE watcher_rag)

# ── основной watcher ──────────────────────────────────────────────────────────
qt6_add_resources(QML_RESOURCES src/ui/qml.qrc)

add_executable(watcher
    src/ui/main_qt.cpp
    src/core/chat_engine.cpp src/core/chat_engine.h
    src/model/flm_gateway.cpp src/model/flm_gateway.h
    src/input/global_input.cpp src/input/global_input.h
    ${QML_RESOURCES}
)

set_target_properties(watcher PROPERTIES AUTOMOC ON AUTORCC ON)

target_include_directories(watcher PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(watcher PRIVATE
    Threads::Threads
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml
    watcher_rag
)

install(TARGETS watcher rag-cli RUNTIME DESTINATION bin)
