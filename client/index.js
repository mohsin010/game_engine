const HotPocket = require("hotpocket-js-client");
const readline = require("readline");

class GameClient {
  constructor() {
    this.client = null;
    this.currentGameId = null;
    this.conversationStarted = false;
    this.waitingForResponse = false;
    this.currentRequestType = null; // Track what type of request we sent
    this.gameWon = false; // Track if game is won and waiting for NFT claim
    this.rl = readline.createInterface({
      input: process.stdin,
      output: process.stdout
    });
  }

  async connect() {
    try {
      const userKeyPair = await HotPocket.generateKeys();
      this.client = await HotPocket.createClient(
        ["wss://localhost:8081"],
        userKeyPair
      );

      if (!(await this.client.connect())) {
        console.log("Connection failed.");
        return false;
      }

      console.log("HotPocket Connected.");
      this.setupEventListeners();
      return true;
    } catch (error) {
      console.error("Connection error:", error.message);
      return false;
    }
  }

  setupEventListeners() {
    this.client.on(HotPocket.events.contractOutput, (result) => {
      console.log("\nResponse received:");
      console.log("=".repeat(50));
      
      result.outputs.forEach((output) => {
        this.handleResponseByType(output);
      });
      
      console.log("=".repeat(50));
      this.waitingForResponse = false;
    });
    this.client.on(HotPocket.se)
  }

  handleResponseByType(output) {
    switch (this.currentRequestType) {
      case 'stats':
        this.formatStatsResponse(output);
        break;
      case 'list_games':
        this.formatListGamesResponse(output);
        break;
      case 'get_game_state':
        this.formatGameStateResponse(output);
        break;
      case 'game_action':
        this.formatGameActionResponse(output);
        break;
      case 'create_game':
        this.formatCreateGameResponse(output);
        break;
      case 'nft_mint':
        this.formatNFTMintResponse(output);
        break;
      default:
        // Fallback to generic handling
        console.log(output);
    }
  }

  formatStatsResponse(output) {
    console.log("System Statistics:");
    
    // Check if output is already an object
    if (typeof output === 'object' && output !== null) {
      if (output.type === 'stats') {
        this.formatStructuredStatsResponse(output);
        return;
      }
    }
    
    // Try to parse as JSON string if it's a string
    try {
      if (typeof output === 'string' && output.trim().startsWith('{')) {
        const parsedOutput = JSON.parse(output);
        
        if (parsedOutput.type === 'stats') {
          this.formatStructuredStatsResponse(parsedOutput);
          return;
        }
      }
    } catch (e) {
      // Not JSON, continue with raw output
    }
    
    // Fallback to raw output
    console.log(output);
  }

  formatStructuredStatsResponse(data) {
    console.log("─".repeat(40));
    
    // Model Information
    console.log("Model Information:");
    console.log(`   Path: ${data.model_path || 'N/A'}`);
    console.log(`   Progress: ${data.model_progress || 0}%`);
    console.log(`   Model Ready: ${data.model_ready ? 'Yes' : 'No'}`);
    
    console.log();
    
    // Daemon Status
    console.log("Daemon Status:");
    console.log(`   Status: ${data.daemon_status || 'Unknown'}`);
    
    if (data.daemon_details) {
      console.log(`   Details:`);
      console.log(`      Ready: ${data.daemon_details.status || 'Unknown'}`);
      console.log(`      Model Loaded: ${data.daemon_details.model_loaded ? 'Yes' : 'No'}`);
      console.log(`      Loading: ${data.daemon_details.model_loading ? 'In Progress' : 'Complete'}`);
    }
    
    console.log();
    
    // Game Statistics
    console.log("Game Statistics:");
    console.log(`   Total Games: ${data.total_games || 0}`);
    
    console.log("─".repeat(40));
  }

  formatListGamesResponse(output) {
    console.log("Available Games:");
    
    // Check if output is already an object
    if (typeof output === 'object' && output !== null) {
      if (output.type === 'gamesList' && Array.isArray(output.games)) {
        this.formatStructuredGamesList(output);
        return;
      }
    }
    
    // Try to parse JSON string if it's a string
    try {
      if (typeof output === 'string' && output.trim().startsWith('{')) {
        const parsedOutput = JSON.parse(output);
        if (parsedOutput.type === 'gamesList' && Array.isArray(parsedOutput.games)) {
          this.formatStructuredGamesList(parsedOutput);
          return;
        }
      }
      
      // Try to parse as array
      if (typeof output === 'string' && output.trim().startsWith('[')) {
        const games = JSON.parse(output);
        if (Array.isArray(games)) {
          games.forEach((game, index) => {
            console.log(`${index + 1}. ${game}`);
          });
          return;
        }
      }
    } catch (e) {
      // If parsing fails, show raw output
    }
    
    // Fallback to raw output
    console.log(output);
  }

  formatStructuredGamesList(data) {
    console.log("─".repeat(30));
    
    if (data.games && data.games.length > 0) {
      data.games.forEach((game, index) => {
        console.log(`${index + 1}. ${game}`);
      });
      console.log();
      console.log(`Total: ${data.games.length} game${data.games.length === 1 ? '' : 's'} available`);
    } else {
      console.log("No games available");
    }
    
    console.log("─".repeat(30));
  }

  formatGameStateResponse(output) {
    console.log("Game State Information:");
    
    // Check if output is already an object
    if (typeof output === 'object' && output !== null) {
      if (output.type === 'gameState' && output.state) {
        this.formatStructuredGameState(output);
        return;
      }
    }
    
    // Try to parse JSON string if it's a string
    try {
      if (typeof output === 'string' && output.trim().startsWith('{')) {
        const parsedOutput = JSON.parse(output);
        if (parsedOutput.type === 'gameState' && parsedOutput.state) {
          this.formatStructuredGameState(parsedOutput);
          return;
        }
      }
    } catch (e) {
      // Not JSON, continue with marker processing
    }
    
    // Try to extract game state markers
    if (typeof output === 'string') {
      const beginMarker = '<<BEGIN_PLAYER_STATE>>';
      const endMarker = '<<END_PLAYER_STATE>>';
      
      const beginIndex = output.indexOf(beginMarker);
      const endIndex = output.indexOf(endMarker);
      
      if (beginIndex !== -1 && endIndex !== -1) {
        const gameState = output.substring(beginIndex + beginMarker.length, endIndex).trim();
        console.log("Current Game State:");
        console.log(gameState);
        
        // Show any additional content outside the markers
        const beforeMarker = output.substring(0, beginIndex).trim();
        const afterMarker = output.substring(endIndex + endMarker.length).trim();
        
        if (beforeMarker) {
          console.log("\nAdditional Information:");
          console.log(beforeMarker);
        }
        if (afterMarker) {
          console.log("\nAdditional Information:");
          console.log(afterMarker);
        }
        
        // Check if game is won and offer NFT claim
        this.checkForGameWin(gameState);
        return;
      }
    }
    
    // Fallback to raw output
    console.log(output);
  }

  formatStructuredGameState(data) {
    console.log("─".repeat(40));
    
    if (data.game_id) {
      console.log(`Game: ${data.game_id}`);
      console.log();
    }
    
    if (data.state) {
      console.log("Current State:");
      console.log("─".repeat(30));
      
      // Parse and format game state
      const stateLines = data.state.split('\n').filter(line => line.trim());
      stateLines.forEach(line => {
        if (line.includes(':')) {
          const [key, value] = line.split(':', 2); // Only split on first colon
          const cleanKey = key.trim().replace('Player_', '').replace('Game_', '');
          const cleanValue = value.trim();
          
          // Special formatting for Messages (parse JSON array)
          if (cleanKey === 'Messages' && cleanValue.startsWith('[')) {
            try {
              const messages = JSON.parse(cleanValue);
              console.log(`${cleanKey}:`);
              messages.forEach((message, index) => {
                console.log(`   ${message}`);
              });
            } catch (e) {
              console.log(`${cleanKey}: ${cleanValue}`);
            }
          } else {
            console.log(`${cleanKey}: ${cleanValue}`);
          }
        }
      });
      
      console.log("─".repeat(30));
      
      // Check if game is won and offer NFT claim
      this.checkForGameWin(data.state);
    }
    
    console.log("─".repeat(40));
  }

  formatGameActionResponse(output) {
    console.log("Game Action Result:");
    
    // Check if output is already an object
    if (typeof output === 'object' && output !== null) {
      if (output.type === 'consensus' && output.game_id) {
        this.formatStructuredGameResponse(output);
        return;
      }
    }
    
    // Try to parse as JSON string if it's a string
    try {
      if (typeof output === 'string' && (output.trim().startsWith('{') || output.trim().startsWith('{'))) {
        const parsedOutput = JSON.parse(output);
        
        // Handle structured consensus response
        if (parsedOutput.type === 'consensus' && parsedOutput.game_id) {
          this.formatStructuredGameResponse(parsedOutput);
          return;
        }
      }
    } catch (e) {
      // Not JSON, continue with string processing
    }
    
    // Try to extract game state markers for text responses
    if (typeof output === 'string') {
      const beginMarker = '<<BEGIN_PLAYER_STATE>>';
      const endMarker = '<<END_PLAYER_STATE>>';
      
      // Use the same extraction logic as the structured response handler
      const extractedState = this.extractLastGameState(output);
      
      if (extractedState) {
        // Find the last pair of markers to separate narrative from game state
        const lastBeginIndex = output.lastIndexOf(beginMarker);
        const lastEndIndex = output.lastIndexOf(endMarker);
        
        if (lastBeginIndex !== -1 && lastEndIndex !== -1 && lastEndIndex > lastBeginIndex) {
          // Show narrative/story content before the last state markers
          const narrative = output.substring(0, lastBeginIndex).trim();
          if (narrative) {
            console.log("Story:");
            console.log(narrative);
            console.log();
          }
          
          // Show extracted and formatted game state
          console.log("Updated Game State:");
          console.log("─".repeat(30));
          
          // Parse and format the extracted game state
          const stateLines = extractedState.split('\n').filter(line => line.trim());
          stateLines.forEach(line => {
            if (line.includes(':')) {
              const [key, ...valueParts] = line.split(':');
              const cleanKey = key.trim().replace('Player_', '').replace('Game_', '');
              const cleanValue = valueParts.join(':').trim();
              
              // Special formatting for Messages and Inventory (parse JSON arrays)
              if ((cleanKey === 'Messages' || cleanKey === 'Inventory') && cleanValue.startsWith('[')) {
                try {
                  const items = JSON.parse(cleanValue);
                  if (cleanKey === 'Messages') {
                    console.log(`${cleanKey}:`);
                    items.forEach((message) => {
                      console.log(`   ${message}`);
                    });
                  } else if (cleanKey === 'Inventory') {
                    console.log(`${cleanKey}: ${items.length > 0 ? items.join(', ') : 'Empty'}`);
                  }
                } catch (e) {
                  console.log(`${cleanKey}: ${cleanValue}`);
                }
              } else {
                console.log(`${cleanKey}: ${cleanValue}`);
              }
            }
          });
          
          console.log("─".repeat(30));
          
          // Show any content after the last state markers
          const afterState = output.substring(lastEndIndex + endMarker.length).trim();
          if (afterState) {
            console.log("\nAdditional Notes:");
            console.log(afterState);
          }
          
          // Check if game is won and offer NFT claim
          this.checkForGameWin(extractedState);
          return;
        }
      }
    }
    
    // Fallback to raw output
    console.log(output);
  }

  formatStructuredGameResponse(data) {
    // Show consensus result with appropriate styling
    const resultText = data.action_result === 'success' ? 'SUCCESS' : 
                       data.action_result === 'failed' ? 'FAILED' : 'PENDING';
    
    console.log(`[${resultText}] Action Result: ${data.action_result.toUpperCase()}`);
    console.log(`Confidence: ${(data.confidence * 100).toFixed(1)}%`);
    console.log(`Decision: ${data.decision}`);
    
    if (data.player_action) {
      console.log(`Your Action: "${data.player_action}"`);
    }
    
    // Parse and show consensus details if available
    if (data.details) {
      try {
        const details = JSON.parse(data.details);
        console.log(`Consensus: ${details.validVotes}/${details.totalVotes} valid votes`);
      } catch (e) {
        // Ignore parsing errors for details
      }
    }
    
    console.log(); // Add spacing
    
    // Show game state in a structured way
    if (data.game_state) {
      console.log("Current Game State:");
      console.log("─".repeat(30));
      
      // Extract the LAST pair of markers (most recent state)
      const gameStateText = this.extractLastGameState(data.game_state);
      
      if (gameStateText) {
        // Parse and format game state
        const stateLines = gameStateText.split('\n').filter(line => line.trim());
        stateLines.forEach(line => {
          if (line.includes(':')) {
            const [key, ...valueParts] = line.split(':'); // Handle multiple colons in value
            const cleanKey = key.trim().replace('Player_', '').replace('Game_', '');
            const cleanValue = valueParts.join(':').trim();
            
            // Add appropriate prefix for different state types
            let prefix = '[INFO]';
            if (cleanKey === 'Location') prefix = '[LOC]';
            else if (cleanKey === 'Health') prefix = '[HP]';
            else if (cleanKey === 'Score') prefix = '[SCORE]';
            else if (cleanKey === 'Inventory') prefix = '[INV]';
            else if (cleanKey === 'Status') prefix = '[STATUS]';
            else if (cleanKey === 'Messages') prefix = '[MSG]';
            else if (cleanKey === 'Turn_Count') prefix = '[TURN]';
            
            // Special formatting for Messages and Inventory (parse JSON arrays)
            if ((cleanKey === 'Messages' || cleanKey === 'Inventory') && cleanValue.startsWith('[')) {
              try {
                const items = JSON.parse(cleanValue);
                if (cleanKey === 'Messages') {
                  console.log(`${prefix} ${cleanKey}:`);
                  items.forEach((message) => {
                    console.log(`   - ${message}`);
                  });
                } else if (cleanKey === 'Inventory') {
                  console.log(`${prefix} ${cleanKey}: ${items.length > 0 ? items.join(', ') : 'Empty'}`);
                }
              } catch (e) {
                console.log(`${prefix} ${cleanKey}: ${cleanValue}`);
              }
            } else {
              console.log(`${prefix} ${cleanKey}: ${cleanValue}`);
            }
          }
        });
      } else {
        // Fallback to raw game state if extraction fails
        console.log(data.game_state);
      }
      
      console.log("─".repeat(30));
      
      // Check if game is won and offer NFT claim for structured responses
      if (gameStateText) {
        this.checkForGameWin(gameStateText);
      }
    }
  }

  // Helper method to check if game is won and offer NFT claim
  checkForGameWin(gameStateText) {
    // Parse the game state to check for won status
    const stateLines = gameStateText.split('\n').filter(line => line.trim());
    let isGameWon = false;
    
    stateLines.forEach(line => {
      if (line.includes(':')) {
        const [key, value] = line.split(':', 2);
        const cleanKey = key.trim().replace('Player_', '').replace('Game_', '');
        const cleanValue = value.trim();
        
        if (cleanKey === 'Status' && cleanValue.toLowerCase() === 'won') {
          isGameWon = true;
        }
      }
    });
    
    if (isGameWon) {
      console.log("\n" + "=".repeat(50));
      console.log("*** CONGRATULATIONS! YOU WON THE GAME! ***");
      console.log("=".repeat(50));
      console.log("You have successfully completed your quest!");
      console.log("You are now eligible to claim your NFT reward.");
      console.log();
      console.log("Would you like to claim your NFT? (y/n)");
      
      // Set a flag to handle NFT claim in the next user input
      this.gameWon = true;
    }
  }

  // Helper method to extract the last (most recent) game state from duplicate markers
  extractLastGameState(gameStateText) {
    const beginMarker = '<<BEGIN_PLAYER_STATE>>';
    const endMarker = '<<END_PLAYER_STATE>>';
    
    // Find all BEGIN markers
    const beginIndices = [];
    let searchIndex = 0;
    while ((searchIndex = gameStateText.indexOf(beginMarker, searchIndex)) !== -1) {
      beginIndices.push(searchIndex);
      searchIndex += beginMarker.length;
    }
    
    // Find all END markers
    const endIndices = [];
    searchIndex = 0;
    while ((searchIndex = gameStateText.indexOf(endMarker, searchIndex)) !== -1) {
      endIndices.push(searchIndex);
      searchIndex += endMarker.length;
    }
    
    // If we have matching pairs, get the last one
    if (beginIndices.length > 0 && endIndices.length > 0) {
      const lastBeginIndex = beginIndices[beginIndices.length - 1];
      const lastEndIndex = endIndices[endIndices.length - 1];
      
      // Make sure the last end comes after the last begin
      if (lastEndIndex > lastBeginIndex) {
        const extractedState = gameStateText.substring(
          lastBeginIndex + beginMarker.length, 
          lastEndIndex
        ).trim();
        
        // Clean up any escaped newlines
        return extractedState.replace(/\\n/g, '\n');
      }
    }
    
    return null; // Return null if extraction fails
  }

  formatCreateGameResponse(output) {
    console.log("Game Creation Result:");
    console.log(output);
  }

  // Helper method to send NFT mint request to server
  async sendNFTMintRequest(gameId) {
    console.log("Sending NFT mint request to server...");
    
    const msg = {
      mint_nft: gameId
    };
    
    try {
      // Use readonly request for NFT mint (faster, no consensus needed)
      this.currentRequestType = 'nft_mint';
      const result = await this.client.submitContractReadRequest(
        JSON.stringify(msg)
      );
      // console.log(result);
      console.log("\nNFT Mint Response received:");
      console.log("=".repeat(100));
      this.handleResponseByType(result);
      console.log("=".repeat(100));
      
      // console.log("NFT mint request completed!", result);
      return true;
    } catch (error) {
      console.error("Failed to send NFT mint request:", error.message);
      return false;
    }
  }

  formatNFTMintResponse(output) {
    console.log("NFT Mint Response:");
    console.log("=".repeat(50));
    
    // Show raw response for debugging
    console.log("Raw Response:");
    console.log(JSON.stringify(output, null, 2));
    console.log("-".repeat(30));
    
    // Check if output is already an object
    if (typeof output === 'object' && output !== null) {
      if (output.type === 'nftMintResult') {
        this.formatStructuredNFTMintResponse(output);
        return;
      } else if (output.type === 'error') {
        this.formatNFTMintErrorResponse(output);
        return;
      }
    }
    
    // Try to parse as JSON string if it's a string
    try {
      if (typeof output === 'string' && output.trim().startsWith('{')) {
        const parsedOutput = JSON.parse(output);
        
        if (parsedOutput.type === 'nftMintResult') {
          this.formatStructuredNFTMintResponse(parsedOutput);
          return;
        } else if (parsedOutput.type === 'error') {
          this.formatNFTMintErrorResponse(parsedOutput);
          return;
        }
      }
    } catch (e) {
      // Not JSON, continue with raw output
    }
    
    // Fallback to raw output
    console.log(output);
    console.log("=".repeat(50));
  }

  formatStructuredNFTMintResponse(data) {
    if (data.already_minted) {
      console.log("[INFO] NFTs Already Minted");
      console.log(`Game ID: ${data.game_id}`);
      console.log(`Message: ${data.message}`);
      console.log("\nYour NFTs were previously minted for this game completion.");
    } else if (data.success) {
      console.log("[SUCCESS] NFT Mint Completed!");
      console.log(`Game ID: ${data.game_id}`);
      
      // Support both old and new format for transaction hash
      const txHash = data.batch_tx_hash || data.tx_hash;
      if (txHash) {
        console.log(`Batch Transaction Hash: ${txHash}`);
      }
      
      console.log(`Mint Timestamp: ${new Date(data.mint_timestamp * 1000).toLocaleString()}`);
      
      // Support both old and new format for counts
      if (data.total_requested !== undefined) {
        console.log(`Total Requested: ${data.total_requested}`);
        console.log(`Successful Mints: ${data.successful_mints}`);
        console.log(`Failed Mints: ${data.failed_mints}`);
      } else if (data.minted_count !== undefined) {
        console.log(`Total Items Minted: ${data.minted_count}`);
      }
      
      console.log("\nMinted NFT Items:");
      console.log("-".repeat(40));
      
      data.minted_items.forEach((item, index) => {
        console.log(`${index + 1}. ${item.name}`);
        
        // Support both old and new format for token ID
        const tokenId = item.nft_token_id || item.token_id;
        if (tokenId) {
          console.log(`   Token ID: ${tokenId}`);
        }
        
        // Show individual transaction hash if available (new format)
        if (item.transaction_hash) {
          console.log(`   Transaction Hash: ${item.transaction_hash}`);
        }
        
        // Show metadata URI if available (new format)
        if (item.metadata_uri) {
          console.log(`   Metadata URI: ${item.metadata_uri}`);
        }
        
        // Show taxon if available (old format)
        if (item.taxon !== undefined) {
          console.log(`   Taxon: ${item.taxon}`);
        }
        
        console.log();
      });
      
      console.log("Congratulations! Your inventory items have been minted as NFTs!");
    } else {
      console.log("[FAILED] NFT Mint Failed");
      console.log(`Game ID: ${data.game_id}`);
      console.log(`Error: ${data.error || 'Unknown error occurred'}`);
    }
    
    console.log("=".repeat(50));
  }

  formatNFTMintErrorResponse(data) {
    console.log("[ERROR] NFT Mint Error");
    console.log(`Error: ${data.error}`);
    console.log("=".repeat(50));
  }

  async handleMintNFT() {
    let gameId = await this.getUserInput("Enter game ID for NFT mint: ");
    if (!gameId && this.currentGameId) {
      gameId = this.currentGameId;
    }
    if (!gameId) {
      console.log("[ERROR] No game ID provided.");
      return;
    }
    
    console.log(`Requesting NFT mint for game: ${gameId}`);
    await this.sendNFTMintRequest(gameId);
  }

  showMenu() {
    console.log("\n=== HotPocket Game Client ===");
    console.log("Current Game:", this.currentGameId || "None");
    console.log("Conversation Started:", this.conversationStarted ? "Yes" : "No");
    console.log("\nSelect an option:");
    console.log("1. Get Stats");
    console.log("2. Create Game");
    console.log("3. List Games");
    console.log("4. Get Game State");
    console.log("5. Play Game");
    console.log("6. Mint NFT");
    console.log("7. Exit");
    console.log();
  }

  async getUserInput(prompt) {
    return new Promise((resolve) => {
      this.rl.question(prompt, (answer) => {
        resolve(answer.trim());
      });
    });
  }

  async sendMessage(msg, requestType) {
    this.currentRequestType = requestType;
    this.waitingForResponse = true;
    this.client.submitContractInput(JSON.stringify(msg));
    
    // Wait for response
    while (this.waitingForResponse) {
      await new Promise(resolve => setTimeout(resolve, 100));
    }
  }

  async handleStats() {
    const msg = { type: "stat" };
    await this.sendMessage(msg, 'stats');
  }

  async handleCreateGame() {
    const gameDescription = await this.getUserInput("Enter game description: ");
    const msg = {
      create_game: gameDescription
    };
    await this.sendMessage(msg, 'create_game');
  }

  async handleListGames() {
    const msg = {
      list_games: true
    };
    await this.sendMessage(msg, 'list_games');
  }

  async handleGetGameState() {
    let gameId = await this.getUserInput("Enter game ID: ");
    if (!gameId && this.currentGameId) {
      gameId = this.currentGameId;
    }
    if (!gameId) {
      console.log("[ERROR] No game ID provided.");
      this.showMenu();
      return;
    }
    
    const msg = {
      get_game_state: gameId
    };
    await this.sendMessage(msg, 'get_game_state');
    
    // After getting game state, check if user wants to claim NFT
    if (this.gameWon) {
      while (this.gameWon) {
        const response = await this.getUserInput("Claim NFT? (y/n): ");
        
        if (response.toLowerCase() === 'y' || response.toLowerCase() === 'yes') {
          console.log("Processing NFT mint request...");
          await this.sendNFTMintRequest(gameId);
          this.gameWon = false; // Reset after handling
          break;
        } else if (response.toLowerCase() === 'n' || response.toLowerCase() === 'no') {
          console.log("NFT claim declined. You can claim it later if needed.");
          this.gameWon = false; // Reset after handling
          break;
        } else {
          console.log("[ERROR] Please enter 'y' for yes or 'n' for no.");
        }
      }
    }
  }

  async handleGameAction() {
    let gameId = this.currentGameId;
    
    // If no current game, ask for game ID
    if (!gameId) {
      gameId = await this.getUserInput("Enter game ID: ");
      if (!gameId) {
        console.log("[ERROR] No game ID provided.");
        return;
      }
    }

    // Enter continuous gameplay loop
    while (true) {
      let promptText = "Enter your action (or 'menu' to return to main menu): ";
      
      // Check if we're waiting for NFT claim response
      if (this.gameWon) {
        promptText = "Claim NFT? (y/n) or 'menu' to return: ";
      }
      
      const action = await this.getUserInput(promptText);
      
      // Allow user to return to main menu
      if (action.toLowerCase() === 'menu') {
        console.log("Returning to main menu...");
        this.gameWon = false; // Reset game won state
        break;
      }
      
      // Handle NFT claim response
      if (this.gameWon) {
        if (action.toLowerCase() === 'y' || action.toLowerCase() === 'yes') {
          console.log("Processing NFT mint request...");
          await this.sendNFTMintRequest(gameId);
          this.gameWon = false; // Reset after handling
          continue;
        } else if (action.toLowerCase() === 'n' || action.toLowerCase() === 'no') {
          console.log("NFT claim declined. You can claim it later if needed.");
          this.gameWon = false; // Reset after handling
          continue;
        } else {
          console.log("[ERROR] Please enter 'y' for yes, 'n' for no, or 'menu' to return.");
          continue;
        }
      }
      
      if (!action) {
        console.log("[ERROR] No action provided. Try again or type 'menu' to return.");
        continue;
      }

      // Automatically determine if this is the first action or continuation
      const isFirstAction = !this.conversationStarted || this.currentGameId !== gameId;
      
      if (isFirstAction) {
        console.log("Starting new game:", gameId);
        this.currentGameId = gameId;
        this.conversationStarted = true;
      } else {
        console.log("Continuing playing game:", gameId);
      }

      const msg = {
        game_id: gameId,
        action: action,
        continue_conversation: isFirstAction ? "false" : "true"
      };
      
      await this.sendMessage(msg, 'game_action');
      
      // After successful action, continue the loop to prompt for next action
      console.log("\nReady for your next action!");
    }
  }

  async run() {
    console.log("Starting HotPocket Interactive Game Client...");
    
    if (!(await this.connect())) {
      this.rl.close();
      return;
    }

    this.showMenu();

    while (true) {
      const choice = await this.getUserInput("Choose option (1-7): ");

      switch (choice) {
        case "1":
          await this.handleStats();
          break;
        case "2":
          await this.handleCreateGame();
          break;
        case "3":
          await this.handleListGames();
          break;
        case "4":
          await this.handleGetGameState();
          break;
        case "5":
          await this.handleGameAction();
          break;
        case "6":
          await this.handleMintNFT();
          break;
        case "7":
          console.log("Goodbye!");
          this.client.close();
          this.rl.close();
          return;
        default:
          console.log("[ERROR] Invalid choice. Please select 1-7.");
      }
      
      // Show menu after each operation
      this.showMenu();
    }
  }
}

// Start the application
const client = new GameClient();
client.run().catch(console.error);
