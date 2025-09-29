# HotPocket Interactive Game Client

A minimal interactive console application for testing HotPocket game functionality.

## Features

1. **Get Stats** - Retrieve system statistics
2. **Create Game** - Create a new game with custom description
3. **List Games** - Show all available games
4. **Get Game State** - Retrieve current state of a specific game
5. **Game Action (Start)** - Perform first action in a game (`continue_conversation: false`)
6. **Game Action (Continue)** - Perform subsequent actions (`continue_conversation: true`)

## Usage

1. Install dependencies:
   ```bash
   npm install
   ```

2. Make sure your HotPocket server is running on `ws://localhost:8081`

3. Run the interactive client:
   ```bash
   npm start
   ```

4. Follow the menu prompts to interact with your HotPocket game server.

## Workflow

1. Start with option **5** (Game Action Start) for your first action in a game
2. Use option **6** (Game Action Continue) for all subsequent actions in the same game
3. The client automatically tracks your current game and conversation state

## Example Game Actions

- "I carefully pick up the torch from the ground and examine it closely"
- "I move deeper into the cave, using the torch to illuminate the path ahead"
- "I search the chamber for any hidden passages or interesting objects"
