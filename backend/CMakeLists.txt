cmake_minimum_required(VERSION 3.16)
project(nas_backend CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon REQUIRED)

# SQLite3 is bundled and resolved internally by Drogon — do NOT call
# find_package(SQLite3) here; it creates a duplicate SQLite3_lib imported
# target and CMake errors out.  Drogon exposes sqlite3 symbols transitively,
# so linking Drogon::Drogon is sufficient.
# (services/EventWriter.cpp uses the raw sqlite3 C API, no Drogon ORM.)

add_executable(
    nas_backend
    main.cpp
    controllers/AuthController.cpp
    controllers/FilesystemController.cpp
    controllers/UploadController.cpp
    controllers/EventsController.cpp
    controllers/AlertsController.cpp
    filters/JwtFilter.cpp
    services/EventRecorder.cpp
    services/EventWriter.cpp
    services/AlertWriter.cpp
    services/EventAnalyzer.cpp
    controllers/SyncController.cpp
    services/SyncManager.cpp
)

# OpenSSL is linked explicitly, unlike SQLite3 above. Drogon pulls it in for
# TLS, but services/SyncManager.cpp calls EVP_* directly for the SHA-256
# fallback in needsCopy(), and relying on a transitive dependency for symbols
# we use ourselves breaks against a Drogon built without TLS.
find_package(OpenSSL REQUIRED)

target_link_libraries(nas_backend PRIVATE Drogon::Drogon OpenSSL::Crypto)
