// filepath: /home/deilnode3/mohsan/evernode/evernode_c/src/ai_jury_module.cpp
#include "ai_jury_module.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>
#include <algorithm>
#include <thread>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <signal.h>
#include <sys/wait.h>
#include <fstream>

// DaemonManager class for automatic AI Jury Daemon startup (outside namespace)
class DaemonManager {
private:
    pid_t daemonPid = -1;
    std::string daemonPath = "../../../ai_jury_daemon";  // AI Jury daemon binary
    std::string pidFile = "../../../ai_jury_daemon.pid";
    
    bool isDaemonProcessRunning(pid_t pid) {
        if (pid <= 0) return false;
        // Check if process exists by sending signal 0
        return kill(pid, 0) == 0;
    }
    
    bool ensureDaemonBinaryExists() {
        // Check if daemon binary exists
        if (std::filesystem::exists(daemonPath)) {
            std::cout << "[AIJury] Daemon binary found: " << daemonPath << std::endl;
            return true;
        }
        
        std::cerr << "[AIJury] ERROR: Daemon binary not found: " << daemonPath << std::endl;
        return false;
    }
    
    pid_t getExistingDaemonPid() {
        if (!std::filesystem::exists(pidFile)) {
            return -1;
        }
        
        std::ifstream file(pidFile);
        pid_t pid;
        if (file >> pid) {
            return pid;
        }
        return -1;
    }
    
    void writePidFile(pid_t pid) {
        std::ofstream file(pidFile);
        if (file.is_open()) {
            file << pid << std::endl;
            file.close();
            std::cout << "[AIJury] PID " << pid << " written to " << pidFile << std::endl;
        }
    }
    
    void cleanupUnresponsiveDaemon(pid_t pid) {
        std::cout << "[AIJury] Cleaning up unresponsive daemon with PID: " << pid << std::endl;
        
        // Try graceful shutdown first
        if (kill(pid, SIGTERM) == 0) {
            std::cout << "[AIJury] Sent SIGTERM to daemon..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        
        // Force kill if still running
        if (isDaemonProcessRunning(pid)) {
            std::cout << "[AIJury] Force killing unresponsive daemon..." << std::endl;
            kill(pid, SIGKILL);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        // Clean up PID file
        std::remove(pidFile.c_str());
        std::cout << "[AIJury] Daemon cleanup complete" << std::endl;
    }
    
public:
    bool startDaemon() {
        std::cout << "[AIJury] ========== Starting AI Jury Daemon ==========" << std::endl;
        std::cout << "[AIJury] Checking AI Jury Daemon status..." << std::endl;
        std::cout << "[AIJury] Current working directory: " << std::filesystem::current_path() << std::endl;
        std::cout << "[AIJury] Expected daemon path: " << daemonPath << std::endl;
        std::cout << "[AIJury] PID file path: " << pidFile << std::endl;
        
        // Check if daemon binary exists
        if (!ensureDaemonBinaryExists()) {
            return false;
        }
        
        // First check if we have a persistent daemon running from previous rounds
        pid_t existingPid = getExistingDaemonPid();
        if (existingPid > 0) {
            std::cout << "[AIJury] Found existing daemon with PID: " << existingPid << std::endl;
            
            // Check if the process is actually running
            if (!isDaemonProcessRunning(existingPid)) {
                std::cout << "[AIJury] Process " << existingPid << " is not running - cleaning up stale PID file" << std::endl;
                unlink(pidFile.c_str());  // Remove stale PID file
            } else {
                std::cout << "[AIJury] Process " << existingPid << " is running - using existing daemon" << std::endl;
                std::cout << "[AIJury] Note: Daemon may be loading model, which can take 5+ minutes" << std::endl;
                daemonPid = existingPid;
                return true;  // Use the existing daemon
            }
        }
        
        std::cout << "[AIJury] No daemon found - starting new daemon..." << std::endl;
        std::cout << "[AIJury] Forking daemon process..." << std::endl;
        std::cout.flush();
        
        // Fork and exec the daemon
        daemonPid = fork();
        if (daemonPid == 0) {
            // Child process - exec the daemon
            std::cout << "[AIJury Child] Executing daemon: " << daemonPath << std::endl;
            std::cout.flush();
            
            execl(daemonPath.c_str(), "ai_jury_daemon", (char*)nullptr);
            std::cerr << "[AIJury Child] FATAL: Failed to exec daemon: " << strerror(errno) << std::endl;
            exit(1);
        } else if (daemonPid > 0) {
            // Parent process - write PID file immediately
            writePidFile(daemonPid);
            std::cout << "[AIJury] Daemon started with PID: " << daemonPid << " (saved to " << pidFile << ")" << std::endl;
            
            // Give daemon a moment to start (non-blocking)
            std::cout << "[AIJury] Waiting 500ms for daemon to initialize..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            // Check if daemon process is still running (just process check, not socket)
            if (isDaemonProcessRunning(daemonPid)) {
                std::cout << "[AIJury] ✓ Daemon process started successfully (PID: " << daemonPid << ")" << std::endl;
                std::cout << "[AIJury] Note: Socket may take additional time to become available during model loading" << std::endl;
                return true;
            } else {
                std::cerr << "[AIJury] ERROR: Daemon process failed to start or crashed immediately" << std::endl;
                std::remove(pidFile.c_str());
                return false;
            }
        } else {
            std::cerr << "[AIJury] FATAL: Failed to fork daemon process: " << strerror(errno) << std::endl;
            return false;
        }
    }
    
    void stopDaemon() {
        if (daemonPid > 0) {
            std::cout << "[AIJury] Stopping daemon with PID: " << daemonPid << std::endl;
            kill(daemonPid, SIGTERM);
            
            // Wait for graceful shutdown
            int status;
            pid_t result = waitpid(daemonPid, &status, WNOHANG);
            if (result == 0) {
                // Still running, force kill
                std::this_thread::sleep_for(std::chrono::seconds(2));
                kill(daemonPid, SIGKILL);
                waitpid(daemonPid, &status, 0);
            }
            
            // Clean up PID file
            std::remove(pidFile.c_str());
            daemonPid = -1;
            std::cout << "[AIJury] Daemon stopped and PID file removed" << std::endl;
        }
    }
    
    ~DaemonManager() {
        // DO NOT stop daemon in destructor - let it persist across rounds
        // The daemon should remain running between contract executions
        std::cout << "[AIJury] Module ending - daemon remains running for next round" << std::endl;
    }
};

// Global daemon manager instance
static std::unique_ptr<DaemonManager> g_daemonManager = nullptr;

namespace AIJury {

// Vote implementation
std::string Vote::toJson() const {
    nlohmann::json j;
    j["requestId"] = requestId;
    j["isValid"] = isValid;
    j["confidence"] = confidence;
    j["reason"] = reason;
    j["juryId"] = juryId;
    j["context"] = context;
    return j.dump();
}

Vote Vote::fromJson(const std::string& json) {
    Vote vote;
    try {
        nlohmann::json j = nlohmann::json::parse(json);
        vote.requestId = j.value("requestId", 0);
        vote.isValid = j.value("isValid", false);
        vote.confidence = j.value("confidence", 0.0);
        vote.reason = j.value("reason", "");
        vote.juryId = j.value("juryId", "");
        vote.context = j.value("context", "");
    } catch (const std::exception& e) {
        std::cerr << "[AIJury] Error parsing vote JSON: " << e.what() << std::endl;
    }
    return vote;
}

// AIModelDecisionEngine implementation
AIModelDecisionEngine::AIModelDecisionEngine() {
    // Initialize daemon manager if not already done
    if (!g_daemonManager) {
        g_daemonManager = std::make_unique<DaemonManager>();
    }
}

std::string AIModelDecisionEngine::getEngineInfo() const {
    std::string status = modelLoaded ? "Connected" : "Disconnected";
    return "AIModelDecisionEngine v1.0 - AI Jury Daemon: " + status;
}

bool AIModelDecisionEngine::loadModel() {
    std::cout << "[AIJury] Loading AI model..." << std::endl;
    // Ping daemon first
    if (!pingAIDaemon()) {
        if (!g_daemonManager->startDaemon()) {
            std::cerr << "[AIJury] Failed to start AI daemon" << std::endl;
            return false;
        }
        // Wait a few seconds for daemon to start
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    // Wait for model to be ready using daemon's status
    for (int i = 0; i < 300; ++i) {
        std::string pingResp = sendToAIDaemon("{\"type\":\"ping\"}");
        try {
            auto resp = nlohmann::json::parse(pingResp);
            if (resp.value("status", "") == "ready" && resp.value("model_loaded", false)) {
                modelLoaded = true;
                std::cout << "[AIJury] AI model loaded and ready" << std::endl;
                return true;
            }
        } catch (...) {}
        if (i % 30 == 0 && i > 0) {
            std::cout << "[AIJury] Still waiting for AI model... (" << i << "/300 seconds)" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cout << "[AIJury] Timeout waiting for AI model readiness" << std::endl;
    modelLoaded = false;
    return false;
}

bool AIModelDecisionEngine::pingAIDaemon() {
    // Simple TCP connection test to AI daemon
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }
    
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8766);  // AI jury daemon port (matches daemon configuration)
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);
    
    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    bool connected = connect(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == 0;
    close(sock);
    
    return connected;
}

std::string AIModelDecisionEngine::sendToAIDaemon(const std::string& request) {
    // Connect to AI daemon via TCP
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return "{\"error\": \"Failed to create socket\"}";
    }
    
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8766);  // AI jury daemon port (matches daemon configuration)
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);
    
    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = 120;  // 120 second timeout for AI responses (model can be slow)
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    if (connect(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        close(sock);
        return "{\"error\": \"Failed to connect to AI daemon\"}";
    }
    
    // Send request
    ssize_t sent = send(sock, request.c_str(), request.length(), 0);
    if (sent < 0) {
        close(sock);
        return "{\"error\": \"Failed to send request\"}";
    }
    
    // Receive response
    char buffer[4096];
    ssize_t received = recv(sock, buffer, sizeof(buffer) - 1, 0);
    close(sock);
    
    if (received > 0) {
        buffer[received] = '\0';
        return std::string(buffer);
    }
    
    return "{\"error\": \"No response from AI daemon\"}";
}

std::string AIModelDecisionEngine::getDaemonStats() const {
    // Send ping request to get daemon status
    return const_cast<AIModelDecisionEngine*>(this)->sendToAIDaemon("{\"type\":\"ping\"}");
}

Decision AIModelDecisionEngine::makeDecision(const std::string& messageType, 
                                           const std::string& messageData, 
                                           const std::string& context) {
    Decision decision;
    decision.isValid = false;
    decision.confidence = 0.0;
    decision.reason = "AI model not available";
    decision.metadata = "";

    // Always ping daemon and check model status before validation
    if (!pingAIDaemon()) {
        decision.reason = "AI daemon not running";
        decision.isValid = true;
        decision.confidence = 0.1;
        return decision;
    }
    std::string pingResp = sendToAIDaemon("{\"type\":\"ping\"}");
    try {
        auto resp = nlohmann::json::parse(pingResp);
        if (resp.value("status", "") != "ready" || !resp.value("model_loaded", false)) {
            decision.reason = "AI model not ready (" + resp.value("status", "") + ")";
            decision.isValid = true;
            decision.confidence = 0.1;
            return decision;
        }
    } catch (...) {
        decision.reason = "Failed to parse daemon status";
        decision.isValid = true;
        decision.confidence = 0.1;
        return decision;
    }
    // Prepare AI request
    nlohmann::json aiRequest;
    aiRequest["type"] = "validate";
    aiRequest["statement"] = messageData;
    // Note: context is not used by the daemon but we could add it later
    std::string response = sendToAIDaemon(aiRequest.dump());
    try {
        nlohmann::json aiResponse = nlohmann::json::parse(response);
        if (aiResponse.contains("error")) {
            decision.reason = "AI error: " + aiResponse["error"].get<std::string>();
            decision.isValid = true;
            decision.confidence = 0.1;
        } else {
            if (aiResponse.contains("valid")) {
                decision.isValid = aiResponse["valid"].get<bool>();
            }
            if (aiResponse.contains("confidence")) {
                decision.confidence = aiResponse["confidence"].get<double>();
            }
            if (aiResponse.contains("reason")) {
                decision.reason = aiResponse["reason"].get<std::string>();
            }
            decision.metadata = response;
        }
    } catch (const std::exception& e) {
        decision.reason = "Failed to parse AI response: " + std::string(e.what());
        decision.isValid = true;
        decision.confidence = 0.1;
    }
    return decision;
}

// AIJuryModule implementation
AIJuryModule::AIJuryModule(std::unique_ptr<IDecisionEngine> engine) 
    : decisionEngine(std::move(engine)) {
    juryId = generateJuryId();
    std::cout << "[AIJury] AI Jury Module initialized with ID: " << juryId << std::endl;
}

void AIJuryModule::processRequest(const hp_user* user, 
                                const std::string& messageType, 
                                const std::string& messageData, 
                                int requestId, 
                                int peerCount,
                                const std::string& context) {
    std::cout << "[AIJury] Processing request " << requestId << " of type: " << messageType << std::endl;
    
    // Create new request state
    auto state = std::make_unique<RequestState>();
    state->user = user;
    state->requestId = requestId;
    state->messageType = messageType;
    state->messageData = messageData;
    state->context = context;
    
    // Make AI decision
    Decision decision = decisionEngine->makeDecision(messageType, messageData, context);
    
    // Create vote
    Vote vote;
    vote.requestId = requestId;
    vote.isValid = decision.isValid;
    vote.confidence = decision.confidence;
    vote.reason = decision.reason;
    vote.juryId = juryId;
    vote.context = context;
    
    // Broadcast vote to other peers
    if (nplBroadcast) {
        nplBroadcast(vote.toJson());
        std::cout << "[AIJury] Broadcasted vote for request " << requestId << std::endl;
    }
    
    // Store the request state for consensus tracking
    activeRequests.push_back(std::move(state));
    
    std::cout << "[AIJury] Vote: " << (decision.isValid ? "VALID" : "INVALID") 
              << " (confidence: " << decision.confidence << ") - " << decision.reason << std::endl;
    
    // For single peer scenarios, process our own vote immediately to reach consensus
    // if (peerCount == 1) {
    //     std::cout << "[AIJury] Single peer scenario - processing own vote for immediate consensus" << std::endl;
    //     processVote(vote.toJson(), peerCount);
    // }
}

void AIJuryModule::processVote(const std::string& voteJson, int peerCount) {
    Vote vote = Vote::fromJson(voteJson);
    
    // Find the corresponding request
    RequestState* state = findRequest(vote.requestId);
    if (!state) {
        std::cout << "[AIJury] Received vote for unknown request " << vote.requestId << std::endl;
        return;
    }
    
    // Update consensus tracking
    state->received++;
    int voteIndex = vote.isValid ? 1 : 0;
    state->tally[voteIndex]++;
    state->confidenceSum[voteIndex] += vote.confidence;
    
    std::cout << "[AIJury] Vote received for request " << vote.requestId 
              << " (" << state->received << "/" << peerCount << ")" << std::endl;
    
    // Check if we have enough votes for consensus
    if (state->received >= peerCount) {
        int validVotes = state->tally[1];
        int invalidVotes = state->tally[0];
        bool majorityValid = validVotes > invalidVotes;
        double avgConfidence = (state->confidenceSum[0] + state->confidenceSum[1]) / state->received;
        
        sendConsensusResult(state, majorityValid, avgConfidence, validVotes, invalidVotes, state->received);
        state->resolved = true;
    }
}

void AIJuryModule::waitForConsensus(int requestId, int peerCount, int timeoutMs) {
    auto startTime = std::chrono::steady_clock::now();
    
    while (true) {
        RequestState* state = findRequest(requestId);
        if (!state || state->resolved) {
            std::cout << "[AIJury] Consensus reached for request " << requestId << std::endl;
            break;
        }
        
        // If timeoutMs is 0, just check once and return
        if (timeoutMs == 0) {
            break;
        }
        
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        
        if (elapsed >= timeoutMs) {
            std::cout << "[AIJury] Consensus timeout for request " << requestId << std::endl;
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool AIJuryModule::isConsensusReached(int requestId) const {
    for (const auto& req : activeRequests) {
        if (req->requestId == requestId) {
            return req->resolved;
        }
    }
    return false; // Request not found, consider it resolved
}

std::string AIJuryModule::getJuryStats() const {
    // Simply reuse the ping endpoint - no need for separate stat logic
    if (auto aiEngine = dynamic_cast<const AIModelDecisionEngine*>(decisionEngine.get())) {
        std::string pingResp = aiEngine->getDaemonStats();
        
        // If we got a valid response, return it directly
        if (pingResp.find("error") == std::string::npos && pingResp.find("status") != std::string::npos) {
            return pingResp;
        }
    }
    
    // Default response when daemon is not available (for consensus safety)
    return "{\"status\":\"loading\",\"model_loaded\":false,\"model_loading\":true}";
}

bool AIJuryModule::loadAIModel() {
    if (auto aiEngine = dynamic_cast<AIModelDecisionEngine*>(decisionEngine.get())) {
        return aiEngine->loadModel();
    }
    return false;
}

bool AIJuryModule::isAIModelReady() const {
    if (auto aiEngine = dynamic_cast<const AIModelDecisionEngine*>(decisionEngine.get())) {
        return aiEngine->isModelReady();
    }
    return false;
}

RequestState* AIJuryModule::findRequest(int requestId) {
    for (auto& req : activeRequests) {
        if (req->requestId == requestId) {
            return req.get();
        }
    }
    return nullptr;
}

void AIJuryModule::sendConsensusResult(RequestState* state, bool majorityValid, 
                                     double avgConfidence, int validVotes, int invalidVotes, int totalVotes) {
    nlohmann::json result;
    result["type"] = "consensus";
    result["requestId"] = state->requestId;
    result["decision"] = majorityValid ? "valid" : "invalid";
    result["confidence"] = avgConfidence;
    result["validVotes"] = validVotes;
    result["invalidVotes"] = invalidVotes;
    result["totalVotes"] = totalVotes;
    result["messageType"] = state->messageType;
    
    std::string response = formatJuryResponse("consensus", 
                                            majorityValid ? "valid" : "invalid", 
                                            avgConfidence, 
                                            result.dump());
    
    if (userResponse && state->user) {
        userResponse(state->user, response);
    }
    
    std::cout << "[AIJury] Consensus reached for request " << state->requestId 
              << ": " << (majorityValid ? "VALID" : "INVALID") 
              << " (" << validVotes << "/" << totalVotes << " valid votes)" << std::endl;
}

std::string AIJuryModule::escapeJson(const std::string& str) const {
    std::string escaped;
    for (char c : str) {
        switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

// Factory functions
std::unique_ptr<AIJuryModule> createAIModelJury(const std::string& juryId) {
    auto engine = std::make_unique<AIModelDecisionEngine>();
    auto jury = std::make_unique<AIJuryModule>(std::move(engine));
    
    if (!juryId.empty()) {
        jury->setJuryId(juryId);
    }
    
    return jury;
}

// Utility functions
std::string generateJuryId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000, 999999);
    return "jury_" + std::to_string(dis(gen));
}

std::string formatJuryResponse(const std::string& type, const std::string& decision, 
                              double confidence, const std::string& details) {
    nlohmann::json response;
    response["type"] = type;
    response["decision"] = decision;
    response["confidence"] = confidence;
    response["details"] = details;
    response["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    return response.dump();
}

std::string formatJuryDecisionResponse(const Vote& vote, const std::string& messageType, int peerCount) {
    nlohmann::json response;
    response["type"] = "jury_decision";
    response["messageType"] = messageType;
    response["requestId"] = vote.requestId;
    response["decision"] = vote.isValid ? "valid" : "invalid";
    response["confidence"] = vote.confidence;
    response["reason"] = vote.reason;
    response["juryId"] = vote.juryId;
    response["peerCount"] = peerCount;
    
    return response.dump();
}

} // namespace AIJury