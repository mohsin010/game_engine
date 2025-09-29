#include "nft_minting_client.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <cstdlib> // For std::getenv
#include <cstdlib> // For std::getenv

NFTMintingClient::NFTMintingClient() {
    // Load signing service URL from environment or .env.signing file
    const char* envUrl = std::getenv("SIGNING_SERVICE_URL");
    if (envUrl) {
        signingServiceUrl = envUrl;
        log("Using signing service URL from environment: " + signingServiceUrl);
    } else {
        // Try to read from .env.signing file created by the signing service
        std::ifstream envFile("xahau_signer/.env.signing");
        if (envFile.is_open()) {
            std::string line;
            while (std::getline(envFile, line)) {
                if (line.find("SIGNING_SERVICE_URL=") == 0) {
                    signingServiceUrl = line.substr(20); // Remove "SIGNING_SERVICE_URL="
                    log("Using signing service URL from .env.signing: " + signingServiceUrl);
                    break;
                }
            }
            envFile.close();
        } else {
            log("Using default signing service URL: " + signingServiceUrl);
        }
    }
    
    // Initialize HTTP client for signing service
    httpClient = new httplib::Client("localhost", 3001);
    httpClient->set_connection_timeout(100);
    httpClient->set_read_timeout(300);
    
    log("NFT Minting Client initialized");
    log("Configured signing service: " + signingServiceUrl);
}

NFTMintingClient::~NFTMintingClient() {
    delete httpClient;
    log("NFT Minting Client destroyed");
}

void NFTMintingClient::log(const std::string& message) {
    if (verboseLogging) {
        std::cout << "[NFTMintingClient] " << message << std::endl;
    }
}

void NFTMintingClient::logError(const std::string& message) {
    std::cerr << "[NFTMintingClient] ERROR: " << message << std::endl;
}

void NFTMintingClient::setVerboseLogging(bool enabled) {
    verboseLogging = enabled;
}

void NFTMintingClient::setSigningServiceUrl(const std::string& url) {
    signingServiceUrl = url;
    
    // Recreate HTTP client with new URL
    delete httpClient;
    
    // Parse URL to extract host and port
    std::string host = "localhost";
    int port = 3001;
    
    if (url.find("http://") == 0) {
        std::string hostPort = url.substr(7);
        size_t colonPos = hostPort.find(':');
        if (colonPos != std::string::npos) {
            host = hostPort.substr(0, colonPos);
            port = std::stoi(hostPort.substr(colonPos + 1));
        } else {
            host = hostPort;
        }
    }
    
    httpClient = new httplib::Client(host, port);
    httpClient->set_connection_timeout(100);
    httpClient->set_read_timeout(300);
    
    log("Signing service URL updated: " + url);
}

void NFTMintingClient::setMinterWallet(const std::string& seed) {
    minterWalletSeed = seed;
    log("Minter wallet seed configured");
}

void NFTMintingClient::setMetadataDirectory(const std::string& dir) {
    nftMetadataDir = dir;
    log("Metadata directory set to: " + dir);
}

nlohmann::json NFTMintingClient::makeSigningServiceCall(const std::string& endpoint, const nlohmann::json& params) {
    log("Calling signing service: " + endpoint);
    
    std::string jsonBody = params.dump();
    auto response = httpClient->Post(endpoint, jsonBody, "application/json");
    
    if (!response) {
        throw std::runtime_error("Failed to connect to signing service at " + signingServiceUrl);
    }
    
    if (response->status != 200) {
        throw std::runtime_error("Signing service error (" + std::to_string(response->status) + "): " + response->body);
    }
    
    try {
        return nlohmann::json::parse(response->body);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error("Failed to parse signing service response: " + std::string(e.what()));
    }
}

std::string NFTMintingClient::loadItemMetadata(const std::string& itemName) {
    std::string metadataPath = nftMetadataDir + "/" + itemName + ".json";
    
    if (!std::filesystem::exists(metadataPath)) {
        logError("Metadata file not found: " + metadataPath);
        return "";
    }
    
    std::ifstream file(metadataPath);
    if (!file) {
        logError("Failed to open metadata file: " + metadataPath);
        return "";
    }
    
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    
    log("Loaded metadata for item: " + itemName);
    return content;
}

std::string NFTMintingClient::extractMetadataUri(const nlohmann::json& metadata) {
    // Look for metadata URI in the JSON
    if (metadata.contains("nft_metadata") && !metadata["nft_metadata"].empty()) {
        return metadata["nft_metadata"].get<std::string>();
    }
    
    // Fallback patterns
    if (metadata.contains("metadata_uri")) {
        return metadata["metadata_uri"].get<std::string>();
    }
    
    if (metadata.contains("uri")) {
        return metadata["uri"].get<std::string>();
    }
    
    // No URI found
    logError("No metadata URI found in JSON");
    return "";
}

std::vector<std::string> NFTMintingClient::parseInventoryItems(const std::string& inventoryString) {
    std::vector<std::string> items;
    
    log("Parsing inventory string: " + inventoryString);
    
    // Parse inventory string like "[torch, magic_key, crystal_of_power]"
    std::string cleaned = inventoryString;
    
    // Remove brackets
    if (!cleaned.empty() && cleaned.front() == '[') cleaned = cleaned.substr(1);
    if (!cleaned.empty() && cleaned.back() == ']') cleaned = cleaned.substr(0, cleaned.length() - 1);
    
    // Split by comma
    std::stringstream ss(cleaned);
    std::string item;
    
    while (std::getline(ss, item, ',')) {
        // Trim whitespace
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        
        // Remove quotes if present
        if (!item.empty() && item.front() == '"') {
            item = item.substr(1);
        }
        if (!item.empty() && item.back() == '"') {
            item = item.substr(0, item.length() - 1);
        }
        
        if (!item.empty()) {
            items.push_back(item);
        }
    }
    
    log("Parsed " + std::to_string(items.size()) + " items from inventory");
    return items;
}

bool NFTMintingClient::isValidMetadataUri(const std::string& uri) {
    // Basic URI validation
    return !uri.empty() && 
           (uri.find("http://") == 0 || uri.find("https://") == 0 || uri.find("ipfs://") == 0);
}

NFTMintResult NFTMintingClient::parseNFTMintResponse(const nlohmann::json& response) {
    NFTMintResult result;
    
    // Parse the response from Node.js signing service
    result.success = response.value("success", false);
    result.item_name = response.value("item_name", "");
    result.uritoken_id = response.value("uritoken_id", "");
    result.transaction_hash = response.value("transaction_hash", "");
    result.metadata_uri = response.value("metadata_uri", "");
    result.engine_result = response.value("engine_result", "");
    result.error_message = response.value("error_message", "");
    result.validated = response.value("validated", false);
    result.engine_result_code = response.value("engine_result_code", 0);
    result.mint_timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    return result;
}

NFTMintResult NFTMintingClient::mintSingleNFT(const std::string& itemName, const std::string& metadataUri) {
    NFTMintResult result;
    result.item_name = itemName;
    result.success = false;
    
    try {
        log("Minting single NFT for item: " + itemName);
        log("Using metadata URI: " + metadataUri);
        
        if (!validateConfiguration()) {
            result.error_message = "Configuration validation failed";
            return result;
        }
        
        if (!isValidMetadataUri(metadataUri)) {
            result.error_message = "Invalid metadata URI: " + metadataUri;
            return result;
        }
        
        // Prepare request for Node.js signing service (simplified - no metadata_uri needed)
        nlohmann::json request = {
            {"account_seed", minterWalletSeed},
            {"item_name", itemName},
            {"flags", 1} // Burnable flag
        };
        
        // Call signing service /mint_nft endpoint
        nlohmann::json response = makeSigningServiceCall("/mint_nft", request);
        
        // Parse response
        result = parseNFTMintResponse(response);
        
        if (result.success) {
            log("✓ NFT minted successfully for " + itemName);
            log("  Transaction Hash: " + result.transaction_hash);
            log("  URITokenID: " + result.uritoken_id);
        } else {
            logError("✗ NFT minting failed for " + itemName + ": " + result.error_message);
        }
        
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("Minting failed: ") + e.what();
        logError("Exception during NFT minting: " + result.error_message);
    }
    
    return result;
}

NFTMintBatch NFTMintingClient::mintNFTsForGame(const std::string& gameId, const nlohmann::json& nftData) {
    NFTMintBatch batch;
    batch.success = true;
    batch.batch_timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    log("Starting NFT minting for game: " + gameId);
    
    // Check if already minted
    if (isAlreadyMinted(nftData)) {
        log("NFTs already minted for this game data");
        batch.success = true;
        batch.total_requested = 0;
        batch.successful_mints = 0;
        batch.failed_mints = 0;
        return batch;
    }
    
    // Parse inventory items
    std::string inventoryString = nftData.value("player_inventory", 
                                               nftData.value("inventory", "[]"));
    std::vector<std::string> items = parseInventoryItems(inventoryString);
    
    if (items.empty()) {
        log("No items found in inventory to mint");
        batch.total_requested = 0;
        batch.successful_mints = 0;
        batch.failed_mints = 0;
        return batch;
    }
    
    log("Found " + std::to_string(items.size()) + " items to mint");
    
    // Initialize batch counters
    batch.total_requested = items.size();
    batch.successful_mints = 0;
    batch.failed_mints = 0;
    
    // Prepare items for batch minting
    std::vector<nlohmann::json> batchItems;
    
    for (const std::string& itemName : items) {
        log("Processing item: " + itemName);
        
        // Add to batch items (simplified - signing service handles all asset processing)
        nlohmann::json batchItem = {
            {"item_name", itemName},
            {"flags", 1}
        };
        batchItems.push_back(batchItem);
    }
    
    if (!batchItems.empty()) {
        try {
            // Call batch minting endpoint
            nlohmann::json request = {
                {"account_seed", minterWalletSeed},
                {"items", batchItems}
            };
            
            log("Calling batch minting service for " + std::to_string(batchItems.size()) + " items");
            nlohmann::json response = makeSigningServiceCall("/mint_batch", request);
            
            // Parse batch response
            if (response.contains("results") && response["results"].is_array()) {
                for (const auto& resultJson : response["results"]) {
                    NFTMintResult result = parseNFTMintResponse(resultJson);
                    batch.results.push_back(result);
                    
                    if (result.success) {
                        batch.successful_mints++;
                        if (batch.first_success_hash.empty()) {
                            batch.first_success_hash = result.transaction_hash;
                        }
                    } else {
                        batch.failed_mints++;
                        batch.failed_items.push_back(result.item_name);
                        batch.success = false;
                    }
                }
            }
            
            // Update totals from service response if available
            if (response.contains("successful_mints")) {
                batch.successful_mints = response["successful_mints"];
            }
            if (response.contains("failed_mints")) {
                batch.failed_mints = response["failed_mints"];
            }
            
        } catch (const std::exception& e) {
            logError("Batch minting service call failed: " + std::string(e.what()));
            batch.success = false;
            
            // Mark all remaining items as failed
            for (const auto& batchItem : batchItems) {
                NFTMintResult result;
                result.item_name = batchItem["item_name"];
                result.success = false;
                result.error_message = "Batch service call failed: " + std::string(e.what());
                batch.results.push_back(result);
                batch.failed_items.push_back(result.item_name);
            }
            batch.failed_mints += batchItems.size();
        }
    }
    
    // Log final results
    if (batch.success) {
        log("✓ All NFTs minted successfully!");
        log("Total minted: " + std::to_string(batch.successful_mints));
    } else {
        log("✗ Batch minting completed with errors");
        log("Results: " + std::to_string(batch.successful_mints) + " successful, " + 
            std::to_string(batch.failed_mints) + " failed");
    }
    
    return batch;
}

NFTMintBatch NFTMintingClient::mintItemList(const std::vector<std::string>& itemNames) {
    // Create a simple inventory string and use existing logic
    std::ostringstream inventory;
    inventory << "[";
    for (size_t i = 0; i < itemNames.size(); ++i) {
        if (i > 0) inventory << ", ";
        inventory << itemNames[i];
    }
    inventory << "]";
    
    return mintInventoryString(inventory.str());
}

NFTMintBatch NFTMintingClient::mintInventoryString(const std::string& inventoryString) {
    // Create mock NFT data with the inventory string
    nlohmann::json nftData = {
        {"inventory", inventoryString},
        {"status", "pending"}
    };
    
    return mintNFTsForGame("direct_mint", nftData);
}

bool NFTMintingClient::isAlreadyMinted(const nlohmann::json& nftData) {
    // Check if status is already "minted"
    if (nftData.contains("status") && nftData["status"] == "minted") {
        return true;
    }
    
    // Check if nft_tokens array exists and is not empty
    if (nftData.contains("nft_tokens") && nftData["nft_tokens"].is_array() && !nftData["nft_tokens"].empty()) {
        return true;
    }
    
    return false;
}

bool NFTMintingClient::validateConfiguration() {
    if (minterWalletSeed.empty()) {
        logError("Minter wallet seed not configured");
        return false;
    }
    
    if (!std::filesystem::exists(nftMetadataDir)) {
        logError("Metadata directory not found: " + nftMetadataDir);
        return false;
    }
    
    return true;
}

bool NFTMintingClient::testConnection() {
    try {
        log("Testing connection to signing service...");
        
        // Use GET request for health check
        auto response = httpClient->Get("/health");
        
        if (!response) {
            logError("✗ Failed to connect to signing service at " + signingServiceUrl);
            return false;
        }
        
        if (response->status != 200) {
            logError("✗ Signing service error (" + std::to_string(response->status) + "): " + response->body);
            return false;
        }
        
        try {
            nlohmann::json health = nlohmann::json::parse(response->body);
            if (health.value("status", "") == "healthy") {
                log("✓ Connected to signing service successfully");
                return true;
            } else {
                logError("✗ Signing service reported unhealthy status");
                return false;
            }
        } catch (const nlohmann::json::parse_error& e) {
            logError("✗ Failed to parse health check response: " + std::string(e.what()));
            return false;
        }
        
    } catch (const std::exception& e) {
        logError("✗ Connection test failed: " + std::string(e.what()));
        return false;
    }
}

bool NFTMintingClient::validateWallet() {
    try {
        log("Validating wallet configuration...");
        
        if (!validateConfiguration()) {
            return false;
        }
        
        // Test wallet by getting account address
        std::string address = getAccountAddress();
        if (address.empty()) {
            logError("Failed to derive account address from seed");
            return false;
        }
        
        log("✓ Wallet validated successfully");
        log("Account address: " + address);
        return true;
        
    } catch (const std::exception& e) {
        logError("Wallet validation failed: " + std::string(e.what()));
        return false;
    }
}

std::string NFTMintingClient::getAccountAddress() {
    try {
        nlohmann::json request = {
            {"secret", minterWalletSeed}
        };
        
        nlohmann::json response = makeSigningServiceCall("/get_account_address", request);
        
        if (response.contains("address")) {
            return response["address"];
        }
        
        return "";
        
    } catch (const std::exception& e) {
        logError("Failed to get account address: " + std::string(e.what()));
        return "";
    }
}

nlohmann::json NFTMintingClient::getServiceHealth() {
    try {
        // Use GET request for health check
        auto response = httpClient->Get("/health");
        
        if (!response) {
            return nlohmann::json{
                {"status", "error"},
                {"error", "Failed to connect to signing service"}
            };
        }
        
        if (response->status != 200) {
            return nlohmann::json{
                {"status", "error"},
                {"error", "HTTP " + std::to_string(response->status) + ": " + response->body}
            };
        }
        
        return nlohmann::json::parse(response->body);
        
    } catch (const std::exception& e) {
        return nlohmann::json{
            {"status", "error"},
            {"error", e.what()}
        };
    }
}
