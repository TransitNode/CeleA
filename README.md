A game bot for an old discontinued game called With Your Destiny / Supreme Destiny (WYD/SD). Contains various "hacks" and "exploits" mainly reversed through the game client with cheat engine and IDA pro. The bot's main function is to automate movement and combat in a dungeon called "Arcane". Built to be "internal", uses IMGUI for GUI which hooks dx9 endscene. Requires the dll to be injected into target process.
![image](https://github.com/user-attachments/assets/688a4820-0975-4ed2-81f2-7e6d8e25064b)
![image](https://github.com/user-attachments/assets/5f0c2bc9-9e77-4b45-9c6c-29db4d9a6970)
![image](https://github.com/user-attachments/assets/0bb90ba7-1d5f-4357-be42-5d150227d2f0)

The dungeon environment consists of exactly 36 interconnected rooms with static connections that never change during runtime. This fixed topology creates an ideal scenario where the O(n³) preprocessing cost of Floyd-Warshall can be amortized across thousands of path queries during a single session. The algorithm's ability to precompute all possible shortest paths eliminates the need for runtime pathfinding calculations, which is critical when the bot must make navigation decisions within milliseconds of detecting player presence.
The implementation leverages the algorithm's mathematical foundation where each iteration k considers whether routing through intermediate vertex k provides a shorter path between any pair of vertices i and j.

![lan_map](https://github.com/user-attachments/assets/871e6296-46d2-499b-bbe3-17f1df49dace)


During initalization we implement a caching strategy that recognizes the static nature of the room topology. When the system first starts, it attempts to load precomputed pathfinding data from persistent storage (game folder), eliminating the need to recalculate the Floyd-Warshall matrices on every execution. This approach transforms the algorithm's O(n³) complexity from a runtime cost to a one-time preprocessing overhead.
The preprocessing implementation follows the Floyd-Warshall approach with careful attention to numerical stability and a overflow prevention. The algorithm initializes direct connections with weight 1 and uses INT_MAX to represent impossible paths requiring careful handling during the relaxation step to prevent integer overflow. The implementation uses early termination optimization where paths that remain at INT_MAX after intermediate vertex consideration are skipped in subsequent iterations.

```c++
for (int k = 0; k < roomCount; k++) {
    for (int i = 0; i < roomCount; i++) {
        if (g_distanceMatrix[i][k] == INT_MAX) continue;
        for (int j = 0; j < roomCount; j++) {
            if (g_distanceMatrix[k][j] == INT_MAX) continue;
            int newDist = g_distanceMatrix[i][k] + g_distanceMatrix[k][j];
            if (newDist < g_distanceMatrix[i][j]) {
                g_distanceMatrix[i][j] = newDist;
                g_nextHopMatrix[i][j] = g_nextHopMatrix[i][k];
            }
        }
    }
}
```
Complete Feature List:
* Autonomous NPC Farming
* Real-Time Player Detection 
* Dynamic Room Selection (scoring algorithm considering NPC density, distance, and player presence)
* Floyd-Warshall Pathfinding
* Dynamic Rerouting (Real-time path adjustment when destination rooms become overtaken by other players)
* Weapon Type Detection (Recognizes and sets appropriate attack style, either physical attacks or magical spells)
* Combat State Tracking (Full combat detection through monitoring hooked function calls)
* SONIC MODE (Speedhack, only activates if no players have been present for 60 seconds)
* AoE Hack (Expands the range of AoE to cover a greater area, 128 tiles)
* No Cooldowns on spells
* Automatic spell page handling
* Bank Access (Open the bank anywhere)
* Open any shop (You can buy any item but cannot sell)
* Zoom hack (bypasses the games orginal zoom values) 
* Use griffin to fly anywhere, from any location.
* Crash server (Hooks the cpsock wrapper and sends a corrupted packet through sendonemessage function, causes server to enter an endless loop)


[early version of the bot]
![image](https://github.com/user-attachments/assets/b580a401-4259-42f0-abce-73859ceefaef)
