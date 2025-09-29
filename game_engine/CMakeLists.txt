cmake_minimum_required(VERSION 3.10)
project(AIGameContract)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)

# Add llama.cpp as a subdirectory
add_subdirectory(llama.cpp)

# Include directories
include_directories(llama.cpp/include)
include_directories(llama.cpp/vendor)
include_directories(include)
include_directories(${OPENSSL_INCLUDE_DIR})

# Add AI Contract with httplib (current optimized version)
add_executable(AIContract_httplib src/ai_contract_httplib.cpp)

# Link libraries for AI Contract httplib
target_link_libraries(AIContract_httplib PRIVATE 
    llama 
    ggml 
    OpenSSL::SSL 
    OpenSSL::Crypto
    pthread
    dl
    m
)

# Add AI Contract with Daemon Architecture (performance optimized)
add_executable(AIContract_daemon src/ai_contract_daemon.cpp)

# Link libraries for AI Contract daemon (no direct llama dependency)
target_link_libraries(AIContract_daemon PRIVATE 
    OpenSSL::SSL 
    OpenSSL::Crypto
    pthread
    dl
    m
)

# Add AI Daemon executable (new background service)
add_executable(AIDaemon src/ai_daemon.cpp)

# Link libraries for AI Daemon
target_link_libraries(AIDaemon PRIVATE 
    llama 
    ggml 
    OpenSSL::SSL 
    OpenSSL::Crypto
    pthread
    dl
    m
)

# Legacy Xahau NFT components removed - now using NFTMintingClient
# which delegates to Node.js signing service for better reliability
    OpenSSL::SSL 
    OpenSSL::Crypto
    pthread
    dl
    m
)

# Set compiler flags for AI Contract
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(AIContract_httplib PRIVATE -O3 -march=native)
    target_compile_options(AIContract_daemon PRIVATE -O3 -march=native)
    target_compile_options(AIDaemon PRIVATE -O3 -march=native)
    target_compile_options(XahauNFT PRIVATE -O3 -march=native)
    target_compile_options(NFTMinterTest PRIVATE -O3 -march=native)
endif()

# Copy shared libraries to output directory for easier deployment
add_custom_command(TARGET AIContract_daemon POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/bin ${CMAKE_BINARY_DIR}/ai_contract_libs
    COMMENT "Copying shared libraries for AI Contract deployment"
)
