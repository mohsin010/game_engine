#ifndef NFT_MINTING_CLIENT_H
#define NFT_MINTING_CLIENT_H

#include <string>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>
#include <httplib.h>

/**
 * NFTMintResult - Result of a single NFT minting operation
 */
struct NFTMintResult {
    bool success;
    std::string item_name;
    std::string uritoken_id;          // Actual URITokenID from Xahau blockchain
    std::string transaction_hash;
    std::string metadata_uri;
    std::string engine_result;
    std::string error_message;
    bool validated;
    
    // Additional fields for detailed tracking
    int engine_result_code;
    int64_t mint_timestamp;
};

/**
 * NFTMintBatch - Result of batch NFT minting operation
 */
struct NFTMintBatch {
    bool success;                     // True if all mints succeeded
    int total_requested;
    int successful_mints;
    int failed_mints;
    int64_t batch_timestamp;
    std::vector<NFTMintResult> results;
    
    // Batch summary
    std::string first_success_hash;   // Hash of first successful transaction
    std::vector<std::string> failed_items; // Names of failed items
};

/**
 * NFTMintingClient - Simplified client for NFT minting operations
 * 
 * This class handles:
 * - Game inventory parsing and metadata loading
 * - HTTP communication with Node.js signing service
 * - Batch operations and result aggregation
 * 
 * All blockchain operations (transaction creation, signing, submission)
 * are delegated to the Node.js signing service for better reliability.
 */
class NFTMintingClient {
private:
    // Configuration
    std::string nftMetadataDir = "nft_metadata";
    std::string signingServiceUrl = "http://localhost:3001"; // Default fallback
    std::string minterWalletSeed;
    
    // HTTP client for signing service communication
    httplib::Client* httpClient;
    
    // Helper methods
    std::string loadItemMetadata(const std::string& itemName);
    std::vector<std::string> parseInventoryItems(const std::string& inventoryString);
    std::string extractMetadataUri(const nlohmann::json& metadata);
    NFTMintResult parseNFTMintResponse(const nlohmann::json& response);
    nlohmann::json makeSigningServiceCall(const std::string& endpoint, const nlohmann::json& params);
    
    // Validation helpers
    bool validateConfiguration();
    bool isValidMetadataUri(const std::string& uri);
    
public:
    NFTMintingClient();
    ~NFTMintingClient();
    
    // Configuration methods
    void setSigningServiceUrl(const std::string& url);
    void setMinterWallet(const std::string& seed);
    void setMetadataDirectory(const std::string& dir);
    
    // Core NFT minting methods
    NFTMintResult mintSingleNFT(const std::string& itemName, const std::string& metadataUri);
    NFTMintBatch mintNFTsForGame(const std::string& gameId, const nlohmann::json& nftData);
    
    // Batch operations
    NFTMintBatch mintItemList(const std::vector<std::string>& itemNames);
    NFTMintBatch mintInventoryString(const std::string& inventoryString);
    
    // Status and validation
    bool isAlreadyMinted(const nlohmann::json& nftData);
    bool testConnection();
    bool validateWallet();
    
    // Utility methods
    std::string getAccountAddress(); // Derive address from seed via signing service
    nlohmann::json getServiceHealth();
    
    // Error handling and logging
    void setVerboseLogging(bool enabled);
    
private:
    bool verboseLogging = true;
    void log(const std::string& message);
    void logError(const std::string& message);
};

#endif // NFT_MINTING_CLIENT_H
