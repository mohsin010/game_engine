#!/bin/bash
# Unified Build System for HotPocket AI Contract
# This script builds the complete integrated system: Game Engine + AI Jury + All Daemons

set -e

# Project paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"
BUILD_DIR="$PROJECT_ROOT/build"
SRC_DIR="$PROJECT_ROOT/src"
GAME_ENGINE_DIR="$SRC_DIR/game_engine"
DEPLOYMENT_DIR="$PROJECT_ROOT/hotpocket_deployment"

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

print_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# Clean and setup build directory
clean_build() {
    print_info "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR/integrated"
    mkdir -p "$DEPLOYMENT_DIR"
    cd "$BUILD_DIR"
}

# Build AI Jury System (AI Jury Daemon)
build_jury_system() {
    print_info "Building AI Jury System..."
    
    # Ensure we're in the correct directory and the integrated directory exists
    cd "$PROJECT_ROOT"
    mkdir -p "$BUILD_DIR/integrated"
    cd "$BUILD_DIR/integrated"
    
    # Create jury subdirectory within integrated to keep sources organized
    mkdir -p jury_build
    cd jury_build
    
    # Copy AI Jury source files (from project root - go back three levels from build/integrated/jury_build)
    cp "../../../src/ai_jury_daemon.cpp" ai_jury_daemon.cpp
    cp "../../../src/ai_jury_module.cpp" .
    cp "../../../src/ai_jury_module.h" .
    
    # Create CMakeLists.txt for AI Jury system
    cat > CMakeLists.txt << 'EOF'
cmake_minimum_required(VERSION 3.12)
project(AIJurySystem)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Force static linking for Docker compatibility
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_EXE_LINKER_FLAGS "-static")

# Compatibility flags for older GLIBC systems
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GLIBCXX_USE_CXX11_ABI=0")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GLIBCXX_USE_CXX11_ABI=0")

# Define CPPHTTPLIB_OPENSSL_SUPPORT for ModelDownloader
add_definitions(-DCPPHTTPLIB_OPENSSL_SUPPORT)

# Find OpenSSL for ModelDownloader functionality
find_package(OpenSSL REQUIRED)

# Include nlohmann/json from llama.cpp vendor
include_directories(../../../llama.cpp/vendor)

# Build llama.cpp statically  
set(LLAMA_STATIC ON CACHE BOOL "Build static libraries" FORCE)
set(LLAMA_SHARED OFF CACHE BOOL "Build shared libraries" FORCE)
add_subdirectory(../../../llama.cpp llama_build)

# Include directories
include_directories(../../../llama.cpp/include)
include_directories(../../../llama.cpp/common)
include_directories(../../../src)
include_directories(../../../include)
include_directories(../../../httplib)

# AI Jury Daemon executable (model inference service on port 8766)
add_executable(ai_jury_daemon ai_jury_daemon.cpp)
target_link_libraries(ai_jury_daemon PRIVATE 
    llama
    ggml
    ggml-base
    ggml-cpu
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
    dl
    m
)

# Ensure fully static linking
set_target_properties(ai_jury_daemon PROPERTIES
    LINK_SEARCH_START_STATIC 1
    LINK_SEARCH_END_STATIC 1
)

message(STATUS "Building AI Jury System with llama.cpp and OpenSSL")
EOF

    # Build jury system
    print_info "Configuring AI Jury build..."
    if ! cmake -DCMAKE_BUILD_TYPE=Release .; then
        print_error "AI Jury CMake configuration failed"
        return 1
    fi
    
    print_info "Building AI Jury system..."
    if ! make -j$(nproc); then
        print_error "AI Jury build failed"
        return 1
    fi
    
    # Verify and deploy jury binaries
    if [ -f "ai_jury_daemon" ]; then
        cp ai_jury_daemon "$DEPLOYMENT_DIR/"
        print_success "AI Jury system built and deployed successfully"
        return 0
    else
        print_error "AI Jury system build failed - binary not created"
        return 1
    fi
}

# Build Game Engine with AI Jury Integration
build_game_engine() {
    print_info "Building Game Engine with AI Jury Integration..."
    
    # Ensure we're in the correct directory and the integrated directory exists
    cd "$PROJECT_ROOT"
    mkdir -p "$BUILD_DIR/integrated"
    cd "$BUILD_DIR/integrated"
    
    # Create engine subdirectory within integrated to keep sources organized
    mkdir -p engine_build
    cd engine_build
    
    # Copy game engine source files (from project root - go back three levels from build/integrated/engine_build)
    cp "../../../src/game_engine/ai_contract.cpp" .
    cp "../../../src/game_engine/ai_daemon.cpp" .
    cp "../../../src/game_engine/ai_service_client.h" .
    
    # Copy new NFT minting client (replaces legacy XahauNFTMinter)
    cp "../../../src/nft_minting_client.cpp" .
    cp "../../../src/nft_minting_client.h" .
    
    # Copy AI Jury module for integration
    cp "../../../src/ai_jury_module.cpp" .
    cp "../../../src/ai_jury_module.h" .
    
    # Create CMakeLists.txt for integrated game engine
    cat > CMakeLists.txt << 'EOF'
cmake_minimum_required(VERSION 3.12)
project(AIGameEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Force static linking for Docker compatibility
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_EXE_LINKER_FLAGS "-static")

# Compatibility flags for older GLIBC systems
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GLIBCXX_USE_CXX11_ABI=0")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GLIBCXX_USE_CXX11_ABI=0")

# Define CPPHTTPLIB_OPENSSL_SUPPORT for ModelDownloader
add_definitions(-DCPPHTTPLIB_OPENSSL_SUPPORT)

# Find OpenSSL for SHA256 and HTTPS
set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)

# Include nlohmann/json from llama.cpp vendor
include_directories(../../../llama.cpp/vendor)

# Build llama.cpp statically  
set(LLAMA_STATIC ON CACHE BOOL "Build static libraries" FORCE)
set(LLAMA_SHARED OFF CACHE BOOL "Build shared libraries" FORCE)
add_subdirectory(../../../llama.cpp llama_build)

# Include directories
include_directories(../../../llama.cpp/include)
include_directories(../../../llama.cpp/common)
include_directories(../../../src)
include_directories(../../../include)
include_directories(../../../httplib)

# AI Game Daemon executable (background service for game engine)
add_executable(AIDaemon ai_daemon.cpp)
target_link_libraries(AIDaemon PRIVATE 
    llama
    ggml
    ggml-base
    ggml-cpu
    pthread
    dl
    m
)

# AI Game Contract executable (HotPocket contract with AI Jury integration)
add_executable(AIContract_daemon ai_contract.cpp ai_jury_module.cpp nft_minting_client.cpp)
target_link_libraries(AIContract_daemon PRIVATE 
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
    dl
    m
)

# Ensure fully static linking
set_target_properties(AIDaemon PROPERTIES
    LINK_SEARCH_START_STATIC 1
    LINK_SEARCH_END_STATIC 1
)

set_target_properties(AIContract_daemon PROPERTIES
    LINK_SEARCH_START_STATIC 1
    LINK_SEARCH_END_STATIC 1
)

message(STATUS "Building AI Game Engine with AI Jury Integration")
EOF

    # Build game engine
    print_info "Configuring Game Engine build..."
    if ! cmake -DCMAKE_BUILD_TYPE=Release .; then
        print_error "Game Engine CMake configuration failed"
        return 1
    fi
    
    print_info "Building Game Engine..."
    if ! make -j$(nproc); then
        print_error "Game Engine build failed"
        return 1
    fi
    
    # Verify and deploy game engine binaries
    if [ -f "AIDaemon" ] && [ -f "AIContract_daemon" ]; then
        cp AIDaemon "$DEPLOYMENT_DIR/"
        cp AIContract_daemon "$DEPLOYMENT_DIR/"
        print_success "Game Engine built and deployed successfully"
        return 0
    else
        print_error "Game Engine build failed - binaries not created"
        return 1
    fi
}



# Deploy model files and update deployment scripts
deploy_model_files() {
    print_info "Deploying model files..."
    
    # Deploy all available model files from project root
    local deployed_count=0
    local total_models=0
    
    # Create deployment model directory
    mkdir -p "$DEPLOYMENT_DIR/model"
    
    # Check for models in project root
    for model_file in "$PROJECT_ROOT"/*.gguf; do
        if [ -f "$model_file" ]; then
            total_models=$((total_models + 1))
            local model_name=$(basename "$model_file")
            local target_path="$DEPLOYMENT_DIR/model/$model_name"
            
            if [ ! -f "$target_path" ]; then
                print_info "Deploying model: $model_name"
                if cp "$model_file" "$target_path"; then
                    deployed_count=$((deployed_count + 1))
                    print_success "✓ $model_name deployed to deployment directory"
                else
                    print_warning "✗ Failed to deploy $model_name"
                fi
            else
                print_info "✓ $model_name already exists in deployment directory"
            fi
        fi
    done
    
    # Report deployment results
    if [ $total_models -eq 0 ]; then
        print_warning "No .gguf model files found in project root"
    else
        print_info "Model deployment summary:"
        print_info "  • Total models found: $total_models"
        print_info "  • Models deployed: $deployed_count"
        print_info "  • Models already present: $((total_models - deployed_count))"
    fi
    
    # Copy models to bin directory for easy access
    mkdir -p "$PROJECT_ROOT/bin"
    for model_file in "$PROJECT_ROOT"/*.gguf; do
        if [ -f "$model_file" ]; then
            local model_name=$(basename "$model_file")
            local bin_target="$PROJECT_ROOT/bin/$model_name"
            
            if [ ! -f "$bin_target" ]; then
                cp "$model_file" "$bin_target"
                print_success "Model copied to bin directory: $model_name"
            fi
        fi
    done
    
    # Deploy Xahau Signing Service (Binary Version)
    print_info "Deploying Xahau Signing Service..."
    
    local SOURCE_XAHAU_DIR="$PROJECT_ROOT/xahau_signer/dist"
    local TARGET_XAHAU_DIR="$DEPLOYMENT_DIR/xahau_signer"
    
    if [ -d "$SOURCE_XAHAU_DIR" ]; then
        # Always update the deployment directory with latest version
        if [ -d "$TARGET_XAHAU_DIR" ]; then
            print_info "Updating existing Xahau Signing Service in deployment..."
            rm -rf "$TARGET_XAHAU_DIR"
        fi
        
        # Copy entire directory
        if cp -r "$SOURCE_XAHAU_DIR" "$TARGET_XAHAU_DIR"; then
            print_success "✓ Xahau Signing Service deployed successfully"
            
            # Check for different signing service formats
            local BINARY_PATH="$TARGET_XAHAU_DIR/xahau-transaction-signer"
            local NCC_BUNDLE_PATH="$TARGET_XAHAU_DIR/index.js"
            
            if [ -f "$NCC_BUNDLE_PATH" ]; then
                print_success "✓ NCC bundle deployed: index.js (Node.js 20 compatible)"
            elif [ -f "$BINARY_PATH" ]; then
                chmod +x "$BINARY_PATH"
                print_success "✓ Legacy binary deployed: xahau-transaction-signer"
            else
                print_warning "⚠ Warning: No signing service binary or bundle found"
            fi
            
            # List deployed files for verification
            print_info "Deployed Xahau Signing Service components:"
            if [ -f "$TARGET_XAHAU_DIR/xahau-transaction-signer" ]; then
                print_info "  ✓ xahau-transaction-signer (binary)"
            fi
            if [ -f "$TARGET_XAHAU_DIR/.env" ]; then
                print_info "  ✓ .env (configuration)"
            fi
            if [ -d "$TARGET_XAHAU_DIR/assets" ]; then
                print_info "  ✓ assets/ (NFT metadata and images)"
            fi
        else
            print_warning "✗ Failed to deploy Xahau Signing Service"
        fi
    else
        print_warning "⚠ Xahau Signing Service directory not found: $SOURCE_XAHAU_DIR"
        print_info "  → Run setup_environment.sh to configure the signing service"
    fi
}

# Update deployment script with integrated system support
update_deployment_script() {
    print_info "Updating deployment script for integrated system..."
    
    cat > "$DEPLOYMENT_DIR/run_contract.sh" << 'EOF'
#!/bin/bash
# HotPocket Run Script for Integrated AI Game Engine with AI Jury System
# Handles persistent file management and multi-daemon architecture
#
# HotPocket Round Protection System:
# 
# Xahau Signing Service Protection (Binary Version):
# - Uses environment flag (xahau_signer.started) to prevent duplicate starts
# - Triple protection: flag + PID file + process validation
# - Runs compiled binary (xahau-transaction-signer) instead of Node.js code
# - Survives HotPocket rounds but allows container restarts
# - To manually reset: rm ../../../xahau_signer.started ../../../xahau_signer.pid
#
# Persistent Files Setup Protection:
# - Uses environment flag (persistent_files.setup) to prevent duplicate file operations
# - Skips model/binary copying if already completed
# - Validates critical directories before skipping
# - To manually reset: rm ../../../persistent_files.setup

# Function to setup persistent files for both daemons
setup_persistent_files() {
    echo "=== Setting up persistent files for integrated system ==="
    
    local PERSISTENT_SETUP_FLAG="../../../persistent_files.setup"
    
    # Check if persistent files have already been setup (prevents HotPocket round duplicates)
    if [ -f "$PERSISTENT_SETUP_FLAG" ]; then
        echo "✓ Persistent files setup flag detected"
        
        # Quick validation - check if critical directories exist
        if [ -d "../../../model" ] && [ -d "../../../" ]; then
            echo "✓ Persistent directories confirmed - skipping setup"
            return 0
        else
            echo "⚠ Warning: Setup flag exists but directories missing"
            echo "  → Cleaning up flag and re-running setup..."
            rm -f "$PERSISTENT_SETUP_FLAG"
        fi
    fi
    
    echo "→ Running first-time persistent files setup..."
    
    # Create persistent directories
    mkdir -p "../../../model"
    mkdir -p "../../../"
    
    # Setup models dynamically - copy all available .gguf files
    echo "Scanning for available model files..."
    local model_count=0
    local copied_count=0
    local failed_count=0
    
    # Check if model directory exists
    if [ -d "./model" ]; then
        # Loop through all .gguf files in the model directory
        for source_model in ./model/*.gguf; do
            # Check if the glob matched any files
            if [ -f "$source_model" ]; then
                model_count=$((model_count + 1))
                
                # Extract filename from path
                local model_filename=$(basename "$source_model")
                local target_model="../../../model/$model_filename"
                
                echo "Found model: $model_filename"
                
                if [ ! -f "$target_model" ]; then
                    echo "  → Moving $model_filename to persistent directory..."
                    if mv "$source_model" "$target_model"; then
                        echo "  ✓ $model_filename moved successfully"
                        copied_count=$((copied_count + 1))
                    else
                        echo "  ✗ Failed to move $model_filename"
                        failed_count=$((failed_count + 1))
                    fi
                else
                    echo "  ✓ $model_filename already exists in persistent directory"
                fi
            fi
        done
        
        # Report results
        echo "Model deployment summary:"
        echo "  • Total models found: $model_count"
        echo "  • Models copied: $copied_count"
        echo "  • Models already present: $((model_count - copied_count - failed_count))"
        if [ $failed_count -gt 0 ]; then
            echo "  • Failed copies: $failed_count"
        fi
        
        if [ $model_count -eq 0 ]; then
            echo "⚠ Warning: No .gguf model files found in ./model/ directory"
        elif [ $failed_count -gt 0 ]; then
            echo "⚠ Warning: Some model files failed to copy"
            return 1
        else
            echo "✓ All available models are now in persistent directory"
        fi
    else
        echo "⚠ Warning: ./model/ directory not found"
    fi
    
    # Setup AI Game Daemon binary
    local SOURCE_DAEMON="./AIDaemon"
    local TARGET_DAEMON="../../../AIDaemon"
    
    if [ -f "$SOURCE_DAEMON" ]; then
        if [ ! -f "$TARGET_DAEMON" ]; then
            echo "Copying AI Game Daemon to persistent directory..."
            if mv "$SOURCE_DAEMON" "$TARGET_DAEMON"; then
                chmod +x "$TARGET_DAEMON"
                echo "✓ AI Game Daemon binary moved successfully"
            else
                echo "✗ Failed to move AI Game Daemon binary"
                return 1
            fi
        else
            echo "✓ AI Game Daemon binary already exists in persistent directory"
        fi
    else
        echo "⚠ Warning: AI Game Daemon binary not found: $SOURCE_DAEMON"
    fi
    
    # Setup AI Jury Daemon binary
    local SOURCE_JURY_DAEMON="./ai_jury_daemon"
    local TARGET_JURY_DAEMON="../../../ai_jury_daemon"
    
    if [ -f "$SOURCE_JURY_DAEMON" ]; then
        if [ ! -f "$TARGET_JURY_DAEMON" ]; then
            echo "Copying AI Jury Daemon to persistent directory..."
            if mv "$SOURCE_JURY_DAEMON" "$TARGET_JURY_DAEMON"; then
                chmod +x "$TARGET_JURY_DAEMON"
                echo "✓ AI Jury Daemon binary moved successfully"
            else
                echo "✗ Failed to move AI Jury Daemon binary"
                return 1
            fi
        else
            echo "✓ AI Jury Daemon binary already exists in persistent directory"
        fi
    else
        echo "⚠ Warning: AI Jury Daemon binary not found: $SOURCE_JURY_DAEMON"
    fi
    
    # Setup Xahau Signing Service (Binary Version)
    local SOURCE_SIGNER_DIR="./xahau_signer"
    local TARGET_SIGNER_DIR="../../../xahau_signer"
    
    if [ -d "$SOURCE_SIGNER_DIR" ]; then
        if [ ! -d "$TARGET_SIGNER_DIR" ]; then
            echo "Copying Xahau Signing Service to persistent directory..."
            if cp -r "$SOURCE_SIGNER_DIR" "$TARGET_SIGNER_DIR"; then
                echo "✓ Xahau Signing Service copied successfully"
            else
                echo "✗ Failed to copy Xahau Signing Service"
                return 1
            fi
        else
            echo "✓ Xahau Signing Service already exists in persistent directory"
            # Update files if source is newer (including binary and config files)
            echo "  → Updating Xahau Signing Service files..."
            if rsync -av --update "$SOURCE_SIGNER_DIR/" "$TARGET_SIGNER_DIR/"; then
                echo "  ✓ Xahau Signing Service files updated"
            else
                echo "  ⚠ Warning: Failed to update some Xahau Signing Service files"
            fi
        fi
        
        # Ensure binary has executable permissions in persistent directory
        local PERSISTENT_BINARY="$TARGET_SIGNER_DIR/xahau-transaction-signer"
        if [ -f "$PERSISTENT_BINARY" ]; then
            chmod +x "$PERSISTENT_BINARY"
            echo "✓ Xahau Signing Service binary permissions set"
        else
            echo "⚠ Warning: Binary not found in persistent directory: $PERSISTENT_BINARY"
        fi
    else
        echo "⚠ Warning: Xahau Signing Service directory not found: $SOURCE_SIGNER_DIR"
    fi
    
    # Set persistent files setup flag to prevent re-execution in future HotPocket rounds
    echo "$(date '+%Y-%m-%d %H:%M:%S') - Persistent files setup completed successfully" > "$PERSISTENT_SETUP_FLAG"
    echo "✓ Persistent files setup flag created for future HotPocket rounds"
    
    echo "Persistent file setup completed"
    return 0
}

# Function to setup and start Xahau signing service (binary version)
setup_xahau_signing_service() {
    echo "=== Setting up Xahau Signing Service (Binary Version) ==="
    
    local SIGNER_DIR="../../../xahau_signer"
    local SIGNER_PID_FILE="../../../xahau_signer.pid"
    local SIGNER_ENV_FLAG="../../../xahau_signer.started"
    
    if [ ! -d "$SIGNER_DIR" ]; then
        echo "✗ Xahau Signing Service directory not found: $SIGNER_DIR"
        return 1
    fi
    
    # Triple protection: Environment flag + PID file + Process check
    # Check if service startup flag exists (prevents HotPocket round duplicates)
    if [ -f "$SIGNER_ENV_FLAG" ]; then
        echo "✓ Xahau Signing Service startup flag detected"
        
        # Verify service is actually running
        if [ -f "$SIGNER_PID_FILE" ]; then
            local existing_pid=$(cat "$SIGNER_PID_FILE" 2>/dev/null)
            if [ -n "$existing_pid" ] && kill -0 "$existing_pid" 2>/dev/null; then
                echo "✓ Xahau Signing Service confirmed running (PID: $existing_pid)"
                return 0
            else
                echo "⚠ Warning: Service flag exists but process not running"
                echo "  → Cleaning up stale files and restarting..."
                rm -f "$SIGNER_PID_FILE" "$SIGNER_ENV_FLAG"
            fi
        else
            echo "⚠ Warning: Service flag exists but no PID file found"
            echo "  → Cleaning up and restarting..."
            rm -f "$SIGNER_ENV_FLAG"
        fi
    fi
    
    # Check if service is already running (without flag)
    if [ -f "$SIGNER_PID_FILE" ]; then
        local existing_pid=$(cat "$SIGNER_PID_FILE" 2>/dev/null)
        if [ -n "$existing_pid" ] && kill -0 "$existing_pid" 2>/dev/null; then
            echo "✓ Xahau Signing Service already running (PID: $existing_pid)"
            # Set flag for future rounds
            echo "$(date '+%Y-%m-%d %H:%M:%S') - Service detected running, setting flag" > "$SIGNER_ENV_FLAG"
            return 0
        else
            echo "Removing stale PID file..."
            rm -f "$SIGNER_PID_FILE"
        fi
    fi
    
    # Change to signing service directory
    cd "$SIGNER_DIR" || {
        echo "✗ Failed to change to signing service directory"
        return 1
    }
    
    # Check for NCC-built bundle first (preferred), then fallback to legacy binary
    local BINARY_TO_USE=""
    if [ -f "index.js" ]; then
        BINARY_TO_USE="index.js"
        echo "✓ Using NCC-built bundle (Node.js 20 compatible)"
    elif [ -f "xahau-transaction-signer" ]; then
        BINARY_TO_USE="xahau-transaction-signer"
        echo "⚠ Using legacy binary (may have OpenSSL issues)"
        
        if [ ! -x "$BINARY_TO_USE" ]; then
            echo "Making $BINARY_TO_USE executable..."
            chmod +x "$BINARY_TO_USE"
        fi
        
        # Verify binary is executable
        if [ ! -x "$BINARY_TO_USE" ]; then
            echo "✗ $BINARY_TO_USE binary is not executable"
            cd - > /dev/null
            return 1
        fi
    else
        echo "✗ No xahau-transaction-signer found in signing service directory"
        cd - > /dev/null
        return 1
    fi
    
    echo "✓ Xahau Signing Service binary ready"
    
    # Verify assets directory exists (required for NFT processing)
    if [ -d "assets" ]; then
        echo "✓ Assets directory found - NFT processing will be available"
    else
        echo "⚠ Warning: Assets directory not found - NFT processing may fail"
        echo "  → Basic blockchain operations will still work"
    fi
    
    # Start the signing service in the background with legacy OpenSSL support
    echo "Starting Xahau Signing Service..."
    if [ "$BINARY_TO_USE" = "index.js" ]; then
        # NCC bundle - run with Node.js directly
        nohup env NODE_OPTIONS="--openssl-legacy-provider" node ./index.js > ../xahau_signer.log 2>&1 &
        echo "  → Using NCC bundle with Node.js runtime"
    else
        # Legacy binary
        nohup env NODE_OPTIONS="--openssl-legacy-provider" ./"$BINARY_TO_USE" > ../xahau_signer.log 2>&1 &
        echo "  → Using legacy binary"
    fi
    local service_pid=$!
    
    # Save PID for later cleanup
    echo "$service_pid" > "../xahau_signer.pid"
    
    # Wait a moment and check if service started successfully
    sleep 2
    if kill -0 "$service_pid" 2>/dev/null; then
        echo "✓ Xahau Signing Service started successfully (PID: $service_pid)"
        echo "  → Service URL: http://localhost:3001"
        echo "  → Log file: ../xahau_signer.log"
        echo "  → Runtime: $BINARY_TO_USE"
        
        # Set startup flag to prevent duplicate starts in future HotPocket rounds
        echo "$(date '+%Y-%m-%d %H:%M:%S') - Binary service started successfully (PID: $service_pid)" > "$SIGNER_ENV_FLAG"
        echo "✓ Startup flag set for future HotPocket rounds"
    else
        echo "✗ Failed to start Xahau Signing Service binary"
        rm -f "../xahau_signer.pid"
        cd - > /dev/null
        return 1
    fi
    
    # Return to original directory
    cd - > /dev/null
    return 0
}

# Function to cleanup Xahau signing service on exit
cleanup_xahau_service() {
    local SIGNER_PID_FILE="../../../xahau_signer.pid"
    local SIGNER_ENV_FLAG="../../../xahau_signer.started"
    
    if [ -f "$SIGNER_PID_FILE" ]; then
        local service_pid=$(cat "$SIGNER_PID_FILE" 2>/dev/null)
        if [ -n "$service_pid" ] && kill -0 "$service_pid" 2>/dev/null; then
            echo "Stopping Xahau Signing Service (PID: $service_pid)..."
            kill "$service_pid" 2>/dev/null
            sleep 1
            # Force kill if still running
            if kill -0 "$service_pid" 2>/dev/null; then
                kill -9 "$service_pid" 2>/dev/null
            fi
        fi
        rm -f "$SIGNER_PID_FILE"
    fi
    
    # Remove startup flag when service is cleaned up
    # Note: This allows service restart if container is fully restarted
    # but flag persists across HotPocket rounds within same container
    if [ -f "$SIGNER_ENV_FLAG" ]; then
        echo "$(date '+%Y-%m-%d %H:%M:%S') - Service cleanup, flag preserved for container restart" >> "$SIGNER_ENV_FLAG"
    fi
}

# Function to reset Xahau signing service (for manual debugging)
reset_xahau_signing_service() {
    echo "=== Resetting Xahau Signing Service ==="
    
    local SIGNER_PID_FILE="../../../xahau_signer.pid"
    local SIGNER_ENV_FLAG="../../../xahau_signer.started"
    
    # Stop any running service
    if [ -f "$SIGNER_PID_FILE" ]; then
        local service_pid=$(cat "$SIGNER_PID_FILE" 2>/dev/null)
        if [ -n "$service_pid" ] && kill -0 "$service_pid" 2>/dev/null; then
            echo "Stopping existing service (PID: $service_pid)..."
            kill "$service_pid" 2>/dev/null
            sleep 1
            if kill -0 "$service_pid" 2>/dev/null; then
                kill -9 "$service_pid" 2>/dev/null
            fi
        fi
    fi
    
    # Clean up all files
    rm -f "$SIGNER_PID_FILE" "$SIGNER_ENV_FLAG"
    echo "✓ All Xahau Signing Service files cleaned up"
    echo "  → Service will restart on next contract execution"
}

# Function to reset persistent files setup (for manual debugging)
reset_persistent_files_setup() {
    echo "=== Resetting Persistent Files Setup ==="
    
    local PERSISTENT_SETUP_FLAG="../../../persistent_files.setup"
    
    if [ -f "$PERSISTENT_SETUP_FLAG" ]; then
        rm -f "$PERSISTENT_SETUP_FLAG"
        echo "✓ Persistent files setup flag removed"
        echo "  → Files will be re-processed on next contract execution"
    else
        echo "✓ No persistent files setup flag found - already reset"
    fi
}

# Setup cleanup trap
trap cleanup_xahau_service EXIT INT TERM

# Setup persistent files before starting contract
if ! setup_persistent_files; then
    echo "Error: Failed to setup persistent files"
    exit 1
fi

# Setup Xahau Signing Service (with HotPocket round protection)
if ! setup_xahau_signing_service; then
    echo "⚠ Warning: Failed to setup Xahau Signing Service"
    echo "  → Contract will continue but blockchain operations may fail"
fi

# Check for available contract versions and select best option
if [ -f "./AIContract_daemon" ]; then
    echo "Using integrated AI Game Engine with AI Jury validation"
    
    # Show status of persistent files before starting
    echo "--- Persistent File Status ---"
    
    # Check for model files dynamically
    echo "Model files in persistent directory:"
    local model_files_found=0
    if [ -d "../../../model" ]; then
        for model_file in ../../../model/*.gguf; do
            if [ -f "$model_file" ]; then
                model_files_found=$((model_files_found + 1))
                local model_name=$(basename "$model_file")
                local model_size=$(stat -c%s "$model_file" 2>/dev/null || echo "0")
                local model_size_mb=$((model_size / 1024 / 1024))
                echo "  ✓ $model_name: ${model_size_mb} MB"
            fi
        done
        
        if [ $model_files_found -eq 0 ]; then
            echo "  ✗ No .gguf model files found in persistent directory"
        else
            echo "  → Total models available: $model_files_found"
        fi
    else
        echo "  ✗ Model directory not found in persistent directory"
    fi
    
    if [ -f "../../../AIDaemon" ]; then
        echo "✓ AI Game Daemon binary available in persistent directory"
    else
        echo "✗ AI Game Daemon binary not found in persistent directory"
    fi
    
    if [ -f "../../../ai_jury_daemon" ]; then
        echo "✓ AI Jury Daemon binary available in persistent directory"
    else
        echo "✗ AI Jury Daemon binary not found in persistent directory"
    fi
    
    # Show Xahau Signing Service status
    if [ -f "../../../xahau_signer.started" ]; then
        if [ -f "../../../xahau_signer.pid" ]; then
            local signer_pid=$(cat "../../../xahau_signer.pid" 2>/dev/null)
            if [ -n "$signer_pid" ] && kill -0 "$signer_pid" 2>/dev/null; then
                echo "✓ Xahau Signing Service binary running (PID: $signer_pid, flagged)"
            else
                echo "⚠ Xahau Signing Service flagged but binary not running"
            fi
        else
            echo "⚠ Xahau Signing Service flagged but no PID file"
        fi
    else
        echo "○ Xahau Signing Service binary not yet started"
    fi
    
    # Show binary status
    if [ -f "../../../xahau_signer/xahau-transaction-signer" ]; then
        if [ -x "../../../xahau_signer/xahau-transaction-signer" ]; then
            echo "✓ Xahau Signing Service binary available and executable"
        else
            echo "⚠ Xahau Signing Service binary found but not executable"
        fi
    else
        echo "✗ Xahau Signing Service binary not found in persistent directory"
    fi
    
    # Show persistent files setup status
    if [ -f "../../../persistent_files.setup" ]; then
        echo "✓ Persistent files setup completed and flagged"
    else
        echo "○ Persistent files setup not yet completed"
    fi
    echo "------------------------------"
    
    # Load environment variables from Xahau Signer .env file
    echo "Loading environment configuration..."
    if [ -f "../../../xahau_signer/.env" ]; then
        # Export MINTER_WALLET_SEED from the .env file
        MINTER_WALLET_SEED=$(grep "^MINTER_WALLET_SEED=" "../../../xahau_signer/.env" | cut -d '=' -f2-)
        if [ -n "$MINTER_WALLET_SEED" ]; then
            export MINTER_WALLET_SEED="$MINTER_WALLET_SEED"
            echo "✓ MINTER_WALLET_SEED loaded from configuration"
        else
            echo "⚠ Warning: MINTER_WALLET_SEED not found in .env file"
        fi
    else
        echo "⚠ Warning: .env file not found - NFT minting may not work"
        echo "  → Expected at: ../../../xahau_signer/.env"
    fi
    
    exec "./AIContract_daemon" "$@"
else
    echo "Error: No contract binary found!"
    echo "Expected: AIContract_daemon (integrated)"
    exit 1
fi
EOF
    
    chmod +x "$DEPLOYMENT_DIR/run_contract.sh"
    print_success "Deployment script updated for integrated system"
}

# Verify all builds
verify_integrated_build() {
    print_info "Verifying unified build results..."
    
    local errors=0
    local required_binaries=(
        "ai_jury_daemon:AI Jury Daemon"
        "AIDaemon:AI Game Daemon"
        "AIContract_daemon:AI Game Contract with Jury Integration"
    )
    
    # Check required binaries
    for binary_info in "${required_binaries[@]}"; do
        local binary_name="${binary_info%%:*}"
        local binary_desc="${binary_info##*:}"
        
        if [ -f "$DEPLOYMENT_DIR/$binary_name" ] && [ -x "$DEPLOYMENT_DIR/$binary_name" ]; then
            print_success "✓ $binary_desc ($binary_name) is valid and executable"
        else
            print_error "✗ $binary_desc ($binary_name) missing or not executable"
            errors=$((errors + 1))
        fi
    done
    
    # Check model files
    if [ -f "$DEPLOYMENT_DIR/model/Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf" ]; then
        print_success "✓ 8B model file available for deployment"
    else
        print_warning "⚠ 8B model file not found in deployment directory"
    fi
    
    # Check Xahau Signing Service (Binary Version)
    if [ -d "$DEPLOYMENT_DIR/xahau_signer" ]; then
        print_success "✓ Xahau Signing Service directory deployed"
        
        local xahau_binary="$DEPLOYMENT_DIR/xahau_signer/xahau-transaction-signer"
        if [ -f "$xahau_binary" ] && [ -x "$xahau_binary" ]; then
            print_success "✓ Xahau Signing Service binary is valid and executable"
        else
            print_error "✗ Xahau Signing Service binary missing or not executable"
            errors=$((errors + 1))
        fi
        
        # Check for configuration files
        if [ -f "$DEPLOYMENT_DIR/xahau_signer/.env" ]; then
            print_success "✓ Xahau Signing Service configuration (.env) available"
        else
            print_warning "⚠ Xahau Signing Service .env file not found"
            print_info "  → Run setup_environment.sh to create configuration"
        fi
        
        # Check for assets directory
        if [ -d "$DEPLOYMENT_DIR/xahau_signer/assets" ]; then
            print_success "✓ Xahau Signing Service assets directory available"
        else
            print_warning "⚠ Xahau Signing Service assets directory not found"
        fi
    else
        print_error "✗ Xahau Signing Service directory not deployed"
        errors=$((errors + 1))
    fi
    
    return $errors
}

# Main build process
main() {
    print_info "=== UNIFIED AI SYSTEM BUILD ==="
    print_info "Building complete integrated system: Game Engine + AI Jury + All Daemons"
    
    clean_build
    
    local jury_success=0
    local engine_success=0
    
    # Build AI Jury System first (foundation)
    if build_jury_system; then
        print_success "AI Jury System build completed"
        jury_success=1
    else
        print_warning "AI Jury System build failed"
    fi
    
    # Build Game Engine with AI Jury Integration
    if build_game_engine; then
        print_success "Game Engine with AI Jury build completed"
        engine_success=1
    else
        print_warning "Game Engine build failed"
    fi
    
    # Check if we have the complete integrated system working
    if [ $jury_success -eq 0 ] || [ $engine_success -eq 0 ]; then
        print_error "Integrated system build failed! Need both AI Jury and Game Engine."
        exit 1
    fi
    
    # Deploy model files and update scripts
    deploy_model_files
    update_deployment_script
    
    # Verify builds
    if ! verify_integrated_build; then
        print_warning "Some components failed verification, but continuing..."
    fi
    
    print_success "=== UNIFIED BUILD COMPLETE ==="
    print_info "Deployment directory: $DEPLOYMENT_DIR"
    print_info ""
    print_info "Built components:"
    if [ $jury_success -eq 1 ]; then
        print_info "✓ AI Jury System (ai_jury_daemon)"
    fi
    if [ $engine_success -eq 1 ]; then
        print_info "✓ Game Engine with AI Jury (AIDaemon + AIContract_daemon)"
    fi
    print_info "✓ Xahau Signing Service (xahau-transaction-signer)"
    print_info ""
    print_info "Unified system: Game Engine with integrated AI Jury validation"
    print_info ""
    print_info "Usage:"
    print_info "  cd $DEPLOYMENT_DIR"
    print_info "  ./run_contract.sh                    # Run complete integrated system"
    print_info ""
    print_info "Manual testing:"
    print_info "  ./ai_jury_daemon --test              # Test AI Jury Daemon"
    print_info "  ./AIDaemon <model_path>              # Test Game Engine Daemon"
    print_info "  cd xahau_signer && NODE_OPTIONS=\"--openssl-legacy-provider\" ./xahau-transaction-signer  # Test Xahau Signer"
}

# Handle command line arguments
case "${1:-build}" in
    "clean")
        clean_build
        print_success "Build directory cleaned"
        ;;
    "build"|*)
        main
        ;;
esac
