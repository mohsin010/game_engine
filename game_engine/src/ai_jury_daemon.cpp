// AI Validation Daemon - Background AI service that provides binary validation
// Solves the 5-20 minute model loading delays for validation tasks

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <memory>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "../httplib/httplib.h"
#include <openssl/sha.h>
#include <nlohmann/json.hpp>
#include "../llama.cpp/common/common.h"
#include "../llama.cpp/include/llama.h"

// Global flags
static std::atomic<bool> g_shutdown_requested{false};
static bool g_test_mode = false;

// Signal handlers
void signal_handler(int signal)
{
    std::cout << "Received signal " << signal << ", shutting down..." << std::endl;
    g_shutdown_requested = true;
}

// AI Model Downloader for automatic model acquisition
class ModelDownloader
{
private:
    // const std::string fileName = "Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf";
    // const std::string expectedHash = "7b064f5842bf9532c91456deda288a1b672397a54fa729aa665952863033557c";
    // const size_t expectedSize = 4920739232; // bytes (~4.5 GB)
    // const std::string sourceUrl = "https://huggingface.co/bartowski/Meta-Llama-3.1-8B-Instruct-GGUF/resolve/main/Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf";

    const std::string fileName = "gpt-oss-20b-Q5_K_M.gguf";
    const std::string expectedHash = "9c3814533c5b4c84d42b5dce4376bbdfd7227e990b8733a3a1c4f741355b3e75";
    const size_t expectedSize = 11717357248; // bytes (~11.3 GB)
    const std::string sourceUrl = "https://huggingface.co/unsloth/gpt-oss-20b-GGUF/resolve/main/gpt-oss-20b-Q5_K_M.gguf";

    const size_t chunkSize = 256 * 1024 * 1024; // 256 MiB chunks

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
                std::cerr << "[ModelDownloader] Invalid URL format" << std::endl;
                return false;
            }

            size_t hostStart = schemePos + 3;
            size_t pathStart = url.find("/", hostStart);

            if (pathStart == std::string::npos)
            {
                std::cerr << "[ModelDownloader] Invalid URL: no path found" << std::endl;
                return false;
            }

            std::string host = url.substr(hostStart, pathStart - hostStart);
            std::string path = url.substr(pathStart);

            // Use httplib HTTPS client
            httplib::SSLClient cli(host);
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
                {"User-Agent", "AI-Jury-Daemon/1.0"}};

            std::cout << "[ModelDownloader] Downloading bytes " << startByte << "-" << endByte
                      << " (" << actualChunkSize << " bytes)" << std::endl;

            auto res = cli.Get(path, headers);

            if (!res)
            {
                std::cerr << "[ModelDownloader] HTTP request failed" << std::endl;
                return false;
            }

            if (res->status != 206 && res->status != 200)
            { // 206 = Partial Content
                std::cerr << "[ModelDownloader] HTTP error: " << res->status << std::endl;
                return false;
            }

            // Open file for writing (append mode)
            std::ofstream file(filePath, std::ios::binary | std::ios::app);
            if (!file)
            {
                std::cerr << "[ModelDownloader] Cannot open file for writing: " << filePath << std::endl;
                return false;
            }

            file.write(res->body.c_str(), res->body.size());
            file.close();

            std::cout << "[ModelDownloader] Downloaded " << res->body.size() << " bytes successfully" << std::endl;
            return true;
        }
        catch (const std::exception &e)
        {
            std::cerr << "[ModelDownloader] Exception during download: " << e.what() << std::endl;
            return false;
        }
    }

    bool ensureModelDownloaded(const std::string &targetPath)
    {
        std::string filePath = targetPath;

        // Create model directory if it doesn't exist
        std::filesystem::create_directories(std::filesystem::path(filePath).parent_path());

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
            std::cerr << "[ModelDownloader] Error checking file: " << e.what() << std::endl;
            fileSize = 0;
        }

        if (fileSize == expectedSize)
        {
            std::cout << "[ModelDownloader] Model already downloaded and verified" << std::endl;
            modelFilePath = filePath;

            // Quick hash verification
            try
            {
                std::string calculatedHash = calculateSHA256(filePath);
                if (calculatedHash == expectedHash)
                {
                    std::cout << "[ModelDownloader] Hash verification successful" << std::endl;
                    return true;
                }
                else
                {
                    std::cout << "[ModelDownloader] Hash mismatch, re-downloading..." << std::endl;
                    std::filesystem::remove(filePath);
                    fileSize = 0;
                }
            }
            catch (const std::exception &e)
            {
                std::cout << "[ModelDownloader] Hash verification failed, re-downloading..." << std::endl;
                std::filesystem::remove(filePath);
                fileSize = 0;
            }
        }

        // Download the model completely
        return downloadCompleteModel(filePath);
    }

    bool downloadCompleteModel(const std::string &filePath)
    {
        std::cout << "[ModelDownloader] Starting complete model download..." << std::endl;
        std::cout << "[ModelDownloader] Current file size: " << fileSize << " / " << expectedSize
                  << " (" << (double)fileSize / expectedSize * 100.0 << "%)" << std::endl;

        while (fileSize < expectedSize)
        {
            std::cout << "[ModelDownloader] Downloading next chunk from byte " << fileSize << "..." << std::endl;

            if (!downloadChunk(sourceUrl, filePath, fileSize))
            {
                std::cerr << "[ModelDownloader] Failed to download chunk, aborting" << std::endl;
                return false;
            }

            // Update file size after download
            try
            {
                fileSize = std::filesystem::file_size(filePath);
            }
            catch (const std::exception &e)
            {
                std::cerr << "[ModelDownloader] Error getting file size after download: " << e.what() << std::endl;
                return false;
            }

            std::cout << "[ModelDownloader] Progress: " << fileSize << " / " << expectedSize
                      << " (" << (double)fileSize / expectedSize * 100.0 << "%)" << std::endl;
        }

        if (fileSize >= expectedSize)
        {
            std::cout << "[ModelDownloader] Download complete, verifying hash..." << std::endl;

            try
            {
                std::string calculatedHash = calculateSHA256(filePath);

                if (calculatedHash == expectedHash)
                {
                    std::cout << "[ModelDownloader] Hash verification successful!" << std::endl;
                    modelFilePath = filePath;
                    return true;
                }
                else
                {
                    std::cerr << "[ModelDownloader] Hash mismatch. Expected: " << expectedHash
                              << ", Got: " << calculatedHash << std::endl;
                    std::filesystem::remove(filePath);
                    return false;
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << "[ModelDownloader] Hash verification failed: " << e.what() << std::endl;
                std::filesystem::remove(filePath);
                return false;
            }
        }

        return false;
    }

    std::string getModelPath() const
    {
        return modelFilePath;
    }

    double getProgress() const
    {
        if (expectedSize == 0)
            return 0.0;
        return (double)fileSize / expectedSize * 100.0;
    }

    size_t getExpectedSize() const
    {
        return expectedSize;
    }
};

class AIValidationDaemon
{
private:
    std::atomic<bool> running{true};
    int server_socket = -1;
    int port = 8766; // AI validation daemon port

    // AI Model components
    llama_model *model = nullptr;
    std::string model_path;
    std::atomic<bool> model_loaded{false};
    std::atomic<bool> model_loading{false};
    std::string model_error = "";

    // Model Downloader
    std::unique_ptr<ModelDownloader> modelDownloader;

    // Heartbeat for debugging
    std::atomic<bool> heartbeat_running{true};
    std::thread heartbeat_thread;

public:
    AIValidationDaemon(const std::string &modelPath) : model_path(modelPath)
    {
        // Install signal handlers
        signal(SIGTERM, signal_handler);
        signal(SIGINT, signal_handler);

        // Initialize model downloader
        modelDownloader = std::make_unique<ModelDownloader>();

        // Start heartbeat thread for debugging
        startHeartbeat();
    }

    ~AIValidationDaemon()
    {
        cleanup();
    }

    void startHeartbeat()
    {
        heartbeat_thread = std::thread([this]()
                                       {
            std::cout << "[Daemon] Heartbeat thread started" << std::endl;
            int beat_count = 0;
            
            while (heartbeat_running && !g_shutdown_requested) {
                std::this_thread::sleep_for(std::chrono::seconds(g_test_mode ? 10 : 60)); // Faster heartbeat in test mode
                
                if (heartbeat_running && !g_shutdown_requested) {
                    beat_count++;
                    std::string status = "unknown";
                    if (model_loaded) status = "ready";
                    else if (model_loading) status = "loading";
                    else if (!model_error.empty()) status = "error";
                    else status = "initializing";
                    
                    std::cout << "[ValidationDaemon] HEARTBEAT #" << beat_count 
                              << " - Status: " << status 
                              << " - PID: " << getpid()
                              << " - Running: " << running.load()
                              << (g_test_mode ? " [TEST MODE]" : "") << std::endl;
                    std::cout.flush();
                }
            }
            std::cout << "[ValidationDaemon] Heartbeat thread exiting" << std::endl; });
    }

    void stopHeartbeat()
    {
        heartbeat_running = false;
        if (heartbeat_thread.joinable())
        {
            heartbeat_thread.join();
        }
    }

    bool loadModel()
    {
        std::cout << "[Daemon] ========== Starting Model Loading Process ==========" << std::endl;
        std::cout << "[Daemon] Model path: " << model_path << std::endl;
        std::cout << "[Daemon] Current working directory: " << std::filesystem::current_path() << std::endl;
        std::cout << "[Daemon] Process ID: " << getpid() << std::endl;

        model_loading = true;

        // STEP 1: Ensure model is downloaded using ModelDownloader
        std::cout << "[Daemon] STEP 1: Ensuring model is downloaded..." << std::endl;
        if (!modelDownloader->ensureModelDownloaded(model_path))
        {
            model_error = "Failed to download or verify model file";
            model_loading = false;
            std::cerr << "[Daemon] ERROR: Model download/verification failed!" << std::endl;
            return false;
        }
        std::cout << "[Daemon] STEP 1: ✓ Model file ready and verified!" << std::endl;

        // Check file size and permissions (quick verification)
        std::cout << "[Daemon] STEP 2: Final verification of downloaded model..." << std::endl;
        try
        {
            auto file_size = std::filesystem::file_size(model_path);
            auto file_perms = std::filesystem::status(model_path).permissions();

            std::cout << "[Daemon] File size: " << file_size << " bytes ("
                      << (file_size / 1024.0 / 1024.0 / 1024.0) << " GB)" << std::endl;
            std::cout << "[Daemon] File readable: " << ((file_perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none) << std::endl;
        }
        catch (const std::exception &e)
        {
            model_error = "Failed to verify downloaded model file: " + std::string(e.what());
            model_loading = false;
            std::cerr << "[Daemon] ERROR: Failed to verify model file: " << e.what() << std::endl;
            return false;
        }
        std::cout << "[Daemon] STEP 2: ✓ Model verification complete!" << std::endl;

        try
        {
            std::cout << "[Daemon] STEP 3: Initializing llama backend..." << std::endl;
            std::cout.flush(); // Force output before potentially blocking operation

            // Initialize llama backend
            llama_backend_init();
            std::cout << "[Daemon] STEP 3: ✓ Llama backend initialized!" << std::endl;

            std::cout << "[Daemon] STEP 4: Setting up model parameters..." << std::endl;
            // Load model using working API
            llama_model_params model_params = llama_model_default_params();
            model_params.n_gpu_layers = 32; // Enable GPU acceleration with 32 layers
            model_params.use_mmap = true;   // Use memory mapping for efficiency
            model_params.use_mlock = false; // Don't lock memory (might fail in Docker)

            std::cout << "[Daemon] Model parameters:" << std::endl;
            std::cout << "[Daemon]   n_gpu_layers: " << model_params.n_gpu_layers << std::endl;
            std::cout << "[Daemon]   use_mmap: " << model_params.use_mmap << std::endl;
            std::cout << "[Daemon]   use_mlock: " << model_params.use_mlock << std::endl;
            std::cout << "[Daemon] STEP 4: ✓ Model parameters set!" << std::endl;

            std::cout << "[Daemon] STEP 5: Loading model from file (THIS MAY TAKE SEVERAL MINUTES)..." << std::endl;
            std::cout << "[Daemon] Starting llama_model_load_from_file() call..." << std::endl;
            std::cout.flush(); // Force output before the long operation

            // Add periodic progress indicators during the long model loading
            std::atomic<bool> loading_in_progress{true};
            std::thread progress_thread([&loading_in_progress]()
                                        {
                int dots = 0;
                while (loading_in_progress) {
                    std::this_thread::sleep_for(std::chrono::seconds(g_test_mode ? 5 : 30)); // Faster progress in test mode
                    if (loading_in_progress) {
                        std::cout << "[Daemon] Model loading still in progress" 
                                  << std::string(dots % 4, '.') 
                                  << (g_test_mode ? " [TEST MODE]" : "") << std::endl;
                        std::cout.flush();
                        dots++;
                    }
                } });

            model = llama_model_load_from_file(model_path.c_str(), model_params);
            loading_in_progress = false;
            progress_thread.join();

            if (!model)
            {
                model_error = "llama_model_load_from_file returned null - model loading failed";
                model_loading = false;
                std::cerr << "[Daemon] ERROR: Model loading failed!" << std::endl;
                std::cerr << "[Daemon] llama_model_load_from_file returned null" << std::endl;
                return false;
            }

            std::cout << "[Daemon] STEP 5: ✓ Model loaded from file successfully!" << std::endl;

            std::cout << "[Daemon] STEP 6: Verifying model..." << std::endl;
            // Basic model verification
            const llama_vocab *vocab = llama_model_get_vocab(model);
            if (!vocab)
            {
                model_error = "Model validation failed - could not get vocabulary";
                model_loading = false;
                std::cerr << "[Daemon] ERROR: Model validation failed!" << std::endl;
                llama_model_free(model);
                model = nullptr;
                return false;
            }

            // Try to get vocabulary size using the vocab pointer
            int vocab_size = llama_vocab_n_tokens(vocab);
            std::cout << "[Daemon] Model vocabulary size: " << vocab_size << std::endl;
            std::cout << "[Daemon] STEP 6: ✓ Model verification passed!" << std::endl;

            model_loaded = true;
            model_loading = false;

            std::cout << "[Daemon] ========== Model Loading Complete! ==========" << std::endl;
            std::cout << "[Daemon] Model loaded successfully and ready for inference!" << std::endl;
            std::cout.flush();

            return true;
        }
        catch (const std::exception &e)
        {
            model_error = "Exception during model loading: " + std::string(e.what());
            model_loading = false;
            std::cerr << "[Daemon] ERROR: Exception during model loading: " << e.what() << std::endl;
            return false;
        }
        catch (...)
        {
            model_error = "Unknown exception during model loading";
            model_loading = false;
            std::cerr << "[Daemon] ERROR: Unknown exception during model loading!" << std::endl;
            return false;
        }
    }

    void loadModelAsync()
    {
        std::cout << "[Daemon] Starting async model loading thread..." << std::endl;
        std::thread([this]()
                    {
            std::cout << "[Daemon] Model loading thread started (thread_id=" << std::this_thread::get_id() << ")" << std::endl;
            std::cout.flush();
            
            auto start_time = std::chrono::steady_clock::now();
            bool success = loadModel();
            auto end_time = std::chrono::steady_clock::now();
            
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);
            
            if (success) {
                std::cout << "[Daemon] ========== MODEL LOADING COMPLETED ==========" << std::endl;
                std::cout << "[Daemon] Model loading successful! Duration: " << duration.count() << " seconds" << std::endl;
            } else {
                std::cout << "[Daemon] ========== MODEL LOADING FAILED ==========" << std::endl;
                std::cout << "[Daemon] Model loading failed! Duration: " << duration.count() << " seconds" << std::endl;
                std::cout << "[Daemon] Error: " << model_error << std::endl;
            }
            std::cout.flush(); })
            .detach();
        std::cout << "[Daemon] Async model loading thread launched" << std::endl;
    }

    std::string generateValidationResponse(const std::string &prompt, int max_tokens = 10)
    {
        if (!model_loaded || !model)
        {
            return "{\"error\":\"Model not loaded\"}";
        }

        const llama_vocab *vocab = llama_model_get_vocab(model);

        const int n_prompt = -llama_tokenize(vocab, prompt.c_str(), prompt.size(), NULL, 0, true, true);
        std::vector<llama_token> prompt_tokens(n_prompt);

        if (llama_tokenize(vocab, prompt.c_str(), prompt.size(), prompt_tokens.data(), prompt_tokens.size(), true, true) < 0)
        {
            return "{\"error\":\"Failed to tokenize prompt\"}";
        }

        llama_context_params ctx_params = llama_context_default_params();
        ctx_params.n_ctx = 2048;                      // Smaller context for validation tasks
        ctx_params.n_batch = std::max(256, n_prompt); // Smaller batch for efficiency
        ctx_params.no_perf = true;
        ctx_params.n_threads = 6; // Fewer threads for validation
        ctx_params.n_threads_batch = 6;
        llama_context *ctx = llama_init_from_model(model, ctx_params);
        if (!ctx)
        {
            return "{\"error\":\"Failed to create context\"}";
        }

        // Ultra-restrictive sampling parameters for binary validation
        auto sparams = llama_sampler_chain_default_params();
        sparams.no_perf = true;
        llama_sampler *smpl = llama_sampler_chain_init(sparams);

        llama_sampler_chain_add(smpl, llama_sampler_init_top_k(2));    // Only top 2 tokens (YES/NO)
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.01f)); // Ultra low temperature for deterministic responses
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(0));

        llama_batch batch = llama_batch_get_one(prompt_tokens.data(), prompt_tokens.size());

        std::string response;
        int n_decode = 0;

        std::cout << "[ValidationDaemon] Starting binary validation..." << std::endl;

        for (int n_pos = 0; n_pos + batch.n_tokens < ctx_params.n_ctx && n_decode < max_tokens;)
        {
            int decode_result = llama_decode(ctx, batch);
            if (decode_result != 0)
            {
                std::cout << "[ValidationDaemon] ERROR: llama_decode failed with code " << decode_result << std::endl;
                break;
            }

            n_pos += batch.n_tokens;

            llama_token new_token_id = llama_sampler_sample(smpl, ctx, -1);

            // Break on end-of-generation
            if (llama_vocab_is_eog(vocab, new_token_id))
            {
                if (n_decode > 0)
                {
                    std::cout << "[ValidationDaemon] End of generation reached after " << n_decode << " tokens" << std::endl;
                    break;
                }
                else
                {
                    std::cout << "[ValidationDaemon] WARNING: Got end-of-generation on first token, continuing..." << std::endl;
                    continue;
                }
            }

            char buf[128];
            int n = llama_token_to_piece(vocab, new_token_id, buf, sizeof(buf), 0, true);
            if (n > 0)
            {
                std::string token_str(buf, n);
                response += token_str;

                // Stop IMMEDIATELY if we get clear binary indicators
                std::string lower_response = response;
                std::transform(lower_response.begin(), lower_response.end(), lower_response.begin(), ::tolower);

                if (lower_response.find("yes") != std::string::npos ||
                    lower_response.find("no") != std::string::npos ||
                    lower_response == "y" || lower_response == "n")
                {
                    std::cout << "[ValidationDaemon] IMMEDIATE termination triggered by YES/NO: " << response << std::endl;
                    break;
                }

                if (lower_response.find("valid") != std::string::npos ||
                    lower_response.find("invalid") != std::string::npos ||
                    lower_response.find("true") != std::string::npos ||
                    lower_response.find("false") != std::string::npos)
                {
                    std::cout << "[ValidationDaemon] Early termination triggered by binary indicator: " << response << std::endl;
                    break;
                }

                // Stop if response is getting too long (prevent rambling)
                if (response.length() > 15)
                {
                    std::cout << "[ValidationDaemon] Response length limit reached, stopping at: " << response << std::endl;
                    break;
                }
            }

            batch = llama_batch_get_one(&new_token_id, 1);
            n_decode++;
        }

        std::cout << "[ValidationDaemon] Validation completed. Generated " << n_decode << " tokens, response: '" << response << "'" << std::endl;

        llama_sampler_free(smpl);
        llama_free(ctx);

        return response;
    }

    std::string processValidation(const nlohmann::json &request)
    {
        std::string statement = request.value("statement", "");

        if (statement.empty())
        {
            return "{\"error\":\"No statement provided for validation\"}";
        }

        // Simple binary validation prompt
        // std::string prompt =
        //     "You are a binary validator. Analyze the following statement and respond with exactly one word: YES or NO.\n\n"
        //     "STATEMENT: " + statement + "\n\n"
        //     "INSTRUCTIONS:\n"
        //     "- If the statement is true, logical, valid, or reasonable, respond: YES\n"
        //     "- If the statement is false, illogical, invalid, or unreasonable, respond: NO\n"
        //     "- Do not explain your reasoning\n"
        //     "- Do not add any other text\n"
        //     "- Your response must be exactly one word: YES or NO\n\n"
        //     "RESPONSE: ";
        std::string prompt =
            "You are an ultra-permissive and creativity-loving game master validator. Your job is to ENCOURAGE player imagination and say YES to almost everything!\n\n"

            "DATA TO ANALYZE:\n" +
            statement + "\n\n"

                        "ULTRA-PERMISSIVE GUIDELINES - Say YES unless the action is:\n"
                        "1. Completely nonsensical (like turning into a refrigerator for no reason)\n"
                        "2. Explicitly breaking fundamental game rules (like instantly killing all NPCs)\n"
                        "3. Completely unrelated to the game context\n\n"

                        "CREATIVITY-FIRST APPROACH:\n"
                        "- Say YES to ALL creative and imaginative actions\n"
                        "- Say YES to magical/fantasy elements even if they seem powerful\n"
                        "- Say YES to unusual character abilities and transformations\n"
                        "- Say YES to inventive problem-solving approaches\n"
                        "- Say YES to dramatic story changes and plot twists\n"
                        "- Say YES to resource gathering, crafting, and exploration\n"
                        "- Say YES to social interactions and dialogue\n"
                        "- Say YES to combat actions and skill usage\n"
                        "- Say YES to world-building and environmental changes\n"
                        "- Say YES to informational requests and observations\n"
                        "- Default to YES when uncertain - favor fun over realism!\n\n"

                        "REMEMBER: Players should feel free to be wildly creative. Only say NO to truly absurd or game-breaking actions.\n\n"

                        "Respond with exactly one word: YES (for creative/valid actions) or NO (only for truly absurd actions)\n\n"

                        "RESPONSE: ";

        std::string ai_response = generateValidationResponse(prompt, 5);

        // Enhanced binary response parsing
        std::string lower_response = ai_response;
        std::transform(lower_response.begin(), lower_response.end(), lower_response.begin(), ::tolower);

        // Remove any whitespace
        lower_response.erase(std::remove_if(lower_response.begin(), lower_response.end(), ::isspace), lower_response.end());

        std::cout << "[ValidationDaemon] === VALIDATION PARSING ===" << std::endl;
        std::cout << "[ValidationDaemon] Raw response: '" << ai_response << "'" << std::endl;
        std::cout << "[ValidationDaemon] Cleaned response: '" << lower_response << "'" << std::endl;

        bool containsYes = (lower_response.find("yes") != std::string::npos);
        bool containsNo = (lower_response.find("no") != std::string::npos);
        bool containsTrue = (lower_response.find("true") != std::string::npos);
        bool containsFalse = (lower_response.find("false") != std::string::npos);
        bool containsValid = (lower_response.find("valid") != std::string::npos);
        bool containsInvalid = (lower_response.find("invalid") != std::string::npos);
        bool isY = (lower_response == "y");
        bool isN = (lower_response == "n");

        // Determine validity - default to false for safety
        bool is_valid = false;
        double confidence = 0.0;

        // Perfect matches get highest confidence
        if (lower_response == "yes" || lower_response == "y")
        {
            is_valid = true;
            confidence = 1.0;
        }
        else if (lower_response == "no" || lower_response == "n")
        {
            is_valid = false;
            confidence = 1.0;
        }
        else if (lower_response == "true")
        {
            is_valid = true;
            confidence = 0.95;
        }
        else if (lower_response == "false")
        {
            is_valid = false;
            confidence = 0.95;
        }
        else if (containsYes && !containsNo)
        {
            is_valid = true;
            confidence = 0.8;
        }
        else if (containsNo && !containsYes)
        {
            is_valid = false;
            confidence = 0.8;
        }
        else if (containsTrue && !containsFalse)
        {
            is_valid = true;
            confidence = 0.75;
        }
        else if (containsFalse && !containsTrue)
        {
            is_valid = false;
            confidence = 0.75;
        }
        else if (containsValid && !containsInvalid)
        {
            is_valid = true;
            confidence = 0.7;
        }
        else if (containsInvalid && !containsValid)
        {
            is_valid = false;
            confidence = 0.7;
        }
        else
        {
            // Ambiguous or unclear response - default to false for safety
            is_valid = false;
            confidence = 0.3;
        }

        std::cout << "[ValidationDaemon] Analysis:" << std::endl;
        std::cout << "[ValidationDaemon]   Statement: " << statement << std::endl;
        std::cout << "[ValidationDaemon]   Final decision: " << (is_valid ? "YES" : "NO") << std::endl;
        std::cout << "[ValidationDaemon]   Confidence: " << confidence << std::endl;
        std::cout << "[ValidationDaemon] ===============================" << std::endl;

        return "{\"valid\":" + std::string(is_valid ? "true" : "false") +
               ",\"confidence\":" + std::to_string(confidence) +
               ",\"raw_response\":\"" + ai_response + "\"}";
    }

    std::string handleRequest(const std::string &request_str)
    {
        try
        {
            nlohmann::json request = nlohmann::json::parse(request_str);
            std::string type = request["type"];

            if (type == "validate")
            {
                return processValidation(request);
            }
            else if (type == "ping")
            {
                std::string status = "loading";
                if (model_loaded)
                {
                    status = "ready";
                }
                else if (!model_loading && !model_error.empty())
                {
                    status = "error";
                }

                return "{\"status\":\"" + status + "\"" +
                       ",\"model_loaded\":" + std::string(model_loaded ? "true" : "false") +
                       ",\"model_loading\":" + std::string(model_loading ? "true" : "false") +
                       (model_error.empty() ? "" : ",\"error\":\"" + model_error + "\"") +
                       "}";
            }
            else
            {
                return "{\"error\":\"Unknown request type. Supported types: 'validate', 'ping'\"}";
            }
        }
        catch (const std::exception &e)
        {
            return "{\"error\":\"Failed to parse request: " + std::string(e.what()) + "\"}";
        }
    }

    void handleClient(int client_socket)
    {
        std::cout << "[ValidationDaemon] Handling client (fd=" << client_socket << ", thread_id=" << std::this_thread::get_id() << ")" << std::endl;

        char buffer[8192];
        ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);

        if (bytes_received > 0)
        {
            buffer[bytes_received] = '\0';
            std::string request(buffer);

            std::cout << "[ValidationDaemon] Received " << bytes_received << " bytes" << std::endl;
            std::cout << "[ValidationDaemon] Request preview: " << request.substr(0, 100) << "..." << std::endl;

            std::string response = handleRequest(request);

            std::cout << "[ValidationDaemon] Generated response (" << response.length() << " bytes)" << std::endl;
            std::cout << "[ValidationDaemon] Response preview: " << response.substr(0, 100) << "..." << std::endl;

            ssize_t bytes_sent = send(client_socket, response.c_str(), response.length(), 0);
            if (bytes_sent == -1)
            {
                std::cerr << "[ValidationDaemon] Failed to send response: " << strerror(errno) << std::endl;
            }
            else
            {
                std::cout << "[ValidationDaemon] Sent " << bytes_sent << " bytes successfully" << std::endl;
            }
        }
        else if (bytes_received == 0)
        {
            std::cout << "[ValidationDaemon] Client closed connection" << std::endl;
        }
        else
        {
            std::cerr << "[ValidationDaemon] Failed to receive data: " << strerror(errno) << std::endl;
        }

        close(client_socket);
        std::cout << "[ValidationDaemon] Client connection closed (fd=" << client_socket << ")" << std::endl;
    }

    bool startServer()
    {
        std::cout << "[Daemon] ========== Starting TCP Server ==========" << std::endl;
        std::cout << "[Daemon] Port: " << port << std::endl;
        std::cout << "[Daemon] Process ID: " << getpid() << std::endl;
        std::cout << "[Daemon] Current working directory: " << std::filesystem::current_path() << std::endl;

        // Create PID file for debugging
        std::cout << "[Daemon] Creating PID file..." << std::endl;
        try
        {
            std::ofstream pid_file("./ai_jury_daemon.pid");
            if (pid_file.is_open())
            {
                pid_file << getpid() << std::endl;
                pid_file.close();
                std::cout << "[ValidationDaemon] ✓ PID file created: ./ai_jury_daemon.pid" << std::endl;
            }
            else
            {
                std::cerr << "[ValidationDaemon] WARNING: Failed to create PID file" << std::endl;
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "[Daemon] WARNING: Exception creating PID file: " << e.what() << std::endl;
        }

        // Create TCP socket
        std::cout << "[Daemon] STEP 1: Creating TCP socket..." << std::endl;
        server_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket == -1)
        {
            std::cerr << "[Daemon] ERROR: Failed to create socket: " << strerror(errno) << std::endl;
            return false;
        }
        std::cout << "[Daemon] STEP 1: ✓ Socket created! (fd=" << server_socket << ")" << std::endl;

        // Set socket options for better cleanup
        std::cout << "[Daemon] STEP 2: Setting socket options..." << std::endl;
        int opt = 1;
        if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
        {
            std::cerr << "[Daemon] WARNING: Failed to set SO_REUSEADDR: " << strerror(errno) << std::endl;
        }
        std::cout << "[Daemon] STEP 2: ✓ Socket options set!" << std::endl;

        // Bind socket to port
        std::cout << "[Daemon] STEP 3: Binding socket to port..." << std::endl;
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1"); // localhost only
        addr.sin_port = htons(port);

        std::cout << "[Daemon] Binding to: 127.0.0.1:" << port << std::endl;
        if (bind(server_socket, (struct sockaddr *)&addr, sizeof(addr)) == -1)
        {
            std::cerr << "[Daemon] ERROR: Failed to bind socket: " << strerror(errno) << std::endl;
            close(server_socket);
            return false;
        }
        std::cout << "[Daemon] STEP 3: ✓ Socket bound successfully!" << std::endl;

        // Listen for connections
        std::cout << "[Daemon] STEP 4: Starting to listen for connections..." << std::endl;
        if (listen(server_socket, 5) == -1)
        {
            std::cerr << "[Daemon] ERROR: Failed to listen on socket: " << strerror(errno) << std::endl;
            close(server_socket);
            return false;
        }
        std::cout << "[Daemon] STEP 4: ✓ Socket listening!" << std::endl;

        std::cout << "[Daemon] ========== TCP Server Started Successfully! ==========" << std::endl;
        return true;
    }

    void run()
    {
        std::cout << "[Daemon] ========== Starting AI jury Daemon ==========" << std::endl;
        std::cout << "[Daemon] Process ID: " << getpid() << std::endl;
        std::cout << "[Daemon] Starting server..." << std::endl;

        if (!startServer())
        {
            std::cerr << "[Daemon] FATAL: Failed to start server, exiting" << std::endl;
            return;
        }

        std::cout << "[Daemon] ========== Server Ready ==========" << std::endl;
        std::cout << "[Daemon] Beginning model loading in background..." << std::endl;

        // Start model loading asynchronously - don't block!
        loadModelAsync();

        std::cout << "[Daemon] ========== Daemon Ready for Requests ==========" << std::endl;
        std::cout << "[Daemon] Model loading in progress - accepting connections" << std::endl;
        std::cout << "[Daemon] TCP server listening on port: " << port << std::endl;
        std::cout.flush();

        // Main server loop with enhanced debugging
        int connection_count = 0;
        while (running && !g_shutdown_requested)
        {
            std::cout << "[Daemon] Waiting for connections... (count: " << connection_count << ")" << std::endl;
            std::cout.flush();

            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            std::cout << "[Daemon] Calling accept()..." << std::endl;
            std::cout.flush();

            int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
            if (client_socket == -1)
            {
                if (running && !g_shutdown_requested)
                {
                    std::cerr << "[Daemon] Failed to accept connection: " << strerror(errno) << std::endl;

                    // Check if server socket is still valid
                    int socket_error = 0;
                    socklen_t len = sizeof(socket_error);
                    if (getsockopt(server_socket, SOL_SOCKET, SO_ERROR, &socket_error, &len) == 0)
                    {
                        if (socket_error != 0)
                        {
                            std::cerr << "[Daemon] Server socket error: " << strerror(socket_error) << std::endl;
                            break; // Exit if server socket is broken
                        }
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }

            connection_count++;
            std::cout << "[Daemon] Accepted connection " << connection_count << " (fd=" << client_socket << ")" << std::endl;

            // Get current model status for logging
            std::string status = model_loaded ? "ready" : (model_loading ? "loading" : "error");
            std::cout << "[Daemon] Current model status: " << status << std::endl;

            // Handle client in separate thread for concurrency
            std::thread client_thread(&AIValidationDaemon::handleClient, this, client_socket);
            client_thread.detach();

            std::cout << "[Daemon] Client handler thread started for connection " << connection_count << std::endl;
            std::cout.flush();
        }

        std::cout << "[Daemon] Exiting main server loop (running=" << running
                  << ", shutdown_requested=" << g_shutdown_requested.load() << ")" << std::endl;
    }

    void stop()
    {
        running = false;
        if (server_socket != -1)
        {
            close(server_socket);
        }
    }

    void cleanup()
    {
        std::cout << "[Daemon] Starting cleanup..." << std::endl;

        stopHeartbeat();
        stop();

        if (model)
        {
            std::cout << "[Daemon] Freeing model..." << std::endl;
            llama_model_free(model);
            model = nullptr;
        }

        std::cout << "[Daemon] Freeing llama backend..." << std::endl;
        llama_backend_free();

        std::cout << "[ValidationDaemon] Removing PID file..." << std::endl;
        unlink("./ai_jury_daemon.pid");

        std::cout << "[Daemon] Cleanup complete" << std::endl;
    }
};

int main(int argc, char *argv[])
{
    std::string model_path = "../../../model/gpt-oss-20b-Q5_K_M.gguf";

    // Parse command line arguments
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "--test")
        {
            g_test_mode = true;
            std::cout << "[Daemon] TEST MODE ENABLED" << std::endl;
        }
        else if (arg.find("--model=") == 0)
        {
            model_path = arg.substr(8); // Skip "--model="
        }
        else if (i == 1 && arg[0] != '-')
        {
            // First non-flag argument is model path (backward compatibility)
            model_path = arg;
        }
    }

    std::cout << "[ValidationDaemon] ========== AI VALIDATION DAEMON STARTUP ==========" << std::endl;
    std::cout << "[ValidationDaemon] Starting AI Validation Daemon with model: " << model_path << std::endl;
    std::cout << "[ValidationDaemon] Process ID: " << getpid() << std::endl;
    std::cout << "[ValidationDaemon] Working directory: " << std::filesystem::current_path() << std::endl;
    std::cout << "[ValidationDaemon] Test mode: " << (g_test_mode ? "ENABLED" : "DISABLED") << std::endl;
    std::cout << "[ValidationDaemon] Command line args: " << argc << std::endl;
    for (int i = 0; i < argc; i++)
    {
        std::cout << "[ValidationDaemon]   arg[" << i << "]: " << argv[i] << std::endl;
    }
    std::cout << "[ValidationDaemon] =============================================" << std::endl;
    std::cout.flush();

    try
    {
        std::cout << "[ValidationDaemon] Creating validation daemon instance..." << std::endl;
        AIValidationDaemon daemon(model_path);

        std::cout << "[ValidationDaemon] Starting daemon run loop..." << std::endl;
        daemon.run();

        std::cout << "[ValidationDaemon] Daemon run loop completed" << std::endl;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[ValidationDaemon] FATAL EXCEPTION: " << e.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "[ValidationDaemon] FATAL UNKNOWN EXCEPTION" << std::endl;
        return 2;
    }

    std::cout << "[ValidationDaemon] Shutting down..." << std::endl;
    return 0;
}
