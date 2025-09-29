// AI Contract with Daemon Architecture - Solves 5-20 minute model loading delays
// Uses AIServiceClient instead of direct AI model loading

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <random>
#include <iomanip>
#include <sstream>
#include <memory>
#include <unordered_map>
#include <set>
#include <thread>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../httplib/httplib.h"
#include <openssl/sha.h>
#include "hotpocket_contract.h"
#include "ai_service_client.h"
#include "ai_jury_module.h"
#include "nft_minting_client.h"
#include <nlohmann/json.hpp>

// AI Model Downloader using cpp-httplib (kept for initial model setup)
class ModelDownloader
{
private:
    // const std::string fileName = "Llama-3.2-1B-Instruct-Q4_K_M.gguf";
    // const std::string expectedHash = "6f85a640a97cf2bf5b8e764087b1e83da0fdb51d7c9fab7d0fece9385611df83";
    // const size_t expectedSize = 807694464; // bytes (770.28 MB)
    // const std::string sourceUrl = "https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF/resolve/main/Llama-3.2-1B-Instruct-Q4_K_M.gguf";
    // const std::string fileName = "Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf";
    // const std::string expectedHash = "7b064f5842bf9532c91456deda288a1b672397a54fa729aa665952863033557c";
    // const size_t expectedSize = 4920739232; // bytes (770.28 MB)
    // const std::string sourceUrl = "https://huggingface.co/bartowski/Meta-Llama-3.1-8B-Instruct-GGUF/resolve/main/Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf";

    const std::string fileName = "gpt-oss-20b-Q5_K_M.gguf";
    const std::string expectedHash = "9c3814533c5b4c84d42b5dce4376bbdfd7227e990b8733a3a1c4f741355b3e75";
    const size_t expectedSize = 11717357248; // bytes (~11.3 GB)
    const std::string sourceUrl = "https://huggingface.co/unsloth/gpt-oss-20b-GGUF/resolve/main/gpt-oss-20b-Q5_K_M.gguf";

    //  const std::string fileName = "qwen3-8b-q4_k_m.gguf";
    // const std::string expectedHash = "609eb8a9fb256d0e2be8b8d252b00bae7c0496fac5e9ccca190206abbb24e2e5";
    // const size_t expectedSize = 5027783872;
    // const std::string sourceUrl = "https://huggingface.co/Aldaris/Qwen3-8B-Q4_K_M-GGUF/resolve/main/qwen3-8b-q4_k_m.gguf";

    const size_t chunkSize = 256 * 1024 * 1024; // 256 MiB

    size_t fileSize = 0;
    std::string modelFilePath;

public:
    std::string calculateSHA256(const std::string &filePath)
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("Cannot open file for hash calculation");
        }

        SHA256_CTX sha256;
        SHA256_Init(&sha256);

        char buffer[8192];
        while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0)
        {
            SHA256_Update(&sha256, buffer, file.gcount());
        }

        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256_Final(hash, &sha256);

        std::stringstream ss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
        }

        return ss.str();
    }

    bool downloadChunk(const std::string &url, const std::string &filePath, size_t startByte)
    {
        try
        {
            // Parse URL into components
            size_t schemePos = url.find("://");
            if (schemePos == std::string::npos)
            {
                std::cerr << "Invalid URL format" << std::endl;
                return false;
            }

            size_t hostStart = schemePos + 3;
            size_t pathStart = url.find("/", hostStart);

            if (pathStart == std::string::npos)
            {
                std::cerr << "Invalid URL: no path found" << std::endl;
                return false;
            }

            std::string host = url.substr(hostStart, pathStart - hostStart);
            std::string path = url.substr(pathStart);

            // Use httplib HTTPS client (with SSL support check)
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
            httplib::SSLClient cli(host);
#else
            httplib::Client cli(host); // Fallback to HTTP if SSL not available
#endif
            cli.set_follow_location(true);
            cli.set_connection_timeout(30); // 30 seconds
            cli.set_read_timeout(60);       // 60 seconds for large chunks

            // Calculate range for this chunk
            size_t remainingBytes = expectedSize - startByte;
            size_t actualChunkSize = std::min(chunkSize, remainingBytes);
            size_t endByte = startByte + actualChunkSize - 1;

            // Set range header for partial content
            httplib::Headers headers = {
                {"Range", "bytes=" + std::to_string(startByte) + "-" + std::to_string(endByte)},
                {"User-Agent", "HotPocket-AI-Contract/1.0"}};

            std::cout << "Downloading bytes " << startByte << "-" << endByte
                      << " (" << actualChunkSize << " bytes)" << std::endl;

            auto res = cli.Get(path, headers);

            if (!res)
            {
                std::cerr << "HTTP request failed" << std::endl;
                return false;
            }

            if (res->status != 206 && res->status != 200)
            { // 206 = Partial Content
                std::cerr << "HTTP error: " << res->status << std::endl;
                return false;
            }

            // Open file for writing (append mode)
            std::ofstream file(filePath, std::ios::binary | std::ios::app);
            if (!file)
            {
                std::cerr << "Cannot open file for writing: " << filePath << std::endl;
                return false;
            }

            file.write(res->body.c_str(), res->body.size());
            file.close();

            std::cout << "Downloaded " << res->body.size() << " bytes successfully" << std::endl;
            return true;
        }
        catch (const std::exception &e)
        {
            std::cerr << "Exception during download: " << e.what() << std::endl;
            return false;
        }
    }

    // Legacy-style chunked download pattern - called every contract execution
    bool ensureModelDownloaded()
    {
        std::string filePath = std::filesystem::path("../../../model") / fileName;

        // Create model directory if it doesn't exist
        std::filesystem::create_directories("../../../model");

        try
        {
            if (std::filesystem::exists(filePath))
            {
                fileSize = std::filesystem::file_size(filePath);
            }
            else
            {
                fileSize = 0;
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error checking file: " << e.what() << std::endl;
            std::cout << "Going to download model" << std::endl;
            return checkAndDownloadModel(filePath);
        }

        if (fileSize == expectedSize)
        {
            std::cout << "Model is downloaded" << std::endl;
            // Note: Set model path so daemon can use it
            modelFilePath = filePath;
            return true; // Model fully downloaded
        }
        else
        {
            std::cout << "Going to download model" << std::endl;
            return checkAndDownloadModel(filePath);
        }
    }

    // Legacy-style method - downloads ONE chunk per contract execution
    bool checkAndDownloadModel(const std::string &filePath)
    {
        std::cout << "Current file size: " << fileSize << " / " << expectedSize
                  << " (" << (double)fileSize / expectedSize * 100.0 << "%)" << std::endl;

        if (fileSize < expectedSize)
        {
            std::cout << "Downloading next chunk..." << std::endl;
            // Download one chunk
            if (!downloadChunk(sourceUrl, filePath, fileSize))
            {
                return false;
            }

            // Update file size after download
            try
            {
                fileSize = std::filesystem::file_size(filePath);
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error getting file size after download: " << e.what() << std::endl;
                return false;
            }

            std::cout << "Updated file size: " << fileSize << " / " << expectedSize
                      << " (" << (double)fileSize / expectedSize * 100.0 << "%)" << std::endl;

            if (fileSize >= expectedSize)
            {
                std::cout << "Download complete, verifying hash..." << std::endl;
                if (!expectedHash.empty())
                {
                    return verifyHashAndSetPath(filePath);
                }
                else
                {
                    modelFilePath = filePath;
                    return true;
                }
            }
            return false; // Partial download, need more chunks
        }

        return false;
    }

    bool verifyHashAndSetPath(const std::string &filePath)
    {
        std::string calculatedHash = calculateSHA256(filePath);

        if (calculatedHash == expectedHash)
        {
            std::cout << "Hash verification successful." << std::endl;
            modelFilePath = filePath;
            return true; // Complete and verified
        }
        else
        {
            std::cerr << "Hash mismatch. Expected: " << expectedHash
                      << ", Got: " << calculatedHash << std::endl;
            std::filesystem::remove(filePath);
            return false;
        }
    }

    std::string getModelPath() const
    {
        return modelFilePath;
    }

    size_t getExpectedSize() const
    {
        return expectedSize;
    }

    double getProgress() const
    {
        if (expectedSize == 0)
            return 0.0;
        return (double)fileSize / expectedSize * 100.0;
    }

    void setModelPath(const std::string &path)
    {
        modelFilePath = path;
    }
};

// Game State Manager (unchanged - handles file persistence)
class GameStateManager
{
private:
    std::string gameDataDir = "game_data";

public:
    GameStateManager()
    {
        std::filesystem::create_directories(gameDataDir);
    }

    std::string generateGameId(const std::string &userPrompt = "", const std::string &userIdHex = "")
    {
        std::vector<std::string> existingGames = listGames();
        int gameNumber = existingGames.size() + 1;

        std::string hashInput = userPrompt + userIdHex;
        std::hash<std::string> hasher;
        size_t hash = hasher(hashInput);

        return "game_" + std::to_string(gameNumber) + "_" + std::to_string(hash % 100000);
    }

    std::pair<std::string, std::string> separateGameContent(const std::string &fullGameContent)
    {
        try
        {
            // Parse narrative text to separate static world from dynamic state
            // Based on section markers generated by AI daemon

            std::istringstream stream(fullGameContent);
            std::string line;
            std::string worldContent = "";
            std::string stateContent = "";

            bool inWorldSection = false;
            bool inStateSection = false;

            while (std::getline(stream, line))
            {
                // Convert to lowercase for case-insensitive matching
                std::string lowerLine = line;
                std::transform(lowerLine.begin(), lowerLine.end(), lowerLine.begin(), ::tolower);

                // Detect WORLD sections (static content) - more precise matching
                if (lowerLine.find("game title:") != std::string::npos ||
                    lowerLine.find("world description:") != std::string::npos ||
                    lowerLine.find("world lore:") != std::string::npos ||
                    lowerLine.find("objectives:") != std::string::npos ||
                    lowerLine.find("win conditions:") != std::string::npos ||
                    lowerLine.find("game rules:") != std::string::npos)
                {
                    inWorldSection = true;
                    inStateSection = false;
                    worldContent += line + "\n";
                }
                // Detect STATE sections (dynamic content) - match AI daemon output exactly
                else if (lowerLine.find("current situation:") != std::string::npos ||
                         lowerLine.find("location:") != std::string::npos ||
                         lowerLine.find("starting status:") != std::string::npos)
                {
                    inWorldSection = false;
                    inStateSection = true;
                    stateContent += line + "\n";
                }
                // Continue adding to current section
                else if (inWorldSection)
                {
                    worldContent += line + "\n";
                }
                else if (inStateSection)
                {
                    stateContent += line + "\n";
                }
                // For unmatched content, default to world unless it's clearly state-like
                else if (!line.empty())
                {
                    // More sophisticated content classification
                    if (lowerLine.find("you are") != std::string::npos ||
                        lowerLine.find("you have") != std::string::npos ||
                        lowerLine.find("you find yourself") != std::string::npos ||
                        lowerLine.find("currently") != std::string::npos ||
                        lowerLine.find("health") != std::string::npos ||
                        lowerLine.find("inventory") != std::string::npos ||
                        lowerLine.find("score") != std::string::npos)
                    {
                        stateContent += line + "\n";
                    }
                    else
                    {
                        worldContent += line + "\n";
                    }
                }
            }

            // Ensure we always have meaningful state content (key fix!)
            if (stateContent.empty())
            {
                stateContent = std::string("Current Situation: You are just beginning your adventure.\n") +
                               "Location: Starting location\n" +
                               "Starting Status: You are ready to begin.\n";
            }

            // Ensure we have world content too
            if (worldContent.empty())
            {
                worldContent = fullGameContent;
            }

            std::cout << "=== DEBUG SEPARATION ===" << std::endl;
            std::cout << "World content length: " << worldContent.length() << std::endl;
            std::cout << "State content length: " << stateContent.length() << std::endl;
            std::cout << "State content preview: " << stateContent.substr(0, 200) << std::endl;

            return {worldContent, stateContent};
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error separating game content: " << e.what() << std::endl;
            // Provide meaningful fallback state
            std::string fallbackState = "Current Situation: Adventure begins...\nLocation: Starting area\nStarting Status: Ready to explore.\n";
            return {fullGameContent, fallbackState};
        }
    }

    bool saveGameWorld(const std::string &gameId, const std::string &gameWorld)
    {
        std::string filePath = gameDataDir + "/game_world_" + gameId + ".txt";
        std::ofstream file(filePath);
        if (file)
        {
            file << gameWorld;
            std::cout << "Game world saved: " << filePath << std::endl;
            return true;
        }
        return false;
    }

    bool saveGameState(const std::string &gameId, const std::string &gameState)
    {
        std::string filePath = gameDataDir + "/game_state_" + gameId + ".txt";
        std::ofstream file(filePath);
        if (file)
        {
            file << gameState;
            std::cout << "Game state saved: " << filePath << std::endl;
            return true;
        }
        return false;
    }

    std::string loadGameWorld(const std::string &gameId)
    {
        std::string filePath = gameDataDir + "/game_world_" + gameId + ".txt";
        std::ifstream file(filePath);
        if (file)
        {
            std::string content((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
            return content;
        }
        return "";
    }

    std::string loadGameState(const std::string &gameId)
    {
        std::string filePath = gameDataDir + "/game_state_" + gameId + ".txt";
        std::ifstream file(filePath);
        if (file)
        {
            std::string content((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
            return content;
        }
        return "";
    }

    std::vector<std::string> listGames()
    {
        std::vector<std::string> games;
        try
        {
            for (const auto &entry : std::filesystem::directory_iterator(gameDataDir))
            {
                if (entry.is_regular_file())
                {
                    std::string filename = entry.path().filename().string();
                    if (filename.substr(0, 11) == "game_world_" &&
                        filename.length() > 15 &&
                        filename.substr(filename.length() - 4) == ".txt")
                    {
                        std::string gameId = filename.substr(11);       // Remove "game_world_"
                        gameId = gameId.substr(0, gameId.length() - 4); // Remove ".txt"
                        games.push_back(gameId);
                    }
                }
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error listing games: " << e.what() << std::endl;
        }
        return games;
    }
};

// Game Engine Daemon Process Manager (renamed to avoid conflict with AI Jury module)
class GameEngineDaemonManager
{
private:
    pid_t gameEngineDaemonPid = -1;
    std::string gameEngineDaemonPath = "../../../AIDaemon"; // Persistent directory for daemon binary
    std::string gameEngineModelPath = "../../../model/gpt-oss-20b-Q5_K_M.gguf";
    std::string gameEnginePidFile = "../../../ai_daemon.pid";

    bool isDaemonProcessRunning(pid_t pid)
    {
        if (pid <= 0)
            return false;
        // Check if process exists by sending signal 0
        return kill(pid, 0) == 0;
    }

    bool ensureGameEngineDaemonBinaryExists()
    {
        // Check if daemon binary exists in persistent directory
        // Note: File copying is now handled by run_contract.sh script for better reliability
        if (std::filesystem::exists(gameEngineDaemonPath))
        {
            std::cout << "[Contract] Daemon binary found in persistent directory: " << gameEngineDaemonPath << std::endl;
            return true;
        }

        std::cerr << "[Contract] ERROR: Daemon binary not found in persistent directory: " << gameEngineDaemonPath << std::endl;
        std::cerr << "[Contract] Note: File copying should be handled by run_contract.sh script" << std::endl;
        return false;
    }

    pid_t getExistingGameEngineDaemonPid()
    {
        std::ifstream pidFileStream(gameEnginePidFile);
        if (!pidFileStream.is_open())
        {
            return -1;
        }

        pid_t pid;
        pidFileStream >> pid;
        pidFileStream.close();

        if (isDaemonProcessRunning(pid))
        {
            return pid;
        }
        else
        {
            // Remove stale PID file
            std::remove(gameEnginePidFile.c_str());
            return -1;
        }
    }

    void writeGameEnginePidFile(pid_t pid)
    {
        std::ofstream pidFileStream(gameEnginePidFile);
        if (pidFileStream.is_open())
        {
            pidFileStream << pid;
            pidFileStream.close();
        }
    }

    void cleanupUnresponsiveDaemon(pid_t pid)
    {
        std::cout << "[Contract] Cleaning up unresponsive daemon with PID: " << pid << std::endl;

        // CRITICAL FIX: Only cleanup if process is actually dead
        // Don't cleanup during model loading when daemon is busy but alive
        if (isDaemonProcessRunning(pid))
        {
            std::cout << "[Contract] WARNING: Daemon process " << pid << " is still running!" << std::endl;
            std::cout << "[Contract] This may be normal during model loading (8B model takes 30-60 seconds)" << std::endl;
            std::cout << "[Contract] NOT cleaning up socket - daemon may be loading model" << std::endl;
            return; // Don't cleanup if process is alive
        }

        std::cout << "[Contract] Process " << pid << " is confirmed dead - proceeding with cleanup" << std::endl;

        // Remove socket file only if process is actually dead
        std::string socketPath = "../../../ai_daemon.sock";
        if (std::filesystem::exists(socketPath))
        {
            std::cout << "[Contract] Removing stale socket file: " << socketPath << std::endl;
            unlink(socketPath.c_str());
        }

        // Try graceful shutdown first (in case process is zombied)
        if (kill(pid, SIGTERM) == 0)
        {
            std::cout << "[Contract] Sent SIGTERM to zombie process..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // Force kill if still somehow running
        if (isDaemonProcessRunning(pid))
        {
            std::cout << "[Contract] Force killing zombie process..." << std::endl;
            kill(pid, SIGKILL);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // Clean up PID file
        if (std::filesystem::exists(gameEnginePidFile))
        {
            std::cout << "[Contract] Removing stale PID file: " << gameEnginePidFile << std::endl;
            std::remove(gameEnginePidFile.c_str());
        }

        std::cout << "[Contract] Daemon cleanup complete" << std::endl;
    }

public:
    bool startDaemon()
    {
        std::cout << "[Contract] ========== Starting AI Daemon ==========" << std::endl;
        std::cout << "[Contract] Checking AI Daemon status..." << std::endl;
        std::cout << "[Contract] Current working directory: " << std::filesystem::current_path() << std::endl;
        std::cout << "[Contract] Expected daemon path: " << gameEngineDaemonPath << std::endl;
        std::cout << "[Contract] Expected model path: " << gameEngineModelPath << std::endl;
        std::cout << "[Contract] PID file path: " << gameEnginePidFile << std::endl;

        // Check if daemon binary exists
        if (!std::filesystem::exists(gameEngineDaemonPath))
        {
            std::cerr << "[Contract] ERROR: Daemon binary not found at: " << gameEngineDaemonPath << std::endl;
            return false;
        }
        else
        {
            std::cout << "[Contract] ✓ Daemon binary found at: " << gameEngineDaemonPath << std::endl;
        }

        // Check if model exists
        if (!std::filesystem::exists(gameEngineModelPath))
        {
            std::cerr << "[Contract] ERROR: Model file not found at: " << gameEngineModelPath << std::endl;
            return false;
        }
        else
        {
            auto model_size = std::filesystem::file_size(gameEngineModelPath);
            std::cout << "[Contract] ✓ Model file found: " << gameEngineModelPath << " (" << (model_size / 1024.0 / 1024.0) << " MB)" << std::endl;
        }

        // First check if we have a persistent daemon running from previous rounds
        pid_t existingPid = getExistingGameEngineDaemonPid();
        if (existingPid > 0)
        {
            std::cout << "[Contract] Found existing daemon with PID: " << existingPid << std::endl;

            // Check if the process is actually running before trying to connect
            if (!isDaemonProcessRunning(existingPid))
            {
                std::cout << "[Contract] Process " << existingPid << " is not running - cleaning up stale PID file" << std::endl;
                unlink(gameEnginePidFile.c_str()); // Remove stale PID file
            }
            else
            {
                std::cout << "[Contract] Process " << existingPid << " is running - using existing daemon" << std::endl;
                std::cout << "[Contract] Note: Daemon may be loading model, which can take 10+ minutes" << std::endl;
                gameEngineDaemonPid = existingPid;
                return true; // Just use the existing daemon, don't test responsiveness
            }
        }

        // Check if daemon is already running via socket (backup check)
        std::cout << "[Contract] Checking for existing daemon via socket..." << std::endl;
        AIServiceClient client;
        if (client.isDaemonRunning())
        {
            std::cout << "[Contract] Daemon already running via socket - using existing daemon" << std::endl;
            return true;
        }

        std::cout << "[Contract] No daemon found - starting new daemon..." << std::endl;

        // Ensure daemon binary exists in persistent directory
        if (!ensureGameEngineDaemonBinaryExists())
        {
            std::cerr << "[Contract] Failed to ensure daemon binary exists" << std::endl;
            return false;
        }

        std::cout << "[Contract] Forking daemon process..." << std::endl;
        std::cout.flush();

        // Fork and exec the daemon
        gameEngineDaemonPid = fork();
        if (gameEngineDaemonPid == 0)
        {
            // Child process - exec the daemon
            std::cout << "[Daemon Child] Executing daemon: " << gameEngineDaemonPath << " " << gameEngineModelPath << std::endl;
            std::cout.flush();

            execl(gameEngineDaemonPath.c_str(), "AIDaemon", gameEngineModelPath.c_str(), (char *)nullptr);
            std::cerr << "[Daemon Child] FATAL: Failed to exec daemon: " << strerror(errno) << std::endl;
            exit(1);
        }
        else if (gameEngineDaemonPid > 0)
        {
            // Parent process - write PID file immediately
            writeGameEnginePidFile(gameEngineDaemonPid);
            std::cout << "[Contract] Daemon started with PID: " << gameEngineDaemonPid << " (saved to " << gameEnginePidFile << ")" << std::endl;

            // Give daemon a moment to start (non-blocking)
            std::cout << "[Contract] Waiting 500ms for daemon to initialize..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // Check if daemon process is still running (just process check, not socket)
            if (isDaemonProcessRunning(gameEngineDaemonPid))
            {
                std::cout << "[Contract] ✓ Daemon process started successfully (PID: " << gameEngineDaemonPid << ")" << std::endl;
                std::cout << "[Contract] Note: Socket may take additional time to become available during model loading" << std::endl;
                return true;
            }
            else
            {
                std::cerr << "[Contract] ERROR: Daemon process failed to start or crashed immediately" << std::endl;
                std::remove(gameEnginePidFile.c_str());
                return false;
            }
        }
        else
        {
            std::cerr << "[Contract] FATAL: Failed to fork daemon process: " << strerror(errno) << std::endl;
            return false;
        }
    }

    void stopDaemon()
    {
        if (gameEngineDaemonPid > 0)
        {
            std::cout << "[Contract] Stopping daemon with PID: " << gameEngineDaemonPid << std::endl;
            kill(gameEngineDaemonPid, SIGTERM);

            // Wait for graceful shutdown
            int status;
            pid_t result = waitpid(gameEngineDaemonPid, &status, WNOHANG);
            if (result == 0)
            {
                // Still running, force kill
                std::this_thread::sleep_for(std::chrono::seconds(2));
                kill(gameEngineDaemonPid, SIGKILL);
                waitpid(gameEngineDaemonPid, &status, 0);
            }

            // Clean up PID file
            std::remove(gameEnginePidFile.c_str());
            gameEngineDaemonPid = -1;
            std::cout << "[Contract] Daemon stopped and PID file removed" << std::endl;
        }
    }

    // Optional method to force stop daemon (for maintenance/debugging)
    void forceStopDaemon()
    {
        std::cout << "[Contract] Force stopping daemon for maintenance..." << std::endl;
        stopDaemon();
    }

    ~GameEngineDaemonManager()
    {
        // DO NOT stop daemon in destructor - let it persist across HotPocket rounds
        // The daemon should remain running between contract executions
        std::cout << "[Contract] Contract round ending - daemon remains running for next round" << std::endl;
    }
};

// Game Action State for AI Jury validation - Legacy voting fields removed
struct GameActionState
{
    const struct hp_user *user;
    std::string gameId;
    std::string action;
    std::string playerAction; // Store the player action for validation
    std::string oldGameState; // Store old state for validation
    std::string newGameState; // Store new state for validation
    std::string gameWorld;    // Store game world for validation
    bool continue_conversation = false; // Store conversation continuity flag

    int action_idx; // Action index for consensus tracking
};

// Valuable Item Extraction for NFT Generation
class ValuableItemExtractor
{
private:
    std::string nftDataDir = "game_data";

public:
    ValuableItemExtractor()
    {
        std::filesystem::create_directories(nftDataDir);
    }

    // Extract player inventory from won game state for NFT generation
    bool extractPlayerInventory(const std::string& gameId, const std::string& gameState, const std::string& playerAction)
    {
        try
        {
            std::cout << "[NFT] Extracting player inventory from won game: " << gameId << std::endl;
            
            // Create NFT data with basic game completion info
            nlohmann::json nftData;
            nftData["game_id"] = gameId;
            nftData["completion_time"] = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            nftData["winning_action"] = playerAction;
            nftData["status"] = "won";
            
            // Extract structured player data from game state
            std::string playerLocation = extractField(gameState, "Player_Location:");
            std::string playerHealth = extractField(gameState, "Player_Health:");
            std::string playerScore = extractField(gameState, "Player_Score:");
            std::string playerInventory = extractField(gameState, "Player_Inventory:");
            
            // Store raw player data for NFT
            nftData["final_location"] = playerLocation;
            nftData["final_health"] = playerHealth;
            nftData["final_score"] = playerScore;
            nftData["player_inventory"] = playerInventory; // Raw inventory - whatever the player has
            
            std::cout << "[NFT] Player inventory extracted: " << playerInventory << std::endl;
            
            // Save to NFT data file (simplified naming without timestamp)
            std::string nftFilePath = nftDataDir + "/nft_" + gameId + ".json";
            std::ofstream nftFile(nftFilePath);
            if (nftFile)
            {
                nftFile << nftData.dump(2);
                std::cout << "[NFT] Player inventory data saved: " << nftFilePath << std::endl;
                return true;
            }
            else
            {
                std::cerr << "[NFT] Failed to save NFT data" << std::endl;
                return false;
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "[NFT] Error extracting player inventory: " << e.what() << std::endl;
            return false;
        }
    }

private:
    // Extract field value from game state text
    std::string extractField(const std::string& gameState, const std::string& fieldName)
    {
        size_t pos = gameState.find(fieldName);
        if (pos == std::string::npos) return "";
        
        // Move to start of value (after field name)
        pos += fieldName.length();
        
        // Skip whitespace
        while (pos < gameState.length() && (gameState[pos] == ' ' || gameState[pos] == '\t')) pos++;
        
        // Extract until newline
        size_t endPos = gameState.find('\n', pos);
        if (endPos == std::string::npos) endPos = gameState.length();
        
        std::string value = gameState.substr(pos, endPos - pos);
        
        // Trim trailing whitespace
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
            value.pop_back();
        }
        
        return value;
    }
};

// Global state
static std::unique_ptr<AIServiceClient> g_aiClient;
static std::unique_ptr<GameStateManager> g_gameManager;
static std::unique_ptr<ModelDownloader> g_modelDownloader;
static std::unique_ptr<GameEngineDaemonManager> g_gameEngineDaemonManager;
static std::vector<std::unique_ptr<GameActionState>> g_gameActionHandlers;
static std::unique_ptr<AIJury::AIJuryModule> g_aiJury;
static std::unique_ptr<ValuableItemExtractor> g_valuableItemExtractor;
static std::unique_ptr<NFTMintingClient> g_nftMintingClient;

// Conversation continuity state tracking
static std::unordered_map<std::string, bool> g_gameConversationActive; // gameId -> conversation active flag
static std::unordered_map<std::string, int> g_gameActionCount; // gameId -> action count for this conversation

// COMMENTED OUT NFT CONSENSUS COORDINATION - IMPLEMENTING READ-ONLY MODE ONLY
// NFT Coordination System (completely separate from AI Jury)
// static std::unordered_map<std::string, bool> g_nftCoordinationInProgress; // gameId -> NFT coordination in progress
// static std::unordered_map<std::string, nlohmann::json> g_nftCoordinationResults; // gameId -> NFT coordination results  
// static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_nftCoordinationStartTime; // gameId -> start time

// LEGACY VOTING SYSTEM REMOVED - Only AI Jury validation is used now


// Helper function declarations
std::string escapeJsonForOutput(const std::string &str);
std::string cleanJsonResponse(const std::string &response);

// Message processing functions for AI-validated game actions
void process_stat_message(const struct hp_user *user);
void process_game_message(const struct hp_user *user, const std::string &action, const std::string &data, int action_idx, int peer_count);
void waitForGameConsensus(int action_idx, int peer_count);

// AI Jury integration functions
void juryNPLBroadcast(const std::string &msg);
void juryUserResponse(const hp_user *user, const std::string &response);
void process_jury_vote(const std::string &voteJson, int peer_count);
void waitForJuryConsensus(int request_idx, int peer_count);

// COMMENTED OUT NFT CONSENSUS COORDINATION FUNCTIONS - IMPLEMENTING READ-ONLY MODE ONLY
// NFT Coordination System functions (completely separate from AI Jury)
// std::string selectDeterministicMinter(const std::string &gameId, int peer_count);
// void processNFTMintingRequest(const struct hp_user *user, const std::string &gameId, int peer_count);
// void waitForNFTConsensus(const std::string &gameId, int peer_count);
// void processNFTCoordinationMessage(const std::string &nplMessage, const char *sender);

// Shared NFT File Update Function - used by both minter and non-minter nodes
bool updateNFTFileWithMintingResults(const std::string &gameId, const nlohmann::json &mintingResults)
{
    std::string nftFilePath = "game_data/nft_" + gameId + ".json";
    
    // Load existing NFT file
    std::ifstream nftFile(nftFilePath);
    if (!nftFile) {
        std::cout << "[NFT] Error: NFT data file not found: " << nftFilePath << std::endl;
        return false;
    }
    
    std::string nftContent((std::istreambuf_iterator<char>(nftFile)),
                           std::istreambuf_iterator<char>());
    nftFile.close();
    
    try {
        nlohmann::json nftData = nlohmann::json::parse(nftContent);
        
        // Update status to minted
        nftData["status"] = "minted";
        
        // Update mint timestamp (preserve original type)
        if (mintingResults.contains("mint_timestamp")) {
            nftData["mint_timestamp"] = mintingResults["mint_timestamp"];
        }
        
        // Update transaction hash (handle both field names for compatibility)
        if (mintingResults.contains("mint_tx_hash")) {
            nftData["mint_tx_hash"] = mintingResults["mint_tx_hash"];
        } else if (mintingResults.contains("batch_tx_hash")) {
            nftData["mint_tx_hash"] = mintingResults["batch_tx_hash"];
        }
        
        // Update NFT tokens array (handle both field names for compatibility)
        if (mintingResults.contains("nft_tokens")) {
            nftData["nft_tokens"] = mintingResults["nft_tokens"];
        } else if (mintingResults.contains("minted_items")) {
            nftData["nft_tokens"] = mintingResults["minted_items"];
        }
        
        // Save updated NFT data file
        std::ofstream updatedFile(nftFilePath);
        if (updatedFile) {
            updatedFile << nftData.dump(2);
            updatedFile.close();
            std::cout << "[NFT] Successfully updated NFT data file: " << nftFilePath << std::endl;
            return true;
        } else {
            std::cout << "[NFT] Error: Failed to write to NFT data file: " << nftFilePath << std::endl;
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cout << "[NFT] Error updating NFT data file: " << e.what() << std::endl;
        return false;
    }
}

// Helper function to escape JSON strings for output
std::string escapeJsonForOutput(const std::string &str)
{
    std::string escaped;
    for (char c : str)
    {
        switch (c)
        {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += c;
            break;
        }
    }
    return escaped;
}

// Helper function to clean AI-generated JSON responses
std::string cleanJsonResponse(const std::string &response)
{
    std::string cleaned = response;

    // Remove markdown code blocks
    size_t start = cleaned.find("```json");
    if (start != std::string::npos)
    {
        start += 7; // Length of "```json"
        size_t end = cleaned.find("```", start);
        if (end != std::string::npos)
        {
            cleaned = cleaned.substr(start, end - start);
        }
    }

    // Remove any text before the first {
    size_t jsonStart = cleaned.find('{');
    if (jsonStart != std::string::npos)
    {
        cleaned = cleaned.substr(jsonStart);
    }

    // Remove any text after the last }
    size_t jsonEnd = cleaned.rfind('}');
    if (jsonEnd != std::string::npos)
    {
        cleaned = cleaned.substr(0, jsonEnd + 1);
    }

    // Remove trailing commas before closing braces/brackets
    size_t pos = 0;
    while ((pos = cleaned.find(",}", pos)) != std::string::npos)
    {
        cleaned.erase(pos, 1);
    }
    pos = 0;
    while ((pos = cleaned.find(",]", pos)) != std::string::npos)
    {
        cleaned.erase(pos, 1);
    }

    return cleaned;
}

void process_stat_message(const struct hp_user *user)
{
    std::string response = "{\"type\":\"stats\"";

    if (g_modelDownloader)
    {
        response += ",\"model_progress\":" + std::to_string(g_modelDownloader->getProgress());
        response += ",\"model_path\":\"" + escapeJsonForOutput(g_modelDownloader->getModelPath()) + "\"";
    }

    if (g_aiClient)
    {
        bool daemon_running = g_aiClient->isDaemonRunning();
        bool model_ready = g_aiClient->isModelReady();
        response += ",\"daemon_status\":\"" + std::string(daemon_running ? "running" : "stopped") + "\"";
        response += ",\"model_ready\":" + std::string(model_ready ? "true" : "false");

        // Get detailed daemon status if running
        if (daemon_running)
        {
            std::string detailed_status = g_aiClient->getDaemonStatus();
            response += ",\"daemon_details\":" + detailed_status;
        }
    }

    if (g_gameManager)
    {
        std::vector<std::string> games = g_gameManager->listGames();
        response += ",\"total_games\":" + std::to_string(games.size());
    }

    response += "}";
    hp_write_user_msg(user, response.c_str(), response.length());
}

void process_game_message(const struct hp_user *user, const std::string &action, const std::string &data, int action_idx, int peer_count)
{
    std::cout << "=== PROCESS_GAME_MESSAGE (Daemon-Based) ===" << std::endl;
    std::cout << "Action: " << action << std::endl;
    std::cout << "Data: " << data << std::endl;
    std::cout << "Action Index: " << action_idx << std::endl;
    std::cout << "Peer Count: " << peer_count << std::endl;

    if (!g_aiClient || !g_gameManager)
    {
        std::string error = "{\"type\":\"error\",\"error\":\"Game systems not initialized\"}";
        std::cout << "ERROR: Game systems not initialized!" << std::endl;
        hp_write_user_msg(user, error.c_str(), error.length());
        return;
    }

    // Check daemon connectivity
    if (!g_aiClient->isDaemonRunning())
    {
        std::string error = "{\"type\":\"error\",\"error\":\"AI Daemon not running\"}";
        std::cout << "ERROR: AI Daemon not running!" << std::endl;
        hp_write_user_msg(user, error.c_str(), error.length());
        return;
    }

    // Check if model is ready (for actions that require AI)
    bool model_ready = g_aiClient->isModelReady();
    if (!model_ready && (action == "create_game" || action == "player_action"))
    {
        std::string error = "{\"type\":\"error\",\"error\":\"AI model still loading, please try again in a few minutes\"}";
        std::cout << "INFO: AI model still loading, skipping " << action << std::endl;
        hp_write_user_msg(user, error.c_str(), error.length());
        return;
    }

    // Only player_action requires voting consensus - all other actions are immediate
    if (action != "player_action")
    {
        std::cout << "Action '" << action << "' does not require voting - processing immediately..." << std::endl;
    }

    // Create game action state
    auto state = std::make_unique<GameActionState>();
    state->user = user;
    state->action = action;

    std::string playerActionText;
    bool continue_conversation = false;
    std::string oldGameState;
    std::string newGameState;
    std::string gameWorld;

    if (action == "create_game")
    {
        // Create new game from user prompt - NO VOTING NEEDED
        std::cout << "=== CREATE_GAME (No Voting - Daemon-Based) ===" << std::endl;
        std::string aiResponse = g_aiClient->createGame(data);

        std::cout << "AI Response Length: " << aiResponse.length() << std::endl;
        std::cout << "AI Response (first 200 chars): " << aiResponse.substr(0, 200) << std::endl;
        std::cout << "AI Response (last 200 chars): " << aiResponse.substr(std::max(0, (int)aiResponse.length() - 200)) << std::endl;

        // Simple error checking - only check for empty response
        if (!aiResponse.empty())
        {
            // Generate deterministic game ID using user prompt for hash
            std::string gameId = g_gameManager->generateGameId(data, "");
            std::cout << "Generated Game ID: " << gameId << std::endl;

            // Separate game world from game state
            auto [gameWorldContent, gameStateContent] = g_gameManager->separateGameContent(aiResponse);

            // Save game files directly (no validation needed for creation)
            if (g_gameManager->saveGameWorld(gameId, gameWorldContent) &&
                g_gameManager->saveGameState(gameId, gameStateContent))
            {

                std::cout << "Game created and saved successfully!" << std::endl;

                // Send immediate response - no consensus needed
                std::string result = "{\"type\":\"gameCreated\",\"game_id\":\"" + escapeJsonForOutput(gameId) + "\",\"status\":\"success\"}";
                std::cout << "Sending response: " << result << std::endl;
                int bytes_written = hp_write_user_msg(user, result.c_str(), result.length());
                std::cout << "hp_write_user_msg returned: " << bytes_written << " bytes" << std::endl;
                std::cout << "Response sent to client immediately!" << std::endl;
                return; // Exit early - no voting needed
            }
            else
            {
                std::cout << "ERROR: Failed to save game files!" << std::endl;
                std::string error = "{\"type\":\"error\",\"error\":\"Failed to save game data\"}";
                hp_write_user_msg(user, error.c_str(), error.length());
                return;
            }
        }
        else
        {
            std::cout << "ERROR: AI Daemon failed to generate game content!" << std::endl;
            std::string error = "{\"type\":\"error\",\"error\":\"Failed to generate game content\"}";
            hp_write_user_msg(user, error.c_str(), error.length());
            return;
        }
    }
    else if (action == "player_action")
    {
        // Extract game_id from data (format: "game_id:action_text")
        size_t colonPos = data.find(":");
        if (colonPos == std::string::npos)
        {
            // Set context even for format errors (no validation needed here)
            state->gameId = "";
            state->playerAction = data;
            state->oldGameState = "";
            state->newGameState = "";
            state->gameWorld = "";
        }
        else
        {
            // std::string gameId = data.substr(0, colonPos);
            // playerActionText = data.substr(colonPos + 1);
            // continue_conversation = data.substr(colonPos + 2);
            std::string gameId = data.substr(0, colonPos);
            size_t secondColonPos = data.find(":", colonPos + 1);
            if (secondColonPos != std::string::npos) {
                playerActionText = data.substr(colonPos + 1, secondColonPos - colonPos - 1);
                std::string continueStr = data.substr(secondColonPos + 1);
                
                // Debug output to see what we're parsing
                std::cout << "[DEBUG] Parsing three-part format:" << std::endl;
                std::cout << "[DEBUG] Raw data: '" << data << "'" << std::endl;
                std::cout << "[DEBUG] Game ID: '" << gameId << "'" << std::endl;
                std::cout << "[DEBUG] Player Action: '" << playerActionText << "'" << std::endl;
                std::cout << "[DEBUG] Continue String: '" << continueStr << "'" << std::endl;
                
                continue_conversation = (continueStr == "true" || continueStr == "1");
                std::cout << "[DEBUG] Continue Conversation Result: " << (continue_conversation ? "true" : "false") << std::endl;
            } else {
                playerActionText = data.substr(colonPos + 1);
                continue_conversation = false; // default for two-part format
                std::cout << "[DEBUG] Using two-part format, continue_conversation = false" << std::endl;
            }
            // Load existing game state AND game world
            oldGameState = g_gameManager->loadGameState(gameId);
            gameWorld = g_gameManager->loadGameWorld(gameId);

            if (oldGameState.empty() || gameWorld.empty())
            {
                // Set context even for missing game errors
                state->gameId = gameId;
                state->playerAction = playerActionText;
                state->continue_conversation = continue_conversation;
                state->oldGameState = oldGameState;
                state->newGameState = oldGameState; // Keep old state
                state->gameWorld = gameWorld;
            }
            else
            {
                // Preview client request parameters
                std::cout << "\n=== AI SERVICE CLIENT REQUEST PREVIEW ===" << std::endl;
                std::cout << "Game ID: " << gameId << std::endl;
                std::cout << "Player Action: " << playerActionText << std::endl;
                std::cout << "Continue Conversation: " << (continue_conversation ? "true" : "false") << std::endl;
                std::cout << "Old Game State Length: " << oldGameState.length() << " chars" << std::endl;
                std::cout << "Game World Length: " << gameWorld.length() << " chars" << std::endl;
                std::cout << "========================================\n" << std::endl;

                // Process player action with AI Daemon to get new state
                std::string actionResult = g_aiClient->processPlayerAction(gameId, playerActionText, oldGameState, gameWorld, continue_conversation);

                // Always set the context data for voting, regardless of action result
                state->gameId = gameId;
                state->playerAction = playerActionText;
                state->oldGameState = oldGameState;
                state->gameWorld = gameWorld;

                // Check for error indicators in the text response
                bool isErrorResponse = false;
                std::string lowerResponse = actionResult;
                std::transform(lowerResponse.begin(), lowerResponse.end(), lowerResponse.begin(), ::tolower);

                // Look for common error indicators in the text
                if (lowerResponse.find("error:") != std::string::npos ||
                    lowerResponse.find("failed") != std::string::npos ||
                    lowerResponse.find("invalid") != std::string::npos ||
                    lowerResponse.find("cannot") != std::string::npos ||
                    actionResult.empty())
                {
                    isErrorResponse = true;
                }

                if (!actionResult.empty() && !isErrorResponse)
                {
                    newGameState = actionResult;
                    state->newGameState = newGameState;

                    // Save new state (validation will be handled by AI Jury consensus)
                    if (!g_gameManager->saveGameState(gameId, newGameState))
                    {
                        // File save failed - this will be handled in consensus
                        std::cout << "WARNING: Failed to save game state during processing" << std::endl;
                    }
                }
                else
                {
                    // Action processing failed - set old state as new state for consensus
                    state->newGameState = oldGameState;
                }
            }
        }
    }
    else if (action == "list_games")
    {
        // List available games - NO VOTING NEEDED (read-only operation)
        std::cout << "=== LIST_GAMES (No Voting) ===" << std::endl;
        std::vector<std::string> games = g_gameManager->listGames();
        std::string gamesList = "[";
        for (size_t i = 0; i < games.size(); ++i)
        {
            gamesList += "\"" + games[i] + "\"";
            if (i < games.size() - 1)
                gamesList += ",";
        }
        gamesList += "]";

        // Send immediate response - no consensus needed
        std::string result = "{\"type\":\"gamesList\",\"games\":" + gamesList + "}";
        hp_write_user_msg(user, result.c_str(), result.length());
        return; // Exit early - no voting needed
    }
    else if (action == "get_game_state")
    {
        // Get game state - NO VOTING NEEDED (read-only operation)
        std::cout << "=== GET_GAME_STATE (No Voting) ===" << std::endl;
        std::string gameState = g_gameManager->loadGameState(data);

        if (!gameState.empty())
        {
            std::string result = "{\"type\":\"gameState\",\"game_id\":\"" + escapeJsonForOutput(data) +
                                 "\",\"state\":\"" + escapeJsonForOutput(gameState) + "\"}";
            hp_write_user_msg(user, result.c_str(), result.length());
        }
        else
        {
            std::string error = "{\"type\":\"error\",\"error\":\"Game not found\"}";
            hp_write_user_msg(user, error.c_str(), error.length());
        }
        return; // Exit early - no voting needed
    }
    else if (action == "mint_nft")
    {
        // NFT Minting in Read-Only Mode (Consensus Coordination Disabled)
        std::cout << "=== MINT_NFT (READ-ONLY MODE) ===" << std::endl;
        
        const struct hp_contract_context *ctx = hp_get_context();
        if (!ctx) {
            std::string error = "{\"type\":\"error\",\"error\":\"Contract context not available\"}";
            hp_write_user_msg(user, error.c_str(), error.length());
            return;
        }
        
        // Check if this is a read-only context (HotPocket read request)
        if (!ctx->readonly) {
            std::string error = "{\"type\":\"error\",\"error\":\"NFT minting is temporarily disabled - only read-only mode supported\"}";
            hp_write_user_msg(user, error.c_str(), error.length());
            return;
        }
        
        std::cout << "[NFT] Running in read-only mode - performing NFT minting without consensus coordination" << std::endl;
        
        if (!g_nftMintingClient) {
            std::string error = "{\"type\":\"error\",\"error\":\"NFT minting client not initialized\"}";
            hp_write_user_msg(user, error.c_str(), error.length());
            return;
        }
        
        // Check if NFT data file exists
        std::string nftFilePath = "game_data/nft_" + data + ".json";
        std::ifstream nftFile(nftFilePath);
        if (!nftFile) {
            std::string error = "{\"type\":\"error\",\"error\":\"NFT data file not found for game: " + data + "\"}";
            hp_write_user_msg(user, error.c_str(), error.length());
            return;
        }
        
        std::string nftContent((std::istreambuf_iterator<char>(nftFile)),
                               std::istreambuf_iterator<char>());
        nftFile.close();
        
        try {
            nlohmann::json nftData = nlohmann::json::parse(nftContent);
            
            // Check if already minted
            if (g_nftMintingClient->isAlreadyMinted(nftData)) {
                nlohmann::json alreadyMintedResult;
                alreadyMintedResult["type"] = "nft_mint_result";
                alreadyMintedResult["game_id"] = data;
                alreadyMintedResult["success"] = true;
                alreadyMintedResult["already_minted"] = true;
                alreadyMintedResult["message"] = "NFTs already minted for this game";
                alreadyMintedResult["readonly_mode"] = true;
                
                hp_write_user_msg(user, alreadyMintedResult.dump().c_str(), alreadyMintedResult.dump().length());
                return;
            }
            
            // Perform actual NFT minting in read-only mode
            std::cout << "[NFT] Starting NFT minting in read-only mode for game: " << data << std::endl;
            NFTMintBatch mintResult = g_nftMintingClient->mintNFTsForGame(data, nftData);
            std::cout << "[NFT] Minting result: " << mintResult.success << std::endl;

            // Prepare result for direct response (no NPL broadcast in read-only mode)
            nlohmann::json result;
            result["type"] = "nft_mint_result";
            result["game_id"] = data;
            result["success"] = mintResult.success;
            result["readonly_mode"] = true;
            result["mint_timestamp"] = mintResult.batch_timestamp;
            result["total_requested"] = mintResult.total_requested;
            result["successful_mints"] = mintResult.successful_mints;
            result["failed_mints"] = mintResult.failed_mints;
            
            if (mintResult.success) {
                result["batch_tx_hash"] = mintResult.first_success_hash;
                
                nlohmann::json mintedItems = nlohmann::json::array();
                for (const auto& mintedResult : mintResult.results) {
                    nlohmann::json item;
                    item["name"] = mintedResult.item_name;
                    item["nft_token_id"] = mintedResult.uritoken_id;
                    item["transaction_hash"] = mintedResult.transaction_hash;
                    item["metadata_uri"] = mintedResult.metadata_uri;
                    mintedItems.push_back(item);
                }
                result["minted_items"] = mintedItems;
            } else {
                result["error"] = "Some NFTs failed to mint";
                
                nlohmann::json failedItems = nlohmann::json::array();
                for (const auto& mintedResult : mintResult.results) {
                    if (!mintedResult.success) {
                        nlohmann::json item;
                        item["name"] = mintedResult.item_name;
                        item["error"] = mintedResult.error_message;
                        failedItems.push_back(item);
                    }
                }
                result["failed_items"] = failedItems;
            }

            std::cout << "[NFT] Read-only minting completed" << result.dump() << std::endl;
            hp_write_user_msg(user, result.dump().c_str(), result.dump().length());
        } catch (const std::exception& e) {
            nlohmann::json errorResult;
            errorResult["type"] = "nft_mint_result";
            errorResult["game_id"] = data;
            errorResult["success"] = false;
            errorResult["readonly_mode"] = true;
            errorResult["error"] = "Failed to parse NFT data: " + std::string(e.what());
            
            hp_write_user_msg(user, errorResult.dump().c_str(), errorResult.dump().length());
        }
        
        return; // Exit early - no consensus needed
    }
    else
    {
        // Unknown action
        std::string error = "{\"type\":\"error\",\"error\":\"Unknown action: " + action + "\"}";
        hp_write_user_msg(user, error.c_str(), error.length());
        return;
    }

    // For player_action: Add to handlers for consensus voting
    state->action_idx = action_idx;

    // CRITICAL FIX: Add the missing voting mechanism from legacy contract
    std::cout << "=== STARTING AI JURY VALIDATION PROCESS ===" << std::endl;
    std::cout << "Action: " << action << " requires consensus validation" << std::endl;

    // Use AI Jury for validation instead of direct daemon
    gameWorld = g_gameManager->loadGameWorld(state->gameId);
    std::string transitionContext = "GameWorld: " + gameWorld + " -> OldState: " + oldGameState + " -> PlayerAction: " + playerActionText + " -> NewState: " + newGameState;

    // std::string transitionContext = "Old: " + oldGameState + " -> Action: " + playerActionText + " -> New: " + newGameState;
    g_aiJury->processRequest(user, "validate_game_action", transitionContext, action_idx, peer_count, "game_engine_context");

    // Store state for consensus BEFORE waiting (like legacy contract)
    g_gameActionHandlers.push_back(std::move(state));

    // Wait for AI Jury consensus
    waitForJuryConsensus(action_idx, peer_count);
}

// LEGACY VOTING SYSTEM REMOVED - Only AI Jury validation is used now

// AI Jury NPL broadcast callback
void juryNPLBroadcast(const std::string &msg)
{
    hp_write_npl_msg(msg.c_str(), msg.length());
}

// Enhanced AI Jury user response callback that includes game state information
void juryUserResponse(const hp_user *user, const std::string &response)
{
    try
    {
        // Parse the AI Jury consensus response
        nlohmann::json juryResponse = nlohmann::json::parse(response);

        std::cout << "[GameEngine] Processing jury response: " << response << std::endl;

        // Check if this is a consensus response for game action validation
        if (juryResponse.contains("type") && juryResponse["type"] == "consensus")
        {

            // The actual consensus details might be in a "details" field as a JSON string
            nlohmann::json consensusDetails;
            if (juryResponse.contains("details"))
            {
                try
                {
                    consensusDetails = nlohmann::json::parse(juryResponse["details"].get<std::string>());
                }
                catch (const std::exception &e)
                {
                    std::cout << "[GameEngine] Failed to parse details field: " << e.what() << std::endl;
                    consensusDetails = juryResponse;
                }
            }
            else
            {
                consensusDetails = juryResponse;
            }

            // Check if this is a game action validation
            if (consensusDetails.contains("messageType") && consensusDetails["messageType"] == "validate_game_action")
            {

                // Find the corresponding game action state to get the game state
                int requestId = consensusDetails.value("requestId", -1);
                bool validAction = consensusDetails.value("decision", "invalid") == "valid";

                std::cout << "[GameEngine] Found game action validation response for request " << requestId
                          << ", valid=" << validAction << std::endl;

                // Find the game action state that matches this request
                GameActionState *gameState = nullptr;
                for (auto &handler : g_gameActionHandlers)
                {
                    if (handler && handler->action_idx == requestId)
                    {
                        gameState = handler.get();
                        std::cout << "[GameEngine] Found matching game state for action: " << gameState->action << std::endl;
                        break;
                    }
                }

                if (gameState && gameState->action == "player_action")
                {
                    // Enhance the response with game state information
                    juryResponse["game_id"] = gameState->gameId;
                    juryResponse["player_action"] = gameState->playerAction;

                    // Include the current game state after the action
                    if (validAction && !gameState->newGameState.empty())
                    {
                        // Action was valid - include the new game state
                        juryResponse["game_state"] = gameState->newGameState;
                        juryResponse["action_result"] = "success";
                        std::cout << "[GameEngine] Added new game state (valid action)" << std::endl;

                        // Check if game is won and trigger NFT generation
                        if (gameState->newGameState.find("Game_Status: won") != std::string::npos)
                        {
                            std::cout << "[GameEngine] GAME WON! Triggering player inventory extraction for NFT generation" << std::endl;
                            
                            if (g_valuableItemExtractor)
                            {
                                try
                                {
                                    // Extract player inventory from the winning game state
                                    g_valuableItemExtractor->extractPlayerInventory(
                                        gameState->gameId,
                                        gameState->newGameState,
                                        gameState->playerAction
                                    );
                                    
                                    std::cout << "[GameEngine] ✓ NFT data successfully generated for game: " 
                                              << gameState->gameId << std::endl;
                                }
                                catch (const std::exception& e)
                                {
                                    std::cerr << "[GameEngine] ERROR during NFT generation: " << e.what() << std::endl;
                                }
                            }
                            else
                            {
                                std::cerr << "[GameEngine] ERROR: ValuableItemExtractor not initialized!" << std::endl;
                            }
                        }
                    }
                    else
                    {
                        // Action was invalid - include the old game state (no change)
                        juryResponse["game_state"] = gameState->oldGameState;
                        juryResponse["action_result"] = "failed";
                        std::cout << "[GameEngine] Added old game state (invalid action)" << std::endl;
                        
                        // CRITICAL FIX: Revert the game state file to old state when action is invalid
                        if (!gameState->gameId.empty() && !gameState->oldGameState.empty())
                        {
                            std::cout << "[GameEngine] REVERTING game state file for game " << gameState->gameId << std::endl;
                            g_gameManager->saveGameState(gameState->gameId, gameState->oldGameState);
                            std::cout << "[GameEngine] Successfully reverted to old game state" << std::endl;
                        }
                    }

                    std::cout << "[GameEngine] Enhanced jury response with game state for game: "
                              << gameState->gameId << std::endl;

                    // Send the enhanced response
                    std::string enhancedResponse = juryResponse.dump();
                    std::cout << "[GameEngine] Sending enhanced response: " << enhancedResponse << std::endl;
                    hp_write_user_msg(user, enhancedResponse.c_str(), enhancedResponse.length());
                    return;
                }
            }
        }

        // For non-game-action responses or if we can't enhance, send original response
        std::cout << "[GameEngine] Sending original response (not enhanced)" << std::endl;
        hp_write_user_msg(user, response.c_str(), response.length());
    }
    catch (const std::exception &e)
    {
        std::cout << "[GameEngine] Error enhancing jury response: " << e.what() << std::endl;
        // Fallback to original response
        hp_write_user_msg(user, response.c_str(), response.length());
    }
}

// AI Jury vote processing (called for each NPL vote)
void process_jury_vote(const std::string &voteJson, int peer_count)
{
    if (g_aiJury)
    {
        g_aiJury->processVote(voteJson, peer_count);
    }
}

// Wait for AI Jury consensus (actively processes NPL messages until consensus reached)
void waitForJuryConsensus(int request_idx, int peer_count)
{
    if (!g_aiJury)
        return;

    char sender[HP_PUBLIC_KEY_SIZE];
    char *npl_msg = (char *)malloc(HP_NPL_MSG_MAX_SIZE);

    std::cout << "=== WAITING FOR AI JURY CONSENSUS ===" << std::endl;
    std::cout << "Request ID: " << request_idx << ", Peer count: " << peer_count << std::endl;

    // Keep processing NPL messages until consensus is reached
    while (true)
    {
        // Check if consensus has been reached first
        if (g_aiJury->isConsensusReached(request_idx))
        {
            std::cout << "[Jury] Consensus reached for request " << request_idx << " - exiting wait loop" << std::endl;
            break;
        }

        // Check for incoming NPL messages with short timeout
        const int npl_len = hp_read_npl_msg(npl_msg, sender, 100); // 100ms timeout
        if (npl_len > 0)
        {
            std::string voteJson(npl_msg, npl_len);
            std::cout << "Received jury vote: " << voteJson.substr(0, 100) << "..." << std::endl;
            process_jury_vote(voteJson, peer_count);
        }

        // Small delay to prevent busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    free(npl_msg);
    std::cout << "=== AI JURY CONSENSUS WAIT COMPLETE ===" << std::endl;
}

// COMMENTED OUT NFT CONSENSUS COORDINATION FUNCTIONS - IMPLEMENTING READ-ONLY MODE ONLY

/*
// NFT Minting Coordination Functions

// Deterministic node selection based on public key comparison
std::string selectDeterministicMinter(const std::string &gameId, int peer_count)
{
    const struct hp_contract_context *ctx = hp_get_context();
    if (!ctx || ctx->unl.count == 0) {
        return std::string(ctx->public_key.data); // Fallback to self if no UNL
    }
    
    std::cout << "[NFT] Selecting deterministic minter for game: " << gameId << std::endl;
    std::cout << "[NFT] UNL count: " << ctx->unl.count << std::endl;
    
    // Use lexicographic comparison to select lowest public key
    std::string selectedMinter = std::string(ctx->public_key.data);
    
    // Compare with all UNL nodes to find lowest public key
    for (size_t i = 0; i < ctx->unl.count; i++) {
        std::string nodeKey(ctx->unl.list[i].public_key.data);
        std::cout << "[NFT] Comparing node key: " << nodeKey.substr(0, 16) << "..." << std::endl;
        
        if (nodeKey < selectedMinter) {
            selectedMinter = nodeKey;
        }
    }
    
    std::cout << "[NFT] Selected minter: " << selectedMinter.substr(0, 16) << "..." << std::endl;
    return selectedMinter;
}
*/

/*
// Process NFT minting request with NPL coordination
void processNFTMintingRequest(const struct hp_user *user, const std::string &gameId, int peer_count)
{
    const struct hp_contract_context *ctx = hp_get_context();
    if (!ctx) {
        std::string error = "{\"type\":\"error\",\"error\":\"Contract context not available\"}";
        hp_write_user_msg(user, error.c_str(), error.length());
        return;
    }
    
    std::cout << "[NFT] Processing minting request for game: " << gameId << std::endl;
    
    // Check if NFT coordination is already in progress for this game
    if (g_nftCoordinationInProgress[gameId]) {
        std::cout << "[NFT] Coordination already in progress for game: " << gameId << std::endl;
        waitForNFTConsensus(gameId, peer_count);
        return;
    }
    
    // Select deterministic minter
    std::string selectedMinter = selectDeterministicMinter(gameId, peer_count);
    bool iAmTheMinter = (std::string(ctx->public_key.data) == selectedMinter);
    
    std::cout << "[NFT] I am the minter: " << (iAmTheMinter ? "YES" : "NO") << std::endl;
    
    // Mark NFT coordination as in progress
    g_nftCoordinationInProgress[gameId] = true;
    g_nftCoordinationStartTime[gameId] = std::chrono::steady_clock::now();
    
    if (iAmTheMinter) {
        std::cout << "[NFT] This node is the designated minter - performing actual minting..." << std::endl;
        
        // Load NFT data file
        std::string nftFilePath = "game_data/nft_" + gameId + ".json";
        std::ifstream nftFile(nftFilePath);
        if (!nftFile) {
            nlohmann::json errorResult;
            errorResult["type"] = "nft_coordination";
            errorResult["game_id"] = gameId;
            errorResult["success"] = false;
            errorResult["error"] = "NFT data file not found";
            
            // Broadcast error via NPL
            hp_write_npl_msg(errorResult.dump().c_str(), errorResult.dump().length());
            
            std::string error = "{\"type\":\"error\",\"error\":\"NFT data file not found for game: " + gameId + "\"}";
            hp_write_user_msg(user, error.c_str(), error.length());
            return;
        }
        
        std::string nftContent((std::istreambuf_iterator<char>(nftFile)),
                               std::istreambuf_iterator<char>());
        nftFile.close();
        
        try {
            nlohmann::json nftData = nlohmann::json::parse(nftContent);
            
            // Check if already minted
            if (g_nftMintingClient->isAlreadyMinted(nftData)) {
                nlohmann::json alreadyMintedResult;
                alreadyMintedResult["type"] = "nft_coordination";
                alreadyMintedResult["game_id"] = gameId;
                alreadyMintedResult["success"] = true;
                alreadyMintedResult["already_minted"] = true;
                alreadyMintedResult["message"] = "NFTs already minted for this game";
                
                // Store result and broadcast via NPL
                g_nftCoordinationResults[gameId] = alreadyMintedResult;
                hp_write_npl_msg(alreadyMintedResult.dump().c_str(), alreadyMintedResult.dump().length());
                
                hp_write_user_msg(user, alreadyMintedResult.dump().c_str(), alreadyMintedResult.dump().length());
                return;
            }
            
            // Perform actual NFT minting
            std::cout << "[NFT] Starting actual NFT minting for game: " << gameId << std::endl;
            NFTMintBatch mintResult = g_nftMintingClient->mintNFTsForGame(gameId, nftData);
            
            // Update NFT data file with minting results using shared function
            if (mintResult.success) {
                // Prepare minting results in standardized format for shared function
                nlohmann::json mintingResults;
                mintingResults["mint_timestamp"] = mintResult.batch_timestamp;
                mintingResults["mint_tx_hash"] = mintResult.first_success_hash;
                
                // Add NFT tokens array with Xahau-specific fields
                nlohmann::json nftTokens = nlohmann::json::array();
                for (const auto& result : mintResult.results) {
                    nlohmann::json token;
                    token["item"] = result.item_name;
                    token["nft_token_id"] = result.uritoken_id;  // Xahau URIToken ID
                    token["transaction_hash"] = result.transaction_hash;
                    token["metadata_uri"] = result.metadata_uri;
                    nftTokens.push_back(token);
                }
                mintingResults["nft_tokens"] = nftTokens;
                
                // Use shared function to update NFT file
                // TEMPORARILY COMMENTED OUT TO TEST CONSENSUS WITHOUT FILE I/O
                // bool fileUpdated = updateNFTFileWithMintingResults(gameId, mintingResults);
                // if (!fileUpdated) {
                //     std::cout << "[NFT] Warning: Failed to update NFT data file via shared function" << std::endl;
                // }
            }
            
            // Prepare result for NPL broadcast
            nlohmann::json nplResult;
            nplResult["type"] = "nft_coordination";
            nplResult["game_id"] = gameId;
            nplResult["success"] = mintResult.success;
            nplResult["mint_timestamp"] = mintResult.batch_timestamp;
            nplResult["total_requested"] = mintResult.total_requested;
            nplResult["successful_mints"] = mintResult.successful_mints;
            nplResult["failed_mints"] = mintResult.failed_mints;
            
            if (mintResult.success) {
                nplResult["batch_tx_hash"] = mintResult.first_success_hash;
                
                nlohmann::json mintedItems = nlohmann::json::array();
                for (const auto& result : mintResult.results) {
                    nlohmann::json item;
                    item["name"] = result.item_name;
                    item["nft_token_id"] = result.uritoken_id;  // Xahau URIToken ID
                    item["transaction_hash"] = result.transaction_hash;
                    item["metadata_uri"] = result.metadata_uri;
                    mintedItems.push_back(item);
                }
                nplResult["minted_items"] = mintedItems;
            } else {
                nplResult["error"] = "Some NFTs failed to mint";
                
                nlohmann::json failedItems = nlohmann::json::array();
                for (const auto& result : mintResult.results) {
                    if (!result.success) {
                        nlohmann::json item;
                        item["name"] = result.item_name;
                        item["error"] = result.error_message;
                        failedItems.push_back(item);
                    }
                }
                nplResult["failed_items"] = failedItems;
            }
            
            // Store result and broadcast via NPL
            g_nftCoordinationResults[gameId] = nplResult;
            hp_write_npl_msg(nplResult.dump().c_str(), nplResult.dump().length());
            
            std::cout << "[NFT] Minting completed and broadcasted via NPL" << std::endl;
            
            // Send response to user
            hp_write_user_msg(user, nplResult.dump().c_str(), nplResult.dump().length());
            
        } catch (const std::exception& e) {
            nlohmann::json errorResult;
            errorResult["type"] = "nft_coordination";
            errorResult["game_id"] = gameId;
            errorResult["success"] = false;
            errorResult["error"] = "Failed to parse NFT data: " + std::string(e.what());
            
            // Store error result and broadcast via NPL
            g_nftCoordinationResults[gameId] = errorResult;
            hp_write_npl_msg(errorResult.dump().c_str(), errorResult.dump().length());
            
            hp_write_user_msg(user, errorResult.dump().c_str(), errorResult.dump().length());
        }
    } else {
        std::cout << "[NFT] This node is NOT the minter - waiting for NPL consensus..." << std::endl;
        waitForNFTConsensus(gameId, peer_count);
    }
}

/*
// Wait for NFT minting consensus via NPL
void waitForNFTConsensus(const std::string &gameId, int peer_count)
{
    const struct hp_contract_context *ctx = hp_get_context();
    if (!ctx) return;
    
    char sender[HP_PUBLIC_KEY_SIZE];
    char *npl_msg = (char *)malloc(HP_NPL_MSG_MAX_SIZE);
    
    std::cout << "[NFT] Waiting for NFT minting consensus for game: " << gameId << std::endl;
    
    // Check if we already have the result
    if (g_nftCoordinationResults.find(gameId) != g_nftCoordinationResults.end()) {
        std::cout << "[NFT] Already have coordination result for game: " << gameId << std::endl;
        free(npl_msg);
        return;
    }
    
    int timeout_ms = 30000; // 30 seconds timeout for NFT minting
    auto start_time = std::chrono::steady_clock::now();
    
    while (true) {
        // Check timeout
        auto current_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start_time);
        if (elapsed.count() > timeout_ms) {
            std::cout << "[NFT] Timeout waiting for NFT minting consensus for game: " << gameId << std::endl;
            break;
        }
        
        // Check if we received the result
        if (g_nftCoordinationResults.find(gameId) != g_nftCoordinationResults.end()) {
            std::cout << "[NFT] Received NFT coordination result for game: " << gameId << std::endl;
            break;
        }
        
        // Check for incoming NPL messages
        const int npl_len = hp_read_npl_msg(npl_msg, sender, 100);
        if (npl_len > 0) {
            try {
                std::string msgStr(npl_msg, npl_len);
                nlohmann::json nplMessage = nlohmann::json::parse(msgStr);
                
                // Check if this is an NFT coordination result for our game
                if (nplMessage.contains("type") && nplMessage["type"] == "nft_coordination" &&
                    nplMessage.contains("game_id") && nplMessage["game_id"] == gameId) {
                    
                    // Filter out messages from ourselves
                    if (std::string(sender) != std::string(ctx->public_key.data)) {
                        std::cout << "[NFT] Received NFT result from minter node for game: " << gameId << std::endl;
                        
                        // Store the result
                        g_nftCoordinationResults[gameId] = nplMessage;
                        
                        // Update local NFT data file with minting results if successful
                        // TEMPORARILY COMMENTED OUT TO TEST CONSENSUS WITHOUT FILE I/O
                        // if (nplMessage.value("success", false) && nplMessage.contains("minted_items")) {
                        //     updateNFTFileWithMintingResults(gameId, nplMessage);
                        // }
                        break;
                    }
                }
            } catch (const std::exception& e) {
                std::cout << "[NFT] Error parsing NPL message: " << e.what() << std::endl;
            }
        }
        
        // Small delay to prevent busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    // Clean up
    g_nftCoordinationInProgress[gameId] = false;
    free(npl_msg);
    std::cout << "[NFT] NFT minting consensus wait complete for game: " << gameId << std::endl;
}

// Process NFT coordination messages (completely separate from AI Jury)
void processNFTCoordinationMessage(const std::string &nplMessageStr, const char *sender)
{
    const struct hp_contract_context *ctx = hp_get_context();
    if (!ctx) return;
    
    try {
        nlohmann::json nplMessage = nlohmann::json::parse(nplMessageStr);
        
        // Check if this is an NFT coordination message
        if (nplMessage.contains("type") && nplMessage["type"] == "nft_coordination" &&
            nplMessage.contains("game_id")) {
            
            std::string gameId = nplMessage["game_id"];
            
            // Filter out messages from ourselves
            if (std::string(sender) != std::string(ctx->public_key.data)) {
                std::cout << "[NFT] Received NFT coordination message from other node for game: " << gameId << std::endl;
                
                // Store the result for any waiting processes
                g_nftCoordinationResults[gameId] = nplMessage;
                g_nftCoordinationInProgress[gameId] = false;
                
                // Update local NFT data file if minting was successful
                // TEMPORARILY COMMENTED OUT TO TEST CONSENSUS WITHOUT FILE I/O
                // if (nplMessage.value("success", false) && nplMessage.contains("minted_items")) {
                //     updateNFTFileWithMintingResults(gameId, nplMessage);
                // }
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[NFT] Error parsing NFT coordination message: " << e.what() << std::endl;
    }
}
*/


// Main contract function
int main(int argc, char **argv)
{
    std::cout << "=== AI GAME CONTRACT (DAEMON-BASED ARCHITECTURE) ===" << std::endl;
    std::cout << "Starting AI Game Contract with daemon architecture..." << std::endl;

    // Initialize HotPocket contract
    if (hp_init_contract() != 0)
    {
        std::cerr << "Failed to initialize HotPocket contract." << std::endl;
        return 1;
    }

    // Initialize user input
    hp_init_user_input_mmap();

    // Initialize systems
    g_modelDownloader = std::make_unique<ModelDownloader>();
    g_gameManager = std::make_unique<GameStateManager>();
    g_aiClient = std::make_unique<AIServiceClient>();
    g_gameEngineDaemonManager = std::make_unique<GameEngineDaemonManager>();

    // Initialize Valuable Item Extractor for NFT generation
    g_valuableItemExtractor = std::make_unique<ValuableItemExtractor>();
    std::cout << "Valuable Item Extractor initialized for NFT generation" << std::endl;

    // Initialize NFT Minting Client for Xahau NFT minting
    g_nftMintingClient = std::make_unique<NFTMintingClient>();
    
    // Configure NFT minting client with wallet seed from environment
    const char* walletSeed = std::getenv("MINTER_WALLET_SEED");
    if (!walletSeed) {
        std::cerr << "ERROR: MINTER_WALLET_SEED environment variable not set!" << std::endl;
        std::cerr << "Please set: export MINTER_WALLET_SEED=<your_wallet_seed>" << std::endl;
        return 1;
    }
    
    g_nftMintingClient->setMinterWallet(walletSeed);
    
    std::cout << "NFT Minting Client initialized with environment configuration" << std::endl;

    // Initialize AI Jury for validation
    g_aiJury = AIJury::createAIModelJury();
    g_aiJury->setNPLBroadcast(juryNPLBroadcast);
    g_aiJury->setUserResponse(juryUserResponse);
    std::cout << "AI Jury ID: " << g_aiJury->getJuryId() << std::endl;

    // Load AI model for jury (starts daemon if needed)
    g_aiJury->loadAIModel();

    // Get contract context and peer count for consensus
    const struct hp_contract_context *ctx = hp_get_context();
    int peer_count = 1; // Default to 1 if no UNL info available

    std::cout << "=== UNL DEBUG INFO ===" << std::endl;
    std::cout << "ctx->unl.count: " << ctx->unl.count << std::endl;

    if (ctx && ctx->unl.count > 0)
    {
        peer_count = ctx->unl.count;
        std::cout << "Using UNL peer count: " << peer_count << std::endl;

        // Debug: Show UNL node count (public keys are binary data, not printable strings)
        std::cout << "Total UNL nodes detected: " << ctx->unl.count << std::endl;
    }
    else
    {
        std::cout << "No UNL peers found, using default peer_count = 1" << std::endl;
    }
    std::cout << "Final peer_count: " << peer_count << std::endl;
    std::cout << "=====================" << std::endl;

    // Initialize model downloading and daemon startup in non-readonly mode
    if (!ctx->readonly)
    {
        // Use legacy-style chunked download pattern - downloads one chunk per contract execution
        std::cout << "==================== MODEL VERIFICATION ===================" << std::endl;
        bool model_ready = g_modelDownloader->ensureModelDownloaded();

        if (model_ready)
        {
            std::cout << "Model is fully downloaded and verified!" << std::endl;
            std::cout << "=========================================================" << std::endl;

            // Start daemon only when model is ready
            std::cout << "Starting AI Daemon with verified model..." << std::endl;
            bool daemon_started = g_gameEngineDaemonManager->startDaemon();

            if (daemon_started)
            {
                std::cout << "AI Daemon process started successfully" << std::endl;
            }
            else
            {
                std::cerr << "WARNING: Failed to start AI Daemon process" << std::endl;
            }
        }
        else
        {
            std::cout << "Model download in progress - chunk downloaded this execution" << std::endl;
            std::cout << "Progress: " << std::fixed << std::setprecision(1)
                      << g_modelDownloader->getProgress() << "%" << std::endl;
            std::cout << "Run contract again to continue downloading..." << std::endl;
            std::cout << "=========================================================" << std::endl;
        }

        // Continue immediately without waiting for model loading
        std::cout << "Contract proceeding - AI operations will be available once model is complete" << std::endl;
    }

    std::cout << "Contract initialization complete. Ready for user requests." << std::endl;
    std::cout << "===========================================" << std::endl;

    // Process user messages
    for (size_t u = 0; u < ctx->users.count; u++)
    {
        const struct hp_user *user = &ctx->users.list[u];

        // Process each input for this user
        for (size_t input_idx = 0; input_idx < user->inputs.count; input_idx++)
        {
            char *buf = (char *)((char *)hp_init_user_input_mmap() + user->inputs.list[input_idx].offset);
            size_t len = user->inputs.list[input_idx].size;

            if (len > 0)
            {
                std::string message(buf, len);

                std::cout << "Received message: " << message << std::endl;

                // Check what patterns are being found
                bool foundStat = (message.find("\"type\":\"stat\"") != std::string::npos);
                bool foundQuery = (message.find("\"type\":\"query\"") != std::string::npos);
                std::cout << "Pattern search results: stat=" << foundStat << ", query=" << foundQuery << std::endl;

                // Simple string-based message type detection
                if (foundStat)
                {
                    std::cout << "=== DETECTED STAT MESSAGE ===" << std::endl;
                    process_stat_message(user);
                    std::cout << "=== STAT MESSAGE PROCESSING COMPLETE ===" << std::endl;
                }
                else if (foundQuery)
                {
                    std::cout << "=== DETECTED QUERY MESSAGE ===" << std::endl;
                    if (!ctx->readonly)
                    {
                        // Better JSON parsing to match example.js behavior
                        std::cout << "Processing query message: " << message << std::endl;

                        // First check if data field exists
                        size_t dataPos = message.find("\"data\":");
                        if (dataPos == std::string::npos)
                        {
                            std::string error = "{\"type\":\"error\",\"error\":\"must provide a data field to query message\"}";
                            hp_write_user_msg(user, error.c_str(), error.length());
                            continue;
                        }

                        // Find the data object content
                        size_t dataValueStart = message.find(":", dataPos) + 1;
                        // Skip whitespace
                        while (dataValueStart < message.length() &&
                               (message[dataValueStart] == ' ' || message[dataValueStart] == '\t' || message[dataValueStart] == '\n'))
                        {
                            dataValueStart++;
                        }

                        // Check if data is null or empty
                        if (dataValueStart >= message.length() ||
                            message.substr(dataValueStart, 4) == "null" ||
                            message.substr(dataValueStart, 9) == "undefined")
                        {
                            std::string error = "{\"type\":\"error\",\"error\":\"must provide a data field to query message\"}";
                            hp_write_user_msg(user, error.c_str(), error.length());
                            continue;
                        }

                        // Handle data as string directly or data as object with query field
                        std::string query;

                        // Check if data starts with a quote (data is a string directly)
                        if (message[dataValueStart] == '"')
                        {
                            // Case 1: {"type":"query","data":"actual query text"}
                            size_t queryStart = dataValueStart + 1;
                            size_t queryEnd = message.find("\"", queryStart);
                            if (queryEnd != std::string::npos)
                            {
                                query = message.substr(queryStart, queryEnd - queryStart);
                                std::cout << "Found query in data string: " << query << std::endl;
                            }
                        }
                        else if (message[dataValueStart] == '{')
                        {
                            // Case 2: {"type":"query","data":{"query":"actual query text"}}
                            size_t queryPos = message.find("\"query\":", dataPos);
                            if (queryPos == std::string::npos)
                            {
                                // Try without quotes around query key
                                queryPos = message.find("query:", dataPos);
                            }

                            if (queryPos != std::string::npos)
                            {
                                // Extract the query value from object
                                size_t queryValueStart = message.find(":", queryPos) + 1;
                                // Skip whitespace
                                while (queryValueStart < message.length() &&
                                       (message[queryValueStart] == ' ' || message[queryValueStart] == '\t' || message[queryValueStart] == '\n'))
                                {
                                    queryValueStart++;
                                }

                                if (queryValueStart < message.length() && message[queryValueStart] == '"')
                                {
                                    // Quoted string
                                    size_t queryStart = queryValueStart + 1;
                                    size_t queryEnd = message.find("\"", queryStart);
                                    if (queryEnd != std::string::npos)
                                    {
                                        query = message.substr(queryStart, queryEnd - queryStart);
                                        std::cout << "Found query in data object: " << query << std::endl;
                                    }
                                }
                            }
                        }
                        else
                        {
                            // Case 3: data is unquoted value
                            size_t queryEnd = message.find_first_of(",}", dataValueStart);
                            if (queryEnd != std::string::npos)
                            {
                                query = message.substr(dataValueStart, queryEnd - dataValueStart);
                                // Trim whitespace
                                query.erase(0, query.find_first_not_of(" \t\n"));
                                query.erase(query.find_last_not_of(" \t\n") + 1);
                                std::cout << "Found query as unquoted data: " << query << std::endl;
                            }
                        }

                        if (query.empty())
                        {
                            std::string error = "{\"type\":\"error\",\"error\":\"query field cannot be empty\"}";
                            hp_write_user_msg(user, error.c_str(), error.length());
                            continue;
                        }

                        std::cout << "Extracted query: " << query << std::endl;
                        // Use AI Jury for query processing
                        if (g_aiJury)
                        {
                            // Generate unique request ID for this query
                            static int query_request_id = 10000; // Start high to avoid conflicts with game actions
                            int current_request_id = query_request_id++;

                            g_aiJury->processRequest(user, "validate_query", query, current_request_id, peer_count, "query_interface_context");
                            waitForJuryConsensus(current_request_id, peer_count);
                        }
                        else
                        {
                            std::string response = "{\"type\":\"queryResult\",\"result\":\"AI Jury not available\"}";
                            hp_write_user_msg(user, response.c_str(), response.length());
                        }
                    }
                    else
                    {
                        std::string error = "{\"type\":\"error\",\"error\":\"query interface must not be read only\"}";
                        hp_write_user_msg(user, error.c_str(), error.length());
                    }
                }
                else
                {
                    // Check if it's a JSON game message format
                    bool isJsonGameMessage = false;
                    std::string gameAction;
                    std::string gameData;

                    // Try to parse as JSON game message first
                    if (message.front() == '{' && message.back() == '}')
                    {
                        // Check for game action fields
                        if (message.find("\"create_game\"") != std::string::npos)
                        {
                            gameAction = "create_game";
                            size_t actionPos = message.find("\"create_game\":");
                            if (actionPos != std::string::npos)
                            {
                                size_t valueStart = message.find(":", actionPos) + 1;
                                while (valueStart < message.length() &&
                                       (message[valueStart] == ' ' || message[valueStart] == '\t' || message[valueStart] == '\n'))
                                {
                                    valueStart++;
                                }
                                if (valueStart < message.length() && message[valueStart] == '"')
                                {
                                    size_t dataStart = valueStart + 1;
                                    size_t dataEnd = message.find("\"", dataStart);
                                    if (dataEnd != std::string::npos)
                                    {
                                        gameData = message.substr(dataStart, dataEnd - dataStart);
                                        isJsonGameMessage = true;
                                    }
                                }
                            }
                        }
                        else if (message.find("\"game_id\"") != std::string::npos && message.find("\"action\"") != std::string::npos)
                        {
                            gameAction = "player_action";
                            // Extract game_id, action, and continue_conversation from JSON format: {"game_id": "id", "action": "text", "continue_conversation": "true"}
                            size_t gameIdPos = message.find("\"game_id\":");
                            size_t actionPos = message.find("\"action\":");
                            size_t continuePos = message.find("\"continue_conversation\":");

                            if (gameIdPos != std::string::npos && actionPos != std::string::npos)
                            {
                                // Extract game_id
                                size_t gameIdValueStart = message.find(":", gameIdPos) + 1;
                                while (gameIdValueStart < message.length() &&
                                       (message[gameIdValueStart] == ' ' || message[gameIdValueStart] == '\t' || message[gameIdValueStart] == '\n'))
                                {
                                    gameIdValueStart++;
                                }
                                if (gameIdValueStart < message.length() && message[gameIdValueStart] == '"')
                                {
                                    size_t gameIdStart = gameIdValueStart + 1;
                                    size_t gameIdEnd = message.find("\"", gameIdStart);

                                    // Extract action
                                    size_t actionValueStart = message.find(":", actionPos) + 1;
                                    while (actionValueStart < message.length() &&
                                           (message[actionValueStart] == ' ' || message[actionValueStart] == '\t' || message[actionValueStart] == '\n'))
                                    {
                                        actionValueStart++;
                                    }
                                    if (actionValueStart < message.length() && message[actionValueStart] == '"')
                                    {
                                        size_t actionStart = actionValueStart + 1;
                                        size_t actionEnd = message.find("\"", actionStart);

                                        if (gameIdEnd != std::string::npos && actionEnd != std::string::npos)
                                        {
                                            std::string gameId = message.substr(gameIdStart, gameIdEnd - gameIdStart);
                                            std::string action = message.substr(actionStart, actionEnd - actionStart);
                                            
                                            // Extract continue_conversation if present
                                            std::string continueConversation = "false"; // default value
                                            if (continuePos != std::string::npos)
                                            {
                                                size_t continueValueStart = message.find(":", continuePos) + 1;
                                                while (continueValueStart < message.length() &&
                                                       (message[continueValueStart] == ' ' || message[continueValueStart] == '\t' || message[continueValueStart] == '\n'))
                                                {
                                                    continueValueStart++;
                                                }
                                                if (continueValueStart < message.length() && message[continueValueStart] == '"')
                                                {
                                                    size_t continueStart = continueValueStart + 1;
                                                    size_t continueEnd = message.find("\"", continueStart);
                                                    if (continueEnd != std::string::npos)
                                                    {
                                                        continueConversation = message.substr(continueStart, continueEnd - continueStart);
                                                    }
                                                }
                                            }
                                            
                                            gameData = gameId + ":" + action + ":" + continueConversation;
                                            isJsonGameMessage = true;
                                            std::cout << "Parsed player action - Game ID: " << gameId << ", Action: " << action << ", Continue: " << continueConversation << std::endl;
                                        }
                                    }
                                }
                            }
                        }
                        else if (message.find("\"list_games\"") != std::string::npos)
                        {
                            gameAction = "list_games";
                            gameData = "";
                            isJsonGameMessage = true;
                        }
                        else if (message.find("\"get_game_state\"") != std::string::npos)
                        {
                            gameAction = "get_game_state";
                            size_t actionPos = message.find("\"get_game_state\":");
                            if (actionPos != std::string::npos)
                            {
                                size_t valueStart = message.find(":", actionPos) + 1;
                                while (valueStart < message.length() &&
                                       (message[valueStart] == ' ' || message[valueStart] == '\t' || message[valueStart] == '\n'))
                                {
                                    valueStart++;
                                }
                                if (valueStart < message.length() && message[valueStart] == '"')
                                {
                                    size_t dataStart = valueStart + 1;
                                    size_t dataEnd = message.find("\"", dataStart);
                                    if (dataEnd != std::string::npos)
                                    {
                                        gameData = message.substr(dataStart, dataEnd - dataStart);
                                        isJsonGameMessage = true;
                                    }
                                }
                            }
                        }
                        else if (message.find("\"mint_nft\"") != std::string::npos)
                        {
                            gameAction = "mint_nft";
                            size_t actionPos = message.find("\"mint_nft\":");
                            if (actionPos != std::string::npos)
                            {
                                size_t valueStart = message.find(":", actionPos) + 1;
                                while (valueStart < message.length() &&
                                       (message[valueStart] == ' ' || message[valueStart] == '\t' || message[valueStart] == '\n'))
                                {
                                    valueStart++;
                                }
                                if (valueStart < message.length() && message[valueStart] == '"')
                                {
                                    size_t dataStart = valueStart + 1;
                                    size_t dataEnd = message.find("\"", dataStart);
                                    if (dataEnd != std::string::npos)
                                    {
                                        gameData = message.substr(dataStart, dataEnd - dataStart);
                                        isJsonGameMessage = true;
                                    }
                                }
                            }
                        }
                    }

                    if (isJsonGameMessage)
                    {
                        std::cout << "=== DETECTED JSON GAME MESSAGE ===" << std::endl;
                        std::cout << "Game Action: " << gameAction << std::endl;
                        std::cout << "Game Data: " << gameData << std::endl;

                        // Process game action with AI validation consensus
                        int action_idx = static_cast<int>(u * 1000 + input_idx);
                        process_game_message(user, gameAction, gameData, action_idx, peer_count);
                    }
                    else
                    {
                        // Fall back to colon-separated format: "action:data"
                        size_t colonPos = message.find(":");
                        if (colonPos != std::string::npos)
                        {
                            std::string action = message.substr(0, colonPos);
                            std::string data = message.substr(colonPos + 1);

                            if (action == "stat")
                            {
                                process_stat_message(user);
                            }
                            else
                            {
                                // Process game action with AI validation consensus
                                // Use a unique action index for each user input
                                int action_idx = static_cast<int>(u * 1000 + input_idx);
                                process_game_message(user, action, data, action_idx, peer_count);
                            }
                        }
                        else
                        {
                            // Unknown message type
                            std::string error = "{\"type\":\"error\",\"error\":\"Unsupported message type\"}";
                            hp_write_user_msg(user, error.c_str(), error.length());
                        }
                    }
                }
            }
        }
    }

    // Handle NPL messages (votes from other nodes) - AI Jury only
    char sender[HP_PUBLIC_KEY_SIZE];
    char *npl_msg = (char *)malloc(HP_NPL_MSG_MAX_SIZE);

    // Check for incoming NPL messages with short timeout
    const int npl_len = hp_read_npl_msg(npl_msg, sender, 100); // 100ms timeout
    if (npl_len > 0)
    {
        std::string msgJson(npl_msg, npl_len);

        try {
            nlohmann::json nplMessage = nlohmann::json::parse(msgJson);
            
            // COMMENTED OUT NFT COORDINATION - READ-ONLY MODE ONLY
            // Check for NFT coordination messages (completely separate system)
            // if (nplMessage.contains("type") && nplMessage["type"] == "nft_coordination") {
            //     processNFTCoordinationMessage(msgJson, sender);
            // }
            // Check for AI Jury votes (separate system)
            if (nplMessage.contains("requestId")) {
                // This is an AI Jury vote - process in main try block
                process_jury_vote(msgJson, peer_count);
            }
            else if (nplMessage.contains("type") && nplMessage["type"] == "nft_coordination") {
                std::cout << "[NPL] IGNORED: NFT coordination disabled - read-only mode only" << std::endl;
            }
            else {
                std::cout << "[NPL] IGNORED: Unknown message format: " << msgJson.substr(0, 100) << "..." << std::endl;
            }
        } catch (const std::exception& e) {
            // Fallback for JSON parsing errors only - use string search as last resort
            std::cout << "[NPL] JSON parse failed, attempting string-based detection: " << e.what() << std::endl;
            
            if (msgJson.find("\"requestId\":") != std::string::npos) {
                // Fallback: This is likely an AI Jury vote with malformed JSON
                std::cout << "[NPL] Fallback: Processing as AI Jury vote" << std::endl;
                process_jury_vote(msgJson, peer_count);
            } else if (msgJson.find("\"type\":\"nft_coordination\"") != std::string::npos) {
                // COMMENTED OUT NFT COORDINATION - READ-ONLY MODE ONLY
                // Fallback: This is likely an NFT coordination message with malformed JSON
                // std::cout << "[NPL] Fallback: Processing as NFT coordination message" << std::endl;
                // processNFTCoordinationMessage(msgJson, sender);
                std::cout << "[NPL] IGNORED: NFT coordination disabled - read-only mode only" << std::endl;
            } else {
                std::cout << "[NPL] IGNORED: Cannot identify message type even with string search: " << msgJson.substr(0, 100) << "..." << std::endl;
            }
        }
    }

    free(npl_msg);

    // Cleanup
    hp_deinit_user_input_mmap();
    hp_deinit_contract();

    return 0;
}

void waitForGameConsensus(int action_idx, int peer_count)
{
    char sender[HP_PUBLIC_KEY_SIZE];
    char *npl_msg = (char *)malloc(HP_NPL_MSG_MAX_SIZE);

    std::cout << "=== WAITING FOR CONSENSUS (AI JURY ONLY) ===" << std::endl;
    std::cout << "Action index: " << action_idx << ", Peer count: " << peer_count << std::endl;

    // Legacy waitForGameConsensus is deprecated - AI Jury handles all consensus now
    // This function remains for compatibility but only processes AI Jury votes
    
    // Brief check for any AI Jury votes that might come through this path
    const int npl_len = hp_read_npl_msg(npl_msg, sender, 100); // Short timeout
    if (npl_len > 0)
    {
        std::string voteJson(npl_msg, npl_len);
        std::cout << "Received vote: " << voteJson.substr(0, 100) << "..." << std::endl;

        // Only process AI Jury votes now
        if (voteJson.find("\"requestId\":") != std::string::npos)
        {
            std::cout << "Processing AI Jury vote through legacy path" << std::endl;
            process_jury_vote(voteJson, peer_count);
        }
        else
        {
            std::cout << "IGNORED: Legacy vote format no longer supported" << std::endl;
        }
    }

    free(npl_msg);
    std::cout << "=== LEGACY CONSENSUS WAIT COMPLETE ===" << std::endl;
}
