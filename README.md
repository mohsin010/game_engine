# AI Game Engine (HotPocket / Evernode)

Deterministic, AI-assisted text adventure engine running as a HotPocket contract. Uses a long-lived AI daemon (LLM kept in memory), an AI Jury validation layer for consensus on state transitions, and optional NFT minting of player achievements on Xahau.

Reference (AI Jury details): https://github.com/mohsin010/c_ai_jury/blob/main/README.md

## Key Features
- Persistent AI daemon (eliminates multi‑minute model reloads)
- Structured game world + incremental player state updates
- AI Jury consensus validation for player actions (valid / invalid + confidence)
- Automatic extraction of winning inventory → NFT metadata JSON
- Deterministic file persistence (world, state, nft data)
- Chunked model downloader (resumable across rounds)

## Core Components
- ai_contract.cpp: HotPocket contract (message routing, game state mgmt, jury integration, NFT trigger)
- ai_daemon.cpp: TCP AI inference service (model load + generation + continuation)
- ai_service_client.h: Lightweight client to daemon (create_game / player_action / status)
- ai_jury_module.*: Consensus validation (wraps AI decision engine)
- nft_minting_client.* + ValuableItemExtractor (inventory → NFT mint batch)
- game_data/: persisted world/state + nft_*.json
- model/: downloaded GGUF model (e.g. gpt-oss-20b-Q5_K_M.gguf)

## Game State Format (excerpt)
Player_Location: <string>
Player_Health: <int>
Player_Score: <int>
Player_Inventory: <comma/structured list>
Game_Status: active|won|lost
Messages: ["Narrative line(s)"]
Turn_Count: <int>

AI daemon emits state delimited internally by markers; contract stores only the cleaned block.

## Message Protocol (Client → Contract)
JSON messages (examples):
- Stats: {"type":"stat"}
- Create Game: {"create_game":"Ancient cave survival"}
- List Games: {"list_games":""}
- Get Game State: {"get_game_state":"game_1_12345"}
- Player Action: {"game_id":"game_1_12345","action":"move north","continue_conversation":"true"}
- Mint NFT (after win): {"mint_nft":"game_1_12345"}

## Action Validation Flow (player_action)
1. Client sends action
2. Contract loads world + prior state
3. AI daemon proposes new state
4. AI Jury request emitted (validate_game_action)
5. Nodes vote (valid / invalid + confidence)
6. Majority valid → state kept; invalid → revert to old state
7. If Game_Status: won → inventory extracted → nft_<game>.json

## NFT Minting Flow
1. Winning state triggers inventory extraction (nft_<game>.json)
2. mint_nft request loads JSON & submits mint batch via NFT minting client
3. Result appended (transaction hashes, token IDs)
4. Updated JSON persisted (status minted)

## System Requirements (Requirements depend on the selected LLM—refer to its published specifications)
Minimum (single game session, CPU inference):
- OS: Linux x86_64 (Ubuntu 20.04+ or equivalent)
- CPU: 4 physical cores (8 threads) with AVX2
- RAM: 16 GB (20B Q5_K_M ~11 GB model + runtime overhead)
- Disk: 15 GB free (11 GB model file + ~2 GB temp/cache + state)
- Network: 10 Mbps sustained (model download is resumable)

Recommended (multiple concurrent games / faster turns):
- CPU: 8+ physical cores (modern >3.0 GHz, AVX2/AVX512 where available)
- RAM: 24–32 GB (headroom for jury + multiple contexts)
- Disk: 30 GB free (future multi-model / logs)
- Network: 50+ Mbps (faster initial provisioning)
- Optional GPU: ≥8 GB VRAM (llama.cpp offload of ~10–15 layers gives 2–3× speedup)

Operational Notes:
- Model file observed: gpt-oss-20b-Q5_K_M.gguf ≈ 11 GB.
- Each additional simultaneously active game adds roughly 200–300 MB transient memory (prompt + generated tokens) until flushed.
- Ensure swap is enabled if running at minimum RAM to avoid OOM during first load.

## Build Prerequisites
(See System Requirements for hardware sizing.)
- g++ (C++17), CMake ≥ 3.16
- OpenSSL dev libraries
- Git, curl
- (Optional) CUDA or other GPU toolchain for llama.cpp offload build

## Building
Integrated helper script:
./build_integrated.sh
Outputs placed under build/integrated/(engine_build|jury_build) and hotpocket_deployment/.

Model will download incrementally during successive contract executions until complete.

## Environment Variables
- MINTER_WALLET_SEED (required for contract NFT minting)
- PINATA_JWT, PINATA_GATEWAY (for media/metadata if using IPFS via signer service)
- XAHAU_NETWORK (e.g. wss://xahau-test.net)

Never commit real secrets (.env is excluded).

## Running
1. Build (see above)
2. Ensure HotPocket node configured; place binaries from hotpocket_deployment/
3. Export MINTER_WALLET_SEED in runtime environment
4. Start HotPocket; contract will: (a) start model download; (b) start daemon once model complete
5. From client/: npm install && npm start (interactive menu)
6. Use Game Action Start (first) then Continue for subsequent moves

## File Persistence
- game_data/game_world_<id>.txt
- game_data/game_state_<id>.txt
- game_data/nft_<id>.json
- model/<model>.gguf (persisted across rounds)

## Troubleshooting
- Model still loading: stat shows daemon_status=running, model_ready=false
- No daemon: ensure model fully downloaded & AIDaemon binary copied
- Invalid action: jury returns decision invalid; state reverted
- NFT not minting: verify MINTER_WALLET_SEED and nft_<id>.json exists with status won

## Roadmap (Condensed)
- Reinstate distributed NFT coordination
- Multi-model tier (fast + large) switching
- Deterministic sampling parameter snapshotting
- Action schema enforcement / grammar constraints

## License & Attribution
Uses llama.cpp and third-party GGUF model (see model source license). Ensure compliance with model provider terms.

---
For deeper AI Jury internals see: https://github.com/mohsin010/c_ai_jury/blob/main/README.md
