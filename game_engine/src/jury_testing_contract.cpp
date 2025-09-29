#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <random>

extern "C" {
    #include "hotpocket_contract.h"
}

#include "ai_jury_module.h"

// Simple validation request structure
struct ValidationRequest {
    int request_idx;
    std::string statement;       // Statement to validate
    std::string context;         // Additional context for validation
    
    std::string toJson() const {
        return "{\"request_idx\":" + std::to_string(request_idx) + 
               ",\"statement\":\"" + escapeJson(statement) + "\"" +
               ",\"context\":\"" + escapeJson(context) + "\"}";
    }

private:
    static std::string escapeJson(const std::string& str) {
        std::string result = str;
        std::replace(result.begin(), result.end(), '"', '\'');
        return result;
    }
};

// Validation vote structure
struct ValidationVote {
    int request_idx;
    bool isValid;              // Decision: valid/invalid
    double confidence;         // Confidence in decision (0.0 - 1.0)
    std::string reason;        // Reasoning for the decision
    std::string juryId;        // Identifier of the jury node
    
    std::string toJson() const {
        return "{\"request_idx\":" + std::to_string(request_idx) + 
               ",\"is_valid\":" + (isValid ? "true" : "false") +
               ",\"confidence\":" + std::to_string(confidence) +
               ",\"reason\":\"" + escapeJson(reason) + "\"" +
               ",\"jury_id\":\"" + juryId + "\"}";
    }

private:
    static std::string escapeJson(const std::string& str) {
        std::string result = str;
        std::replace(result.begin(), result.end(), '"', '\'');
        return result;
    }
};

// Request state tracking for validation consensus
struct ValidationRequestState {
    const struct hp_user* user;
    int request_idx;
    std::string statement;       // Statement being validated
    std::string context;         // Additional context
    
    // Consensus state
    bool resolved = false;
    int received = 0;
    int tally[2] = {0, 0};         // 0=invalid, 1=valid
    double confidenceSum[2] = {0.0, 0.0};
};

// Global state
static std::vector<std::unique_ptr<ValidationRequestState>> g_validationRequests;
static std::string g_juryId;

// Global AI Jury instance
static std::unique_ptr<AIJury::AIJuryModule> g_aiJury;

// Jury NPL broadcast callback
void juryNPLBroadcast(const std::string& msg) {
    hp_write_npl_msg(msg.c_str(), msg.length());
}

// Jury user response callback
void juryUserResponse(const hp_user* user, const std::string& response) {
    hp_write_user_msg(user, response.c_str(), response.length());
}

// Jury vote processing (called for each NPL vote)
void process_jury_vote(const std::string& voteJson, int peer_count) {
    if (g_aiJury) {
        g_aiJury->processVote(voteJson, peer_count);
    }
}

// Wait for jury consensus (actively processes NPL messages until consensus reached)
// Relies on HotPocket's round timeout instead of custom timeout
void waitForJuryConsensus(int request_idx, int peer_count) {
    if (!g_aiJury) return;
    
    char sender[HP_PUBLIC_KEY_SIZE];
    char *npl_msg = (char*)malloc(HP_NPL_MSG_MAX_SIZE);
    
    std::cout << "=== WAITING FOR JURY CONSENSUS ===" << std::endl;
    std::cout << "Request ID: " << request_idx << ", Peer count: " << peer_count << std::endl;
    std::cout << "No timeout - relying on HotPocket round timeout" << std::endl;
    
    // Keep processing NPL messages until consensus is reached
    // HotPocket will timeout the round if it takes too long
    while (true) {
        // Check if consensus has been reached first
        if (g_aiJury->isConsensusReached(request_idx)) {
            std::cout << "[Jury] Consensus reached for request " << request_idx << " - exiting wait loop" << std::endl;
            break;
        }
        
        // Check for incoming NPL messages with short timeout
        const int npl_len = hp_read_npl_msg(npl_msg, sender, 100); // 100ms timeout
        if (npl_len > 0) {
            std::string voteJson(npl_msg, npl_len);
            std::cout << "Received jury vote: " << voteJson.substr(0, 100) << "..." << std::endl;
            process_jury_vote(voteJson, peer_count);
        }
        
        // Small delay to prevent busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    free(npl_msg);
    std::cout << "=== JURY CONSENSUS WAIT COMPLETE ===" << std::endl;
}

// Jury status message handler
void process_stat_message(const struct hp_user* user) {
    if (g_aiJury) {
        std::string stats = g_aiJury->getJuryStats();
        hp_write_user_msg(user, stats.c_str(), stats.length());
    }
}

// Jury query handler
void process_jury_message(const struct hp_user* user, const std::string& messageType, const std::string& messageData, int request_idx, int peer_count) {
    if (!g_aiJury) return;
    g_aiJury->processRequest(user, messageType, messageData, request_idx, peer_count, "jury_contract_context");
    // Wait for AI jury to complete processing - no timeout, rely on HotPocket
    waitForJuryConsensus(request_idx, peer_count);
}

int main(int argc, char **argv) {
    std::cout << "=== JURY CONTRACT (AI Jury) STARTING ===" << std::endl;
    if (hp_init_contract() == -1) {
        std::cout << "Failed to initialize contract" << std::endl;
        return 1;
    }
    const struct hp_contract_context *ctx = hp_get_context();
    if (!ctx) {
        std::cout << "Failed to get contract context" << std::endl;
        return 1;
    }
    // Initialize AI Jury
    g_aiJury = AIJury::createAIModelJury();
    g_aiJury->setNPLBroadcast(juryNPLBroadcast);
    g_aiJury->setUserResponse(juryUserResponse);
    std::cout << "Jury ID: " << g_aiJury->getJuryId() << std::endl;
    // Load AI model (starts daemon if needed)
    g_aiJury->loadAIModel();
    // Get peer count
    const int peer_count = ctx->unl.count;
    const void *input_mmap = hp_init_user_input_mmap();
    std::cout << "Processing " << ctx->users.count << " users" << std::endl;
    for (size_t u = 0; u < ctx->users.count; u++) {
        const struct hp_user *user = &ctx->users.list[u];
        for (size_t input_idx = 0; input_idx < user->inputs.count; input_idx++) {
            char* buf = (char*)((char*)input_mmap + user->inputs.list[input_idx].offset);
            size_t len = user->inputs.list[input_idx].size;
            if (len > 0) {
                std::string message(buf, len);
                std::string messageType = "";
                std::string messageData = "";
                
                std::cout << "Received message: " << message << std::endl;
                
                // Parse JSON format: {"type": "stat"} or {"type": "validate", "statement": "..."}
                if (message.find("{") == 0 && message.find("}") != std::string::npos) {
                    // JSON format
                    if (message.find("\"type\":\"stat\"") != std::string::npos) {
                        messageType = "stat";
                    } else if (message.find("\"type\":\"validate\"") != std::string::npos) {
                        messageType = "validate";
                        // Extract statement from JSON
                        size_t pos = message.find("\"statement\":\"");
                        if (pos != std::string::npos) {
                            pos = message.find(":\"", pos) + 2;
                            size_t end = message.find("\"", pos);
                            if (end != std::string::npos) {
                                messageData = message.substr(pos, end - pos);
                            }
                        }
                    } else {
                        messageType = "unknown";
                        messageData = message;
                    }
                }
                // Parse simple string format: "stat" or "validate:statement"  
                else if (message == "stat") {
                    messageType = "stat";
                } else if (message.find("validate:") == 0) {
                    messageType = "validate";
                    messageData = message.substr(9); // Remove "validate:" prefix
                } else {
                    messageType = "unknown";
                    messageData = message;
                }
                
                // Handle only validation-focused requests
                if (messageType == "stat") {
                    process_stat_message(user);
                } else if (messageType == "validate" && !ctx->readonly) {
                    process_jury_message(user, messageType, messageData, input_idx, peer_count);
                } else if (messageType == "validate" && ctx->readonly) {
                    std::string readonly_response = "{\"type\":\"info\",\"message\":\"Contract in readonly mode\"}";
                    hp_write_user_msg(user, readonly_response.c_str(), readonly_response.length());
                } else {
                    std::string error = "{\"type\":\"error\",\"error\":\"Invalid request. Use 'stat' for status or 'validate:statement' for validation\",\"received\":\"" + message + "\"}";
                    hp_write_user_msg(user, error.c_str(), error.length());
                }
            }
        }
    }
    std::cout << "=== JURY CONTRACT COMPLETE ----------===" << std::endl;
    return 0;
}
