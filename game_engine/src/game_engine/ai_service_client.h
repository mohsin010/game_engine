// AI Service Client - Replaces AIGameEngine and AIGameStateValidator
// Communicates with AI Daemon via TCP sockets for fast responses

#pragma once

#include <string>
#include <filesystem>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include <iostream>

class AIServiceClient {
private:
    std::string daemon_host = "127.0.0.1";
    int daemon_port = 8765;
    int connect_timeout_ms = 5000;
    
    int connectToDaemon() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == -1) {
            std::cerr << "[Client] Failed to create socket: " << strerror(errno) << std::endl;
            return -1;
        }
        
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(daemon_host.c_str());
        addr.sin_port = htons(daemon_port);
        
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            std::cerr << "[Client] Failed to connect to daemon at " << daemon_host << ":" << daemon_port
                     << " - " << strerror(errno) << std::endl;
            close(sock);
            return -1;
        }
        
        return sock;
    }
    
    std::string sendRequest(const std::string& request, bool isStatusRequest = false) {
        int sock = connectToDaemon();
        if (sock == -1) {
            // For status requests, distinguish between "daemon not running" and "socket not ready"
            if (isStatusRequest) {
                // Check if this might be a model loading scenario by checking PID file
                std::string pid_file_path = "../../../ai_daemon.pid";
                if (std::filesystem::exists(pid_file_path)) {
                    return "{\"status\":\"socket_unavailable\",\"reason\":\"daemon_loading_model\",\"error\":\"Daemon process exists but TCP connection failed - daemon likely loading model\"}";
                } else {
                    return "{\"status\":\"socket_unavailable\",\"reason\":\"no_pid_file\",\"error\":\"PID file not found - daemon not running\"}";
                }
            }
            return "{\"error\":\"Failed to connect to AI daemon\"}";
        }
        
        // Send request
        if (send(sock, request.c_str(), request.length(), 0) == -1) {
            std::cerr << "[Client] Failed to send request" << std::endl;
            close(sock);
            if (isStatusRequest) {
                return "{\"status\":\"socket_unavailable\",\"error\":\"Failed to send request\"}";
            }
            return "{\"error\":\"Failed to send request\"}";
        }
        
        // Receive response with timeout for model loading scenarios
        char buffer[8192];
        ssize_t bytes_received;
        
        if (isStatusRequest) {
            // For status requests, use longer timeout to account for model loading
            // Set socket timeout to 10 seconds
            struct timeval timeout;
            timeout.tv_sec = 10;  // 10 seconds for status requests
            timeout.tv_usec = 0;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        }
        
        bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
        close(sock);
        
        if (bytes_received <= 0) {
            if (isStatusRequest) {
                if (bytes_received == 0) {
                    return "{\"status\":\"socket_unavailable\",\"error\":\"Daemon closed connection - may be busy loading model\"}";
                } else {
                    return "{\"status\":\"socket_unavailable\",\"error\":\"Receive timeout - daemon may be loading model\"}";
                }
            }
            return "{\"error\":\"Failed to receive response\"}";
        }
        
        buffer[bytes_received] = '\0';
        return std::string(buffer);
    }
    
public:
    // Test daemon connectivity with model loading awareness
    bool isDaemonRunning() {
        nlohmann::json request;
        request["type"] = "ping";
        
        std::string response = sendRequest(request.dump(), true);  // Mark as status request
        
        try {
            nlohmann::json resp_json = nlohmann::json::parse(response);
            std::string status = resp_json.value("status", "");
            
            // Accept "loading" and "ready" as indicating daemon is running
            if (status == "loading" || status == "ready") {
                return true;
            }
            
            // For socket_unavailable, only treat as running if there's evidence of daemon process
            if (status == "socket_unavailable") {
                // Check if there's evidence of an actual daemon process running
                std::string pidFile = "../../../ai_daemon.pid";
                if (std::filesystem::exists(pidFile)) {
                    std::cout << "[Client] Socket unavailable but PID file exists - daemon may be loading model" << std::endl;
                    return true;
                } else {
                    std::cout << "[Client] Socket unavailable and no PID file - no daemon process running" << std::endl;
                    return false;
                }
            }
            
            return false;
        } catch (...) {
            // Parse error usually means connection failed entirely
            std::cout << "[Client] Failed to parse daemon response - treating as not running" << std::endl;
            return false;
        }
    }
    
    // Check if daemon is running and model is loaded
    bool isModelReady() {
        nlohmann::json request;
        request["type"] = "ping";
        
        std::string response = sendRequest(request.dump(), true);  // Mark as status request
        
        try {
            nlohmann::json resp_json = nlohmann::json::parse(response);
            std::string status = resp_json.value("status", "");
            return status == "ready" && resp_json.value("model_loaded", false);
        } catch (...) {
            return false;
        }
    }
    
    // Check if model is currently loading
    bool isModelLoading() {
        nlohmann::json request;
        request["type"] = "ping";
        
        std::string response = sendRequest(request.dump(), true);  // Mark as status request
        
        try {
            nlohmann::json resp_json = nlohmann::json::parse(response);
            std::string status = resp_json.value("status", "");
            // Consider both "loading" status and "socket_unavailable" as model loading
            // (socket_unavailable during startup usually means model is still loading)
            return status == "loading" || (status == "socket_unavailable" && resp_json.value("model_loading", false));
        } catch (...) {
            return false;
        }
    }
    
    // Game creation (replaces AIGameEngine::createGame)
    std::string createGame(const std::string& userPrompt, const std::string& userIdHex = "") {
        nlohmann::json request;
        request["type"] = "create_game";
        request["prompt"] = userPrompt;
        request["user_id"] = userIdHex;
        
        std::cout << "[Client] Requesting game creation..." << std::endl;
        std::string response = sendRequest(request.dump());
        std::cout << "[Client] Game creation response received" << std::endl;
        
        return response;
    }
    
    // Player action processing (replaces AIGameEngine::processPlayerAction)
    std::string processPlayerAction(const std::string& gameId, const std::string& action, 
                                  const std::string& currentGameState = "", const std::string& gameWorld = "",
                                  bool continue_conversation = false) {
        nlohmann::json request;
        request["type"] = "player_action";
        request["game_id"] = gameId;
        request["action"] = action;
        request["game_state"] = currentGameState;
        request["game_world"] = gameWorld;
        request["continue_conversation"] = continue_conversation;
        
        std::cout << "[Client] Processing player action..." << std::endl;
        std::string response = sendRequest(request.dump());
        std::cout << "[Client] Action processing response received" << std::endl;
        
        return response;
    }
    
    // Get daemon status information
    std::string getDaemonStatus() {
        nlohmann::json request;
        request["type"] = "ping";
        
        return sendRequest(request.dump(), true);  // Mark as status request
    }
};
