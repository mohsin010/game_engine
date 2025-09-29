// AI Daemon - Background AI service that keeps model loaded in memory
// Solves the 5-20 minute model loading delays in the contract system

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

class AIDaemon
{
private:
    std::atomic<bool> running{true};
    int server_socket = -1;
    int port = 8765; // TCP port instead of socket file

    // AI Model components
    llama_model *model = nullptr;
    std::string model_path;
    std::atomic<bool> model_loaded{false};
    std::atomic<bool> model_loading{false};
    std::string model_error = "";

    // Conversation continuity components
    llama_context *persistent_ctx = nullptr;
    llama_sampler *persistent_sampler = nullptr;
    std::atomic<bool> conversation_active{false};
    int conversation_position = 0; // Track position in context for continuation

    // Heartbeat for debugging
    std::atomic<bool> heartbeat_running{true};
    std::thread heartbeat_thread;

public:
    AIDaemon(const std::string &modelPath) : model_path(modelPath)
    {
        // Install signal handlers
        signal(SIGTERM, signal_handler);
        signal(SIGINT, signal_handler);

        // Start heartbeat thread for debugging
        startHeartbeat();
    }

    ~AIDaemon()
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
                    
                    std::cout << "[Daemon] HEARTBEAT #" << beat_count 
                              << " - Status: " << status 
                              << " - PID: " << getpid()
                              << " - Running: " << running.load()
                              << (g_test_mode ? " [TEST MODE]" : "") << std::endl;
                    std::cout.flush();
                }
            }
            std::cout << "[Daemon] Heartbeat thread exiting" << std::endl; });
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

        // Check if model file exists in persistent directory
        std::cout << "[Daemon] STEP 1: Checking if model file exists..." << std::endl;
        if (!std::filesystem::exists(model_path))
        {
            model_error = "Model file not found in persistent directory";
            model_loading = false;
            std::cerr << "[Daemon] ERROR: Model file not found!" << std::endl;
            std::cerr << "[Daemon] Path checked: " << model_path << std::endl;

            // List contents of parent directories for debugging
            std::cout << "[Daemon] Listing contents of ../../../ :" << std::endl;
            try
            {
                for (const auto &entry : std::filesystem::directory_iterator("../../../"))
                {
                    std::cout << "[Daemon]   " << entry.path().filename() << std::endl;
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << "[Daemon] Failed to list ../../../: " << e.what() << std::endl;
            }

            return false;
        }
        std::cout << "[Daemon] STEP 1: ✓ Model file found!" << std::endl;

        // Check file size and permissions
        std::cout << "[Daemon] STEP 2: Checking file size and permissions..." << std::endl;
        try
        {
            auto file_size = std::filesystem::file_size(model_path);
            auto file_perms = std::filesystem::status(model_path).permissions();

            std::cout << "[Daemon] File size: " << file_size << " bytes ("
                      << (file_size / 1024.0 / 1024.0) << " MB)" << std::endl;
            std::cout << "[Daemon] File readable: " << ((file_perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none) << std::endl;

            if (file_size < 1000000)
            { // Less than 1MB is definitely wrong
                model_error = "Model file appears to be incomplete (size: " + std::to_string(file_size) + " bytes)";
                model_loading = false;
                std::cerr << "[Daemon] ERROR: Model file too small!" << std::endl;
                return false;
            }

            // Expected size should be around 4.9GB
            if (file_size < 4000000000)
            { // Less than 4GB
                std::cout << "[Daemon] WARNING: Model file smaller than expected 4.9GB" << std::endl;
            }
        }
        catch (const std::exception &e)
        {
            model_error = "Failed to check model file: " + std::string(e.what());
            model_loading = false;
            std::cerr << "[Daemon] ERROR: Failed to check model file: " << e.what() << std::endl;
            return false;
        }
        std::cout << "[Daemon] STEP 2: ✓ File size and permissions OK!" << std::endl;

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

    std::string generateResponse(const std::string &prompt, int max_tokens = 800)
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
        ctx_params.n_ctx = 8192;                      // FIXED: Increased to 8192 for better context and longer conversations
        ctx_params.n_batch = std::max(512, n_prompt); // FIXED: Ensure batch size can handle the full prompt
        ctx_params.no_perf = true;
        ctx_params.n_threads = 10;       // Use 10 CPU cores for inference
        ctx_params.n_threads_batch = 10; // Use 10 cores for batch processing
        llama_context *ctx = llama_init_from_model(model, ctx_params);
        if (!ctx)
        {
            return "{\"error\":\"Failed to create context\"}";
        }

        // Adaptive sampling parameters based on task type
        auto sparams = llama_sampler_chain_default_params();
        sparams.no_perf = true;
        llama_sampler *smpl = llama_sampler_chain_init(sparams);

        // Optimized sampling parameters for instruction following and structured output
        llama_sampler_chain_add(smpl, llama_sampler_init_top_k(20));    // Reduced for more focused responses
        llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.7f, 1)); // Reduced for more deterministic output
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.3f));   // Much lower temperature for instruction following
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(0));

        llama_batch batch = llama_batch_get_one(prompt_tokens.data(), prompt_tokens.size());

        std::string response;
        int n_decode = 0;

        // CRITICAL FIX: Add debug logging and more robust token generation
        std::cout << "[Daemon] Starting token generation for " << max_tokens << " tokens..." << std::endl;

        for (int n_pos = 0; n_pos + batch.n_tokens < ctx_params.n_ctx && n_decode < max_tokens;)
        {
            int decode_result = llama_decode(ctx, batch);
            if (decode_result != 0)
            {
                std::cout << "[Daemon] ERROR: llama_decode failed with code " << decode_result << " at token " << n_decode << std::endl;
                break;
            }

            n_pos += batch.n_tokens;

            llama_token new_token_id = llama_sampler_sample(smpl, ctx, -1);

            // CRITICAL FIX: Only break on end-of-generation after generating some tokens
            if (llama_vocab_is_eog(vocab, new_token_id))
            {
                if (n_decode > 0)
                {
                    std::cout << "[Daemon] End of generation reached after " << n_decode << " tokens" << std::endl;
                    break;
                }
                else
                {
                    // If we get EOG immediately, continue sampling to try to get actual content
                    std::cout << "[Daemon] WARNING: Got end-of-generation on first token, continuing..." << std::endl;
                    continue;
                }
            }

            char buf[128];
            int n = llama_token_to_piece(vocab, new_token_id, buf, sizeof(buf), 0, true);
            if (n > 0)
            {
                std::string token_str(buf, n);
                response += token_str;
                
                // Check for end marker to stop generation early
                if (response.find("<<END_PLAYER_STATE>>") != std::string::npos)
                {
                    std::cout << "[Daemon] Found end marker, stopping generation at " << n_decode << " tokens" << std::endl;
                    break;
                }
                
                // Check for Llama 3.1 end tokens
                if (response.find("<|eot_id|>") != std::string::npos)
                {
                    std::cout << "[Daemon] Found Llama 3.1 end token, stopping generation at " << n_decode << " tokens" << std::endl;
                    break;
                }

                // Debug: Print progress every 50 tokens
                if (n_decode % 50 == 0 && n_decode > 0)
                {
                    std::cout << "[Daemon] Generated " << n_decode << " tokens so far..." << std::endl;
                }
            }

            batch = llama_batch_get_one(&new_token_id, 1);
            n_decode++;
        }

        std::cout << "[Daemon] Token generation completed. Generated " << n_decode << " tokens, response length: " << response.length() << std::endl;

        llama_sampler_free(smpl);
        llama_free(ctx);

        return response;
    }

    bool initializePersistentContext()
    {
        if (!model_loaded || !model)
        {
            std::cout << "[Daemon] Cannot initialize persistent context - model not loaded" << std::endl;
            return false;
        }

        std::cout << "[Daemon] Initializing persistent context for conversation continuity..." << std::endl;

        // Create persistent context
        llama_context_params ctx_params = llama_context_default_params();
        ctx_params.n_ctx = 8192;                      // Increased to 8192 for longer conversations
        ctx_params.n_batch = 2048;  // FIXED: Increased from 512 to 2048 to prevent GGML assertion failure
        ctx_params.no_perf = true;
        ctx_params.n_threads = 10;
        ctx_params.n_threads_batch = 10;
        
        persistent_ctx = llama_init_from_model(model, ctx_params);
        if (!persistent_ctx)
        {
            std::cout << "[Daemon] ERROR: Failed to create persistent context" << std::endl;
            return false;
        }

        // Create persistent sampler
        auto sparams = llama_sampler_chain_default_params();
        sparams.no_perf = true;
        persistent_sampler = llama_sampler_chain_init(sparams);

        // Game-optimized sampling parameters
        llama_sampler_chain_add(persistent_sampler, llama_sampler_init_top_k(40));
        llama_sampler_chain_add(persistent_sampler, llama_sampler_init_top_p(0.9f, 1));
        llama_sampler_chain_add(persistent_sampler, llama_sampler_init_temp(0.8f));
        llama_sampler_chain_add(persistent_sampler, llama_sampler_init_dist(0));

        conversation_position = 0;
        std::cout << "[Daemon] ✓ Persistent context initialized successfully" << std::endl;
        return true;
    }

    void cleanupPersistentContext()
    {
        if (persistent_sampler)
        {
            std::cout << "[Daemon] Cleaning up persistent sampler..." << std::endl;
            llama_sampler_free(persistent_sampler);
            persistent_sampler = nullptr;
        }

        if (persistent_ctx)
        {
            std::cout << "[Daemon] Cleaning up persistent context..." << std::endl;
            llama_free(persistent_ctx);
            persistent_ctx = nullptr;
        }

        conversation_active = false;
        conversation_position = 0;
        std::cout << "[Daemon] ✓ Persistent context cleanup complete" << std::endl;
    }

    std::string generateResponseContinue(const std::string &action, int max_tokens = 250)
    {
        if (!model_loaded || !model)
        {
            return "{\"error\":\"Model not loaded\"}";
        }

        if (!persistent_ctx || !persistent_sampler)
        {
            std::cout << "[Daemon] ERROR: Persistent context not initialized, falling back to regular generation" << std::endl;
            return "{\"error\":\"Persistent context not available\"}";
        }

        std::cout << "[Daemon] Using conversation continuation mode..." << std::endl;

        const llama_vocab *vocab = llama_model_get_vocab(model);

        // Create lightweight continuation prompt using Llama 3.1 format
        std::string continuation_prompt = 
            "<|start_header_id|>user<|end_header_id|>\n\n"
            "Player Action: " + action + "\n\n"
            "Update the player state:<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n"
            "<<BEGIN_PLAYER_STATE>>\n";

        // Tokenize the continuation prompt
        const int n_prompt = -llama_tokenize(vocab, continuation_prompt.c_str(), continuation_prompt.size(), NULL, 0, true, true);
        std::vector<llama_token> prompt_tokens(n_prompt);

        if (llama_tokenize(vocab, continuation_prompt.c_str(), continuation_prompt.size(), prompt_tokens.data(), prompt_tokens.size(), true, true) < 0)
        {
            return "{\"error\":\"Failed to tokenize continuation prompt\"}";
        }

        // Process the continuation prompt
        llama_batch batch = llama_batch_get_one(prompt_tokens.data(), prompt_tokens.size());
        
        std::string response;
        int n_decode = 0;

        std::cout << "[Daemon] Starting continuation token generation for " << max_tokens << " tokens..." << std::endl;

        // Process the prompt first
        int decode_result = llama_decode(persistent_ctx, batch);
        if (decode_result != 0)
        {
            std::cout << "[Daemon] ERROR: llama_decode failed for continuation prompt with code " << decode_result << std::endl;
            return "{\"error\":\"Failed to process continuation prompt\"}";
        }

        conversation_position += batch.n_tokens;

        // Generate response tokens
        for (int n_pos = conversation_position; n_pos < 8192 && n_decode < max_tokens;)
        {
            llama_token new_token_id = llama_sampler_sample(persistent_sampler, persistent_ctx, -1);

            // Check for end of generation
            if (llama_vocab_is_eog(vocab, new_token_id))
            {
                if (n_decode > 0)
                {
                    std::cout << "[Daemon] End of generation reached after " << n_decode << " tokens" << std::endl;
                    break;
                }
                else
                {
                    std::cout << "[Daemon] WARNING: Got end-of-generation on first token, continuing..." << std::endl;
                    continue;
                }
            }

            // Convert token to text
            char buf[128];
            int n = llama_token_to_piece(vocab, new_token_id, buf, sizeof(buf), 0, true);
            if (n > 0)
            {
                std::string token_str(buf, n);
                response += token_str;
                
                // Check for end marker to stop generation early
                if (response.find("<<END_PLAYER_STATE>>") != std::string::npos)
                {
                    std::cout << "[Daemon] Found end marker in continuation, stopping generation at " << n_decode << " tokens" << std::endl;
                    break;
                }
                
                // Check for Llama 3.1 end tokens
                if (response.find("<|eot_id|>") != std::string::npos)
                {
                    std::cout << "[Daemon] Found Llama 3.1 end token in continuation, stopping generation at " << n_decode << " tokens" << std::endl;
                    break;
                }

                // Debug: Print progress every 50 tokens
                if (n_decode % 50 == 0 && n_decode > 0)
                {
                    std::cout << "[Daemon] Generated " << n_decode << " continuation tokens so far..." << std::endl;
                }
            }

            // Add the new token to context for next iteration
            llama_batch next_batch = llama_batch_get_one(&new_token_id, 1);
            decode_result = llama_decode(persistent_ctx, next_batch);
            if (decode_result != 0)
            {
                std::cout << "[Daemon] ERROR: llama_decode failed during generation with code " << decode_result << " at token " << n_decode << std::endl;
                break;
            }

            conversation_position++;
            n_decode++;
        }

        std::cout << "[Daemon] Continuation generation completed. Generated " << n_decode << " tokens, response length: " << response.length() << std::endl;
        std::cout << "[Daemon] Conversation position now at: " << conversation_position << std::endl;

        return response;
    }

    std::string processGameCreation(const nlohmann::json &request)
    {
        std::string prompt = request["prompt"];
        std::string game_prompt =
            "Create a complete structured game world for a hybrid AI-governed gaming system. This must be compatible with rule-based processing.\n\n"
            
            "REQUIRED FORMAT (follow exactly):\n\n"
            
            "Game Title: [Engaging title]\n\n"
            
            "World Description: [2-3 sentences describing setting and atmosphere]\n\n"
            
            "World Lore: [1-2 sentences of background that affects gameplay]\n\n"
            
            "Objectives: [Primary goal - clear and achievable]\n\n"
            
            "Win Conditions: [Specific conditions to win]\n\n"
            
            "Valid Actions: MOVE [direction], EXAMINE [object], TAKE [item], USE [item], TALK [character], ATTACK [target], CAST [spell], OPEN [container]\n\n"
            
            "Locations:\n"
            "- [Location 1]: [Description]. Exits: [directions]. Items: [list]. NPCs: [list]\n"
            "- [Location 2]: [Description]. Exits: [directions]. Items: [list]. NPCs: [list]\n"
            "- [Add 3-5 connected locations]\n\n"
            
            "Items:\n"
            "- [Item 1]: [Description and properties]\n"
            "- [Item 2]: [Description and properties]\n"
            "- [Add key items for objectives]\n\n"
            
            "Game Rules:\n"
            "- [Rule about movement/exploration]\n"
            "- [Rule about items/inventory]\n"
            "- [Rule about winning/losing]\n\n"
            
            "Starting Location: [Location name]\n\n"
            
            "Starting Inventory: [List starting items]\n\n"
            
            "Starting Health: [Number/100]\n\n"
            
            "Current Situation: [Opening scenario that sets the stage]\n\n"
            
            "User request: " + prompt + "\n\n"
            "CRITICAL: Follow the exact format above. Create a world that supports structured rule-based gameplay with bounded actions.";

        std::string ai_response = generateResponse(game_prompt, 500);

        // For text format, we don't need JSON cleaning - just return the narrative
        return ai_response;
    }

    std::string processPlayerAction(const nlohmann::json &request)
    {
        std::string action = request["action"];
        std::string game_state = request.value("game_state", "");
        std::string game_world = request.value("game_world", "");
        bool continue_conversation = request.value("continue_conversation", false);

        std::string ai_response;

        // Determine which mode to use
        // || !conversation_active.load()
        if (!continue_conversation)
        {
            // INITIAL MODE - Full context establishment
            std::cout << "[Daemon] Using initial mode - establishing full context" << std::endl;
            
            std::string system_prompt = 
                "You are a game state processor. Process player actions and return ONLY the updated player state in the exact format specified. Use this format for subsequent entire conversation thread. "
                "STRICTLY Do not PRODUCE explanations, reasoning, or any other text. Replace bracketed placeholders with actual values based on the action and game rules."
                "IMPORTANT: If player repeats an action or similar action send the same updated state again without changes.";
            
            std::string user_content = 
                "GAME WORLD:\n" + game_world + "\n\n"
                "CURRENT PLAYER STATE:\n" + game_state + "\n\n"
                "PLAYER ACTION: " + action + "\n\n"
                "Return the updated player state in this exact format below:\n"

                "<<BEGIN_PLAYER_STATE>>\n"
                
                "Player_Location: [location_name]\n"
                "Player_Health: [number]\n"
                "Player_Score: [number]\n"
                "Player_Inventory: [list]\n"
                "Game_Status: [active/won/lost]\n"
                "Messages: [\"A narrative of what happens and should be immersive and provides good game play experience\"]\n"
                "Turn_Count: [number]\n"

                "<<END_PLAYER_STATE>>";
            
            // Format as Llama 3.1 chat template
            std::string prompt = 
                "<|begin_of_text|><|start_header_id|>system<|end_header_id|>\n\n" + 
                system_prompt + "<|eot_id|><|start_header_id|>user<|end_header_id|>\n\n" + 
                user_content + "<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n";

            ai_response = generateResponse(prompt, 400);

            // After successful initial response, set up persistent context for future continuations
            // continue_conversation && 
            if (!conversation_active.load())
            {
                if (initializePersistentContext())
                {
                    std::cout << "[Daemon] Initializing conversation context with full prompt..." << std::endl;
                    
                    // Process the full initial prompt through persistent context to establish conversation
                    const llama_vocab *vocab = llama_model_get_vocab(model);
                    const int n_prompt = -llama_tokenize(vocab, prompt.c_str(), prompt.size(), NULL, 0, true, true);
                    std::vector<llama_token> prompt_tokens(n_prompt);

                    if (llama_tokenize(vocab, prompt.c_str(), prompt.size(), prompt_tokens.data(), prompt_tokens.size(), true, true) >= 0)
                    {
                        llama_batch batch = llama_batch_get_one(prompt_tokens.data(), prompt_tokens.size());
                        int decode_result = llama_decode(persistent_ctx, batch);
                        
                        if (decode_result == 0)
                        {
                            conversation_position = batch.n_tokens;
                            conversation_active = true;
                            std::cout << "[Daemon] ✓ Conversation context established, position: " << conversation_position << std::endl;
                        }
                        else
                        {
                            std::cout << "[Daemon] WARNING: Failed to establish conversation context" << std::endl;
                            cleanupPersistentContext();
                        }
                    }
                    else
                    {
                        std::cout << "[Daemon] WARNING: Failed to tokenize initial prompt for conversation setup" << std::endl;
                        cleanupPersistentContext();
                    }
                }
            }
        }
        else
        {
            // CONTINUATION MODE - Lightweight conversation continuation
            std::cout << "[Daemon] Using continuation mode - lightweight conversation" << std::endl;
            ai_response = generateResponseContinue(action, 400);

            // If continuation fails, fall back to initial mode
            if (ai_response.find("{\"error\"") != std::string::npos)
            {
                std::cout << "[Daemon] Continuation failed, falling back to initial mode..." << std::endl;
                cleanupPersistentContext();
                
                // Recursive call with continue_conversation = false
                nlohmann::json fallback_request = request;
                fallback_request["continue_conversation"] = false;
                return processPlayerAction(fallback_request);
            }
        }

        // Post-process to extract only the player state (same for both modes) - ROBUST MARKER DETECTION
        std::string begin_marker_text = "<<BEGIN_PLAYER_STATE>>";
        std::string end_marker_text = "<<END_PLAYER_STATE>>";
        
        // Find the LAST occurrence of begin marker (in case there are multiple)
        size_t begin_marker = std::string::npos;
        size_t search_pos = 0;
        while ((search_pos = ai_response.find(begin_marker_text, search_pos)) != std::string::npos) {
            begin_marker = search_pos;
            search_pos++;
        }
        
        // Find the FIRST occurrence of end marker after the begin marker
        size_t end_marker = std::string::npos;
        if (begin_marker != std::string::npos) {
            end_marker = ai_response.find(end_marker_text, begin_marker + begin_marker_text.length());
        }
        
        if (begin_marker != std::string::npos && end_marker != std::string::npos) {
            // Extract only what's between the markers (excluding the markers themselves)
            size_t content_start = begin_marker + begin_marker_text.length();
            std::string clean_response = ai_response.substr(content_start, end_marker - content_start);
            
            // Efficient trimming of whitespace and newlines
            size_t start = clean_response.find_first_not_of(" \t\n\r");
            if (start == std::string::npos) {
                clean_response = "";
            } else {
                size_t end = clean_response.find_last_not_of(" \t\n\r");
                clean_response = clean_response.substr(start, end - start + 1);
            }
            
            std::cout << "[Daemon] Successfully extracted clean player state (excluding markers)" << std::endl;
            std::cout << "[Daemon] Begin marker found at position: " << begin_marker << std::endl;
            std::cout << "[Daemon] End marker found at position: " << end_marker << std::endl;
            std::cout << "[Daemon] Extracted content: " << clean_response.substr(0, 100) << "..." << std::endl;
            return clean_response;
        }
        
        // Fallback in case markers aren't found
        std::cout << "[Daemon] WARNING: Could not find state markers, returning raw response" << std::endl;
        std::cout << "[Daemon] " << ai_response << std::endl;

        return ai_response;
    }

    std::string handleRequest(const std::string &request_str)
    {
        try
        {
            nlohmann::json request = nlohmann::json::parse(request_str);
            std::string type = request["type"];

            if (type == "create_game")
            {
                return processGameCreation(request);
            }
            else if (type == "player_action")
            {
                return processPlayerAction(request);
            }
            else if (type == "reset_conversation")
            {
                std::cout << "[Daemon] Resetting conversation context..." << std::endl;
                cleanupPersistentContext();
                return "{\"status\":\"conversation_reset\",\"message\":\"Conversation context has been reset\"}";
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
                return "{\"error\":\"Unknown request type\"}";
            }
        }
        catch (const std::exception &e)
        {
            return "{\"error\":\"Failed to parse request: " + std::string(e.what()) + "\"}";
        }
    }

    void handleClient(int client_socket)
    {
        std::cout << "[Daemon] Handling client (fd=" << client_socket << ", thread_id=" << std::this_thread::get_id() << ")" << std::endl;

        char buffer[8192];
        ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);

        if (bytes_received > 0)
        {
            buffer[bytes_received] = '\0';
            std::string request(buffer);

            std::cout << "[Daemon] Received " << bytes_received << " bytes" << std::endl;
            std::cout << "[Daemon] Request preview: " << request.substr(0, 100) << "..." << std::endl;

            std::string response = handleRequest(request);

            std::cout << "[Daemon] Generated response (" << response.length() << " bytes)" << std::endl;
            std::cout << "[Daemon] Response preview: " << response.substr(0, 100) << "..." << std::endl;

            ssize_t bytes_sent = send(client_socket, response.c_str(), response.length(), 0);
            if (bytes_sent == -1)
            {
                std::cerr << "[Daemon] Failed to send response: " << strerror(errno) << std::endl;
            }
            else
            {
                std::cout << "[Daemon] Sent " << bytes_sent << " bytes successfully" << std::endl;
            }
        }
        else if (bytes_received == 0)
        {
            std::cout << "[Daemon] Client closed connection" << std::endl;
        }
        else
        {
            std::cerr << "[Daemon] Failed to receive data: " << strerror(errno) << std::endl;
        }

        close(client_socket);
        std::cout << "[Daemon] Client connection closed (fd=" << client_socket << ")" << std::endl;
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
            std::ofstream pid_file("../../../ai_daemon.pid");
            if (pid_file.is_open())
            {
                pid_file << getpid() << std::endl;
                pid_file.close();
                std::cout << "[Daemon] ✓ PID file created: ../../../ai_daemon.pid" << std::endl;
            }
            else
            {
                std::cerr << "[Daemon] WARNING: Failed to create PID file" << std::endl;
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
        std::cout << "[Daemon] ========== Starting AI Daemon ==========" << std::endl;
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
            std::thread client_thread(&AIDaemon::handleClient, this, client_socket);
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

        // Clean up persistent context first
        cleanupPersistentContext();

        if (model)
        {
            std::cout << "[Daemon] Freeing model..." << std::endl;
            llama_model_free(model);
            model = nullptr;
        }

        std::cout << "[Daemon] Freeing llama backend..." << std::endl;
        llama_backend_free();

        std::cout << "[Daemon] Removing PID file..." << std::endl;
        unlink("../../../ai_daemon.pid");

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

    std::cout << "[Daemon] ========== AI DAEMON STARTUP ==========" << std::endl;
    std::cout << "[Daemon] Starting AI Daemon with model: " << model_path << std::endl;
    std::cout << "[Daemon] Process ID: " << getpid() << std::endl;
    std::cout << "[Daemon] Working directory: " << std::filesystem::current_path() << std::endl;
    std::cout << "[Daemon] Test mode: " << (g_test_mode ? "ENABLED" : "DISABLED") << std::endl;
    std::cout << "[Daemon] Command line args: " << argc << std::endl;
    for (int i = 0; i < argc; i++)
    {
        std::cout << "[Daemon]   arg[" << i << "]: " << argv[i] << std::endl;
    }
    std::cout << "[Daemon] =============================================" << std::endl;
    std::cout.flush();

    try
    {
        std::cout << "[Daemon] Creating daemon instance..." << std::endl;
        AIDaemon daemon(model_path);

        std::cout << "[Daemon] Starting daemon run loop..." << std::endl;
        daemon.run();

        std::cout << "[Daemon] Daemon run loop completed" << std::endl;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Daemon] FATAL EXCEPTION: " << e.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "[Daemon] FATAL UNKNOWN EXCEPTION" << std::endl;
        return 2;
    }

    std::cout << "[Daemon] Shutting down..." << std::endl;
    return 0;
}
