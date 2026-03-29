cmake_minimum_required(VERSION 3.16)
project(nas_backend CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon REQUIRED)

add_executable(
    nas_backend
    main.cpp
    controllers/AuthController.cpp
    controllers/FilesystemController.cpp
    controllers/UploadController.cpp
    filters/JwtFilter.cpp
)

target_link_libraries(nas_backend PRIVATE Drogon::Drogon)
