#ifndef AI_JURY_MODULE_H
#define AI_JURY_MODULE_H

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <nlohmann/json.hpp>

// Forward declarations
struct hp_user;

namespace AIJury {

// Core decision structure
struct Decision {
    bool isValid;
    double confidence;     // 0.0 to 1.0
    std::string reason;    // Human-readable explanation
    std::string metadata;  // Additional context data
};

// Vote structure for consensus
struct Vote {
    int requestId;
    bool isValid;
    double confidence;
    std::string reason;
    std::string juryId;
    std::string context;   // Context data for validation
    
    std::string toJson() const;
    static Vote fromJson(const std::string& json);
};

// Request state for consensus tracking
struct RequestState {
    const struct hp_user* user;
    int requestId;
    std::string messageType;
    std::string messageData;
    std::string context;
    
    // Consensus state
    bool resolved = false;
    int received = 0;
    int tally[2] = {0, 0};         // [invalid_count, valid_count]
    double confidenceSum[2] = {0.0, 0.0};
    
    // Response callback
    std::function<void(const std::string&)> responseCallback;
};

// Interface for decision-making engines
class IDecisionEngine {
public:
    virtual ~IDecisionEngine() = default;
    virtual Decision makeDecision(const std::string& messageType, 
                                const std::string& messageData, 
                                const std::string& context) = 0;
    virtual std::string getEngineInfo() const = 0;
};

// AI model-based decision engine - Uses direct AI Daemon communication
class AIModelDecisionEngine : public IDecisionEngine {
private:
    bool modelLoaded = false;
    
    // Direct AI daemon communication methods
    bool pingAIDaemon();
    std::string sendToAIDaemon(const std::string& request);
    bool waitForModelReady(int maxWaitSeconds = 300);  // Wait for model to be ready
    
public:
    AIModelDecisionEngine();
    bool loadModel();           // Connect to AI daemon
    bool isModelReady() const { return modelLoaded; }
    std::string getDaemonStats() const;  // Get daemon status via ping
    
    Decision makeDecision(const std::string& messageType, 
                        const std::string& messageData, 
                        const std::string& context) override;
    std::string getEngineInfo() const override;
};

// Main AI Jury class
class AIJuryModule {
private:
    std::unique_ptr<IDecisionEngine> decisionEngine;
    std::vector<std::unique_ptr<RequestState>> activeRequests;
    std::string juryId;
    
    // NPL messaging functions (set by contract)
    std::function<void(const std::string&)> nplBroadcast;
    std::function<void(const hp_user*, const std::string&)> userResponse;
    
public:
    explicit AIJuryModule(std::unique_ptr<IDecisionEngine> engine);
    
    // Configuration
    void setJuryId(const std::string& id) { juryId = id; }
    void setNPLBroadcast(std::function<void(const std::string&)> func) { nplBroadcast = func; }
    void setUserResponse(std::function<void(const hp_user*, const std::string&)> func) { userResponse = func; }
    
    // Core jury functions
    void processRequest(const hp_user* user, 
                       const std::string& messageType, 
                       const std::string& messageData, 
                       int requestId, 
                       int peerCount,
                       const std::string& context = "");
    
    void processVote(const std::string& voteJson, int peerCount);
    void waitForConsensus(int requestId, int peerCount, int timeoutMs = 5000);
    bool isConsensusReached(int requestId) const;
    
    // Status and utilities
    std::string getJuryStats() const;
    std::string getJuryId() const { return juryId; }
    size_t getActiveRequestCount() const { return activeRequests.size(); }
    
    // Model management (for AI-based engines)
    bool loadAIModel();
    void unloadAIModel();
    bool isAIModelReady() const;
    
private:
    RequestState* findRequest(int requestId);
    void sendConsensusResult(RequestState* state, bool majorityValid, 
                           double avgConfidence, int validVotes, int invalidVotes, int totalVotes);
    std::string escapeJson(const std::string& str) const;
};

// Factory functions
std::unique_ptr<AIJuryModule> createAIModelJury(const std::string& juryId = "");

// Utility functions
std::string generateJuryId();
std::string formatJuryResponse(const std::string& type, const std::string& decision, 
                              double confidence, const std::string& details = "");
std::string formatJuryDecisionResponse(const Vote& vote, const std::string& messageType, int peerCount);

} // namespace AIJury

#endif // AI_JURY_MODULE_H
