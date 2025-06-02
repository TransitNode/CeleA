#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define STRICT                   // Enforces type safety in Windows API functions
#define VC_EXTRALEAN             // Excludes rarely-used parts of Windows headers
#define DIRECT3D_VERSION 0x0900  // Target specific D3D version for leaner headers
#include <Windows.h>
#include <vector>
#include <string>
#include <iostream>
#include <cmath>
#include <thread>
#include <mutex>
#include <chrono>
#include <unordered_map>
#include <deque>
#include <random>
#include <map>
#include <algorithm>
#include <unordered_set>
#include <fstream>
#include "Core/globals.h"
#include <set>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx9.h"
#include "imgui/imgui_impl_win32.h"
#include "hooks/hooks.h"  // for accessing the hooks namespace
#include "./minhook/MinHook.h"

// Forward declarations
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK WNDProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

// forward declare these...
bool GetPlayerPosition(int& x, int& y);
bool IsInLanArea();
bool IsPlayerMoving();

//=========================================================================================
// Entity Scanner Structures and Definitions
//=========================================================================================

// Entity information structure
struct EntityInfo {
	uintptr_t address = 0;
	uint32_t id = 0;
	std::string name;
	float posX = 0.0f;
	float posY = 0.0f;
	int currentHP = 0;
	bool isNPC = false;
	bool isMoving = false;
	bool isLocalPlayer = false;
	float distance = 0.0f;
};

// Scene type enumeration - Core game state identifiers
// These values represent different game scenes/states in the application's state machine
enum ESCENE_TYPE
{
	ESCENE_NONE = 0x0,           // Null state - used during transitions or uninitialized scenes
	ESCENE_FIELD = 0x7530,       // Primary gameplay scene where combat/movement occurs
	// This is the main "in-game" state - all other scenes are menu/UI states
	// Value 0x7530 likely chosen to avoid collisions with other enums
	ESCENE_SELCHAR = 0x7531,     // Character selection screen after login
	// Displays available characters for the authenticated account
	ESCENE_LOGIN = 0x7532,       // Login screen - first user interaction point
	// Handles account authentication before server selection
	ESCENE_CREATE_ACCOUNT = 0x7533, // New account creation interface
	// Separate from login to maintain clean state separation
	ESCENE_SELECT_SERVER = 0x7534,  // Server selection menu after successful login
	// Allows choosing game world/instance
	ESCENE_DEMO = 0x7535,          // Demo/tutorial scene for new players
	// Separate from main game field for controlled environment
};

// Memory offsets
namespace Offsets {
	namespace Game {
		const char* MODULE_NAME = "wyd.exe";            // Target process module identifier
		// Used for base address calculation in memory scanning

		const uint32_t CURRENT_SCENE = 0x1BD0EEC;      // Global scene manager pointer
		// Points to active TMScene instance
		// Critical for state management and scene transitions
		// Read as: [base + 0x1BD0EEC] -> TMScene*

		const uint32_t AUTO_ATTACK = 0x525408;         // Auto-attack mode state variable
		// Value mapping:
		// 0: Disabled (manual attacks only)
		// 1: Physical auto-attack enabled
		// 2: Magical auto-attack enabled
		// Used by bot logic for combat automation

		const uint32_t HIT_COUNTER = 0x2F7A6C;         // Physical attack sequence counter
		// Increments on each physical projectile launch
		// Does NOT increment for:
		// - Magical attacks
		// Used for attack timing calculations 00597E10

		const uint32_t NOT_MOVING_HUNTING = 0x597E10; // NOT_MOVING_HUNTING, this is the name of a automatic movement mode, that we usually want disabled
		// to disable we have to write the value "2" into this address,
		// to enable NOT_MOVING_HUNTING, we write the value "1"
		// to enable MOVING_HUNTING, we write the value "0"
		// this is basically a movement mode usually coupled with the auto attack, moving hunting, will move the character to the nearest enemy
		// not moving hunting, is the name of the second mode, that moves to the nearest enemy, then returns to the original position where it was originally enabled. 
		// we usually want these disabled.


		//is actually a float value, default is 1000, as float, then decreasing this value increases our movement speed
		// 900 is a good enhanced value without side effects, 200 is as low we can go without encountering some side effects.
		//what this really does seems to control how often movement updates in-engine are sent and not speed in itself. 
		const uint32_t MOVEMENT_SPEED = 0x173608;
	}

	namespace TMScene {
		const uint32_t SCENE_TYPE = 0x24;               // ESCENE_TYPE enum value
		// Read as: [TMScene* + 0x24] -> ESCENE_TYPE
		// Used to verify scene context before operations

		const uint32_t MY_HUMAN = 0x4C;                 // Pointer to player's TMHuman instance
		// Read as: [TMScene* + 0x4C] -> TMHuman*
		// Primary player entity reference for:
		// - Movement commands
		// - Health/mana monitoring
		// - Buff/debuff tracking

		const uint32_t HUMAN_CONTAINER = 0x34;          // Pointer to human entity container
		// Read as: [TMScene* + 0x34] -> TreeNode*
		// Container for all TMHuman entities in scene
		// Used for entity enumeration and targeting

		const uint32_t TARGETX = 0x27750;               // Target X coordinate for movement
		// Write as: [TMScene* + 0x27750] = float (X position)
		// Triggers automatic pathfinding to specified location
		// Part of click-to-move functionality

		const uint32_t TARGETY = 0x27754;               // Target Y coordinate for movement
		// Write as: [TMScene* + 0x27754] = float (Y position)
		// Must be written in conjunction with TARGETX
		// Invalid coordinates may be ignored by pathfinder
	}

	namespace TreeNode {
		const uint32_t TOP = 0x04;                      // Parent node pointer
		// Read as: [TreeNode* + 0x04] -> TreeNode*
		// NULL indicates root node

		const uint32_t PREV_LINK = 0x08;                // Previous sibling pointer
		// Read as: [TreeNode* + 0x08] -> TreeNode*
		// Forms doubly-linked list with siblings

		const uint32_t NEXT_LINK = 0x0C;                // Next sibling pointer
		// Read as: [TreeNode* + 0x0C] -> TreeNode*
		// Used for traversing sibling nodes

		const uint32_t DOWN = 0x10;                     // First child node pointer
		// Read as: [TreeNode* + 0x10] -> TreeNode*
		// Entry point for child traversal

		const uint32_t DELETED_FLAG = 0x14;             // Deletion marker flag
		// Read as: [TreeNode* + 0x14] -> char
		// Non-zero indicates node marked for cleanup
		// Prevents processing during traversal

		const uint32_t KEY = 0x18;                      // Node classification key
		// Read as: [TreeNode* + 0x18] -> uint32_t
		// Used for fast node type identification

		const uint32_t ID = 0x20;                       // Unique node identifier
		// Read as: [TreeNode* + 0x20] -> uint32_t
		// Used for:
		// - Network synchronization
		// - Object lookup operations
		// - Event targeting
	}

	namespace TMHuman {
		const uint32_t POSITION_X = 0x28;               // World X coordinate
		// Read/Write as: [TMHuman* + 0x28] -> int32_t

		const uint32_t POSITION_Y = 0x2C;               // World Y coordinate
		// Read/Write as: [TMHuman* + 0x2C] -> int32_t

		const uint32_t WeaponType = 0xF4;
		//Read, contains a value that represents what type of weapon the entity is wielding. 

		const uint32_t HEALTH = 0x64C;                  // Current health points
		// Read/Write as: [TMHuman* + 0x64C] -> int32_t

		const uint32_t MANA = 0x650;                    // Current mana points
		// Read/Write as: [TMHuman* + 0x650] -> int32_t

		const uint32_t NAME = 0xFC;                     // Character name string
		// Read as: [TMHuman* + 0xFC] -> char[N]
		// Used for:
		// - Player identification
		// - Friend/enemy recognition
		// - Chat message attribution

		const uint32_t MOVING = 0x224;                  // Movement state flag
		// Read as: [TMHuman* + 0x224] -> bool
		// Value mapping:
		// 0: Static (not moving)
		// 1: In motion (walking/running)
		// Used for animation state management
	}

	// TMCamera offsets
	namespace TMCamera {
		constexpr uint32_t SIGHT_LENGTH = 0x34;         // Current camera zoom level
		// Read/Write as: [TMCamera* + 0x34] -> float
		// Controls viewing distance from player
		// Smaller values = zoomed in, larger = zoomed out
		// Must respect MAX_CAM_LEN constraint

		constexpr uint32_t MAX_CAM_LEN = 0xC0;          // Maximum allowed zoom distance
		// Read as: [TMCamera* + 0xC0] -> float
		// Hard limit for camera distance
	}
}

// Global variables for entity scanner
uintptr_t g_gameBaseAddress = NULL;
uintptr_t g_targetAddressBase = 0;

std::vector<EntityInfo> g_entities;
std::mutex g_entitiesMutex;
bool g_scannerRunning = false;
int g_scanInterval = 1; // ms
bool g_showNPCs = true;
bool g_showPlayers = true;
bool g_autoSelectNearest = false;
int g_selectedEntityIndex = -1;

bool g_isWandering = false;
int g_wanderTargetRoom = 0;

// Atomic waypoint sequence tracking
bool g_waitingAtWaypoint = false;
bool g_inWaypointSequence = false;
std::chrono::steady_clock::time_point g_sequenceStartTime;
std::chrono::steady_clock::time_point g_waypointWaitStartTime;

// Movement speed management
std::chrono::steady_clock::time_point g_lastPlayerDetectedTime = std::chrono::steady_clock::now();
bool g_sonicModeEnabled = false;  // SONIC mode toggle state
bool g_usingSonicSpeed = false;  // Ensure this exists
float g_movementSpeed = 1010.0f;  // Default speed

// Combat hack dynamic values
bool g_usingEnhancedCombatValues = false;  // Tracks if enhanced values are active
bool g_manualCombatValueOverride = false;  // Manual override flag          // Default value
int g_enhancedCombatValue = 128;           // Enhanced value when no players detected

// Hash function for pair to use in unordered_map
struct PairHash {
	template <class T1, class T2>
	std::size_t operator() (const std::pair<T1, T2>& pair) const {
		return std::hash<T1>()(pair.first) ^ std::hash<T2>()(pair.second);
	}
};

// Global variables for Floyd-Warshall algorithm
std::vector<std::vector<int>> g_distanceMatrix;    // Shortest distances between rooms
std::vector<std::vector<int>> g_nextHopMatrix;     // Next room in the shortest path

// Room mapping for optimized Floyd-Warshall
std::unordered_map<int, int> g_roomToIndex;  // Maps room ID to matrix index
std::vector<int> g_indexToRoom;              // Maps matrix index to room ID
const int MAX_VALID_ROOMS = 36;

// Fixed-size arrays for critical path operations
int g_roomIDToFixedIndex[100];  // Mapping from room ID to array index (oversized for safety)
int g_fixedIndexToRoomID[MAX_VALID_ROOMS]; // Reverse mapping

// Path caching system
struct PathCacheEntry {
	std::vector<int> path;
	std::chrono::steady_clock::time_point lastUsed;
};

// Cache for frequently used paths
std::unordered_map<std::string, PathCacheEntry> g_pathCache;
const size_t MAX_CACHE_SIZE = 50;  // Maximum number of paths to cache

// Room sector definitions for hierarchical pathfinding
const std::vector<std::vector<int>> ROOM_SECTORS = {
	{1, 2, 7, 8},          // Sector 0 - Northwestern rooms
	{3, 4, 9, 10},         // Sector 1 - North central rooms
	{5, 6, 11, 12},        // Sector 2 - Northeastern rooms
	{13, 14, 19, 20},      // Sector 3 - Western rooms
	{15, 16, 21, 22},      // Sector 4 - Central rooms
	{17, 18, 23, 24},      // Sector 5 - Eastern rooms
	{25, 26, 31, 32},      // Sector 6 - Southwestern rooms 
	{27, 28, 33, 34},      // Sector 7 - South central rooms
	{29, 30, 35, 36}       // Sector 8 - Southeastern rooms
};


// Add the function prototypes to the Menu namespace
namespace Menu {
	// Existing declarations from your globals.h file
	extern WNDCLASSEX wnd_class;
	extern HWND hwnd;
	extern WNDPROC org_wndproc;
	extern bool setup;
	extern bool show_overlay;
	extern LPDIRECT3D9 d3d9;
	extern LPDIRECT3DDEVICE9 device;

	bool setup_wnd_class(const char* class_name) noexcept;
	void destroy_wnd_class() noexcept;
	bool setup_hwnd(const char* name) noexcept;
	void destroy_hwnd() noexcept;
	bool SetupDX() noexcept;
	void DestroyDX() noexcept;
	void Core();
	void SetupMenu(LPDIRECT3DDEVICE9 device) noexcept;
	void Destroy() noexcept;
	void Render() noexcept;

	// Selection persistence, when selecting an entity in the entity list.
	uintptr_t g_lastSelectedEntityAddress = 0; // Store address instead of index

	bool is_window_inactive = false;
	bool is_window_collapsed = false;

	void RenderLanBotTab();

	void RenderMiscTab();
	void RenderTelemetryDisplay() noexcept;

}

namespace ZoomSystem {
	float g_currentZoom = 30.0f;      // Default game zoom level
	float g_maxZoomLimit = 100.0f;    // Default expanded limit
	bool g_zoomEnabled = true;        // Feature toggle
	std::mutex g_zoomMutex;
}

// for tracking some values from patches or memory edits, for example aoe hack or movementspeed
// this is because we want the bot to dynamically apply these altered values only when players are absent and apply default values 
// in the presence of players. This is mostly for the bot itself when its running LAN A.
namespace ValueTracker {
	int g_currentImulValue = 1000;    // Default value

	// Track the three combat hack values
	int g_combatValue1 = 3;  // Default for first address
	int g_combatValue2 = 2;  // Default for second address 
	int g_combatValue3 = 1;  // Default for third address
	bool g_usingDefaultCombatValues = true;  // Track whether we're using default values
}

// Add to global variables
bool g_autoAttackNoCDEnabled = false; // Tracks if auto-attack cooldown is disabled

// Add this with the other global variables
bool g_skillSwitchEnabled = false;  // Track if skill page switching is enabled
bool g_onPage2 = false;             // Track which skill page we're currently on


//=========================================================================================
// Memory Reading Functions - Improved and Separated
//=========================================================================================

// Initialize cache values
void InitializeCache() {
	g_gameBaseAddress = reinterpret_cast<uintptr_t>(GetModuleHandleA(Offsets::Game::MODULE_NAME));
	if (!g_gameBaseAddress) {
		// Log or handle the error appropriately
		std::cout << "Failed to get game module handle!" << std::endl;
	}
}

// Check if an address is valid
bool IsValidAddress(uintptr_t address) {
	return address != 0 && address > 0x10000;
}

// Template for reading memory with validation
template <typename T>
T ReadMemory(uintptr_t address) {
	if (!IsValidAddress(address)) return T{};

	__try {
		return *reinterpret_cast<T*>(address);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return T{};
	}
}

// Read float with validation
float ReadFloat(uintptr_t address) {
	if (!IsValidAddress(address)) return 0.0f;

	__try {
		return *reinterpret_cast<float*>(address);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return 0.0f;
	}
}

// Read integer with validation
int ReadInt(uintptr_t address) {
	if (!IsValidAddress(address)) return 0;

	__try {
		return *reinterpret_cast<int*>(address);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

// Read uintptr_t with validation
uintptr_t ReadPointer(uintptr_t address) {
	if (!IsValidAddress(address)) return 0;

	__try {
		return *reinterpret_cast<uintptr_t*>(address);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

// Read boolean value from memory
bool ReadBool(uintptr_t address) {
	if (!IsValidAddress(address)) return false;

	__try {
		int value = *reinterpret_cast<int*>(address);
		return value != 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// String reading function with safety checks - no exception handling
std::string ReadString(uintptr_t address, size_t maxLength = 16) {
	if (!IsValidAddress(address)) return "";

	// Create a buffer with extra space for null termination
	char buffer[256] = { 0 };
	size_t safeLength = (maxLength < 255) ? maxLength : 255;

	// Use a volatile pointer to help prevent compiler optimizations
	// that might interfere with our safety checks
	volatile const char* src = reinterpret_cast<const char*>(address);

	// Copy memory with bounds checking
	for (size_t i = 0; i < safeLength; i++) {
		// Check if we've hit a null terminator already
		if (src[i] == 0) {
			buffer[i] = 0;
			break;
		}

		// Check for non-printable characters
		if (src[i] < 32 || src[i] > 126) {
			buffer[i] = 0;
			break;
		}

		buffer[i] = src[i];
	}

	// Ensure null termination
	buffer[safeLength] = 0;

	return std::string(buffer);
}

// Write memory with validation
template <typename T>
bool WriteMemory(uintptr_t address, T value) {
	if (!IsValidAddress(address)) return false;

	__try {
		*reinterpret_cast<T*>(address) = value;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

//=========================================================================================
// Memory Patching Structure and Functions
//=========================================================================================

// Patch information structure
struct PatchInfo {
	uintptr_t offset;
	size_t size;
	const char* description;
	const unsigned char newBytes[12];  // Maximum patch size
};

// Write multiple bytes to memory with protection handling
bool WriteMemoryBytes(uintptr_t address, const unsigned char* bytes, size_t size) {
	if (!IsValidAddress(address)) return false;

	DWORD oldProtect;
	if (!VirtualProtect(reinterpret_cast<LPVOID>(address), size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
		return false;
	}

	bool success = false;
	__try {
		for (size_t i = 0; i < size; i++) {
			*reinterpret_cast<unsigned char*>(address + i) = bytes[i];
		}
		success = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		success = false;
	}

	// Restore original memory protection (capture result, even if we ignore it)
	DWORD tempProtect;
	VirtualProtect(reinterpret_cast<LPVOID>(address), size, oldProtect, &tempProtect);

	return success;
}

// Apply a single memory patch
bool ApplyPatch(const PatchInfo& patch) {
	// Ensure game base address is initialized
	if (!g_gameBaseAddress) {
		g_gameBaseAddress = reinterpret_cast<uintptr_t>(GetModuleHandleA(Offsets::Game::MODULE_NAME));
		if (!g_gameBaseAddress) {
			return false;
		}
	}

	// Calculate absolute address
	uintptr_t targetAddress = g_gameBaseAddress + patch.offset;

	// Apply the patch
	return WriteMemoryBytes(targetAddress, patch.newBytes, patch.size);
}

bool ApplyCombatHackValues(int value, bool isDefault = false) {
	if (!g_gameBaseAddress) {
		g_gameBaseAddress = reinterpret_cast<uintptr_t>(GetModuleHandleA(Offsets::Game::MODULE_NAME));
		if (!g_gameBaseAddress) {
			return false;
		}
	}

	bool success = true;
	bool valueChanged = false;

	if (isDefault) {
		if (ValueTracker::g_usingDefaultCombatValues) {
			return true;
		}

		// Target immediate value locations within instructions
		if (ValueTracker::g_combatValue1 != 3) {
			unsigned char value1Bytes[4] = { 3, 0, 0, 0 };
			success &= WriteMemoryBytes(g_gameBaseAddress + 0xB3026, value1Bytes, 4);
			if (success) {
				ValueTracker::g_combatValue1 = 3;
				valueChanged = true;
			}
		}

		if (ValueTracker::g_combatValue2 != 2) {
			unsigned char value2Bytes[4] = { 2, 0, 0, 0 };
			success &= WriteMemoryBytes(g_gameBaseAddress + 0xB3032, value2Bytes, 4);
			if (success) {
				ValueTracker::g_combatValue2 = 2;
				valueChanged = true;
			}
		}

		if (ValueTracker::g_combatValue3 != 1) {
			unsigned char value3Bytes[4] = { 1, 0, 0, 0 };
			success &= WriteMemoryBytes(g_gameBaseAddress + 0xB303E, value3Bytes, 4);
			if (success) {
				ValueTracker::g_combatValue3 = 1;
				valueChanged = true;
			}
		}

		if (valueChanged) {
			ValueTracker::g_usingDefaultCombatValues = true;
		}
	}
	else {
		unsigned char valueBytes[4];
		valueBytes[0] = static_cast<unsigned char>(value & 0xFF);
		valueBytes[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
		valueBytes[2] = static_cast<unsigned char>((value >> 16) & 0xFF);
		valueBytes[3] = static_cast<unsigned char>((value >> 24) & 0xFF);

		if (ValueTracker::g_combatValue1 != value) {
			success &= WriteMemoryBytes(g_gameBaseAddress + 0xB3026, valueBytes, 4);
			if (success) {
				ValueTracker::g_combatValue1 = value;
				valueChanged = true;
			}
		}

		if (ValueTracker::g_combatValue2 != value) {
			success &= WriteMemoryBytes(g_gameBaseAddress + 0xB3032, valueBytes, 4);
			if (success) {
				ValueTracker::g_combatValue2 = value;
				valueChanged = true;
			}
		}

		if (ValueTracker::g_combatValue3 != value) {
			success &= WriteMemoryBytes(g_gameBaseAddress + 0xB303E, valueBytes, 4);
			if (success) {
				ValueTracker::g_combatValue3 = value;
				valueChanged = true;
			}
		}

		if (valueChanged) {
			ValueTracker::g_usingDefaultCombatValues = false;
		}
	}

	return success;
}

// Change the IMUL immediate value dynamically (1000 default, 0 when no players)
//this affects cooldown of autoskilluse (auto attack with magic spells) default 1000 ms
bool SetImulMultiplierValue(int value) {
	if (ValueTracker::g_currentImulValue == value) {
		return true;
	}

	if (!g_gameBaseAddress) {
		g_gameBaseAddress = reinterpret_cast<uintptr_t>(GetModuleHandleA(Offsets::Game::MODULE_NAME));
		if (!g_gameBaseAddress) {
			return false;
		}
	}

	unsigned char valueBytes[4];
	valueBytes[0] = static_cast<unsigned char>(value & 0xFF);
	valueBytes[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
	valueBytes[2] = static_cast<unsigned char>((value >> 16) & 0xFF);
	valueBytes[3] = static_cast<unsigned char>((value >> 24) & 0xFF);

	// Write directly to the immediate value location in the IMUL instruction
	bool success = WriteMemoryBytes(g_gameBaseAddress + 0xB2619, valueBytes, 4);

	if (success) {
		ValueTracker::g_currentImulValue = value;
	}

	return success;
}

// Apply all patches
bool ApplyAllPatches() {
	// Define patches
	static const PatchInfo patches[] = {
		// NOP patches for various game checks mostly to prevent zoom value being reset
		{
		0xF0091, 7, "Remove check 1", // zoom check 1
		{0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90} // 7 NOPs
		},
		{
		0xF00CB, 7, "Remove check 2", // zoom check 2
		{0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90} // 7 NOPs
		},
		{
		0xF0085, 10, "Remove check 3", // zoom check 3
		{0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90} // 10 NOPs
		},
		{
		0x51563, 10, "Remove check 4", // zoom check 4
		{0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90} // 10 NOPs
		},
		{
		0xF00B3, 10, "Remove check 5", // zoom check 5
		{0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90} // 10 NOPs
		},
		{
		0xF00C4, 5, "Remove check 6", // zoom check 6
		{0x90, 0x90, 0x90, 0x90, 0x90} // 5 NOPs
		},

		// Combat hacks - NOP patches
		{
			0xB2606, 5, "Combat hack: NOP add eax,000003E8",
			{0x90, 0x90, 0x90, 0x90, 0x90}
		},
		{
			0xAF2E3, 6, "Combat hack: NOP imul eax,edx,000003E8",
			{0x90, 0x90, 0x90, 0x90, 0x90, 0x90}
		},
	};

	// [Rest of the function remains exactly the same...]
	bool allPatchesSuccessful = true;

	for (const auto& patch : patches) {
		bool success = ApplyPatch(patch);
		if (!success) {
			allPatchesSuccessful = false;
		}
	}

	// Apply initial combat values
	allPatchesSuccessful &= ApplyCombatHackValues(0, true);

	return allPatchesSuccessful;
}

//=========================================================================================
// Game State Functions
//=========================================================================================

// Get current game scene address with error checking
uintptr_t GetCurrentSceneAddress() {
	if (!g_gameBaseAddress) return 0;

	uintptr_t scenePointerAddress = g_gameBaseAddress + Offsets::Game::CURRENT_SCENE;
	return ReadPointer(scenePointerAddress);
}

// Get scene type with validation
uint32_t GetSceneType(uintptr_t sceneAddress) {
	if (!IsValidAddress(sceneAddress)) return ESCENE_NONE;
	return ReadMemory<uint32_t>(sceneAddress + Offsets::TMScene::SCENE_TYPE);
}

// Check if the game is in field mode
bool IsInFieldMode() {
	uintptr_t sceneAddress = GetCurrentSceneAddress();
	if (!sceneAddress) return false;

	uint32_t sceneType = GetSceneType(sceneAddress);
	return sceneType == ESCENE_FIELD;
}

// Get local player address
uintptr_t GetLocalPlayerAddress() {
	uintptr_t sceneAddress = GetCurrentSceneAddress();
	if (!sceneAddress) return 0;

	return ReadPointer(sceneAddress + Offsets::TMScene::MY_HUMAN);
}

// Get human container address
uintptr_t GetHumanContainerAddress() {
	uintptr_t sceneAddress = GetCurrentSceneAddress();
	if (!sceneAddress) return 0;

	return ReadPointer(sceneAddress + Offsets::TMScene::HUMAN_CONTAINER);
}

//=========================================================================================
// Entity Reading Functions
//=========================================================================================

// Calculate distance between two points
float CalculateDistance(float x1, float y1, float x2, float y2) {
	float dx = x2 - x1;
	float dy = y2 - y1;
	return sqrt(dx * dx + dy * dy);
}

// Determine if entity is an NPC based on ID
bool IsEntityNPC(uint32_t entityId) {
	// NPCs have IDs starting at 1001 with no upper limit within 32-bit range
	return (entityId >= 1000);
}

bool IsPlayer(const EntityInfo& entity) {
	// Player IDs are below 1000
	return (entity.id > 0 && entity.id < 999);
}

// Check if two entities are the same by ID
bool IsSameEntity(uint32_t id1, uint32_t id2) {
	return id1 == id2;
}

// Read entity information with improved validation
void ReadEntityInfo(uintptr_t entityAddress, EntityInfo& info) {
	// Reset info to default values
	info = EntityInfo();

	if (!IsValidAddress(entityAddress)) {
		return;
	}

	info.address = entityAddress;
	info.id = ReadMemory<uint32_t>(entityAddress + Offsets::TreeNode::ID);
	info.name = ReadString(entityAddress + Offsets::TMHuman::NAME, 16);
	info.posX = ReadFloat(entityAddress + Offsets::TMHuman::POSITION_X);
	info.posY = ReadFloat(entityAddress + Offsets::TMHuman::POSITION_Y);
	info.currentHP = ReadInt(entityAddress + Offsets::TMHuman::HEALTH);
	info.isMoving = ReadBool(entityAddress + Offsets::TMHuman::MOVING);
	info.isNPC = IsEntityNPC(info.id);
}

// Calculate entity distance from player
void CalculateEntityDistance(EntityInfo& entity, const EntityInfo& player) {
	entity.distance = CalculateDistance(entity.posX, entity.posY, player.posX, player.posY);
}

// Get the first down node from a container
uintptr_t GetFirstDownNode(uintptr_t containerAddress) {
	if (!IsValidAddress(containerAddress)) return 0;
	return ReadPointer(containerAddress + Offsets::TreeNode::DOWN);
}

// Get the next linked node
uintptr_t GetNextLinkedNode(uintptr_t nodeAddress) {
	if (!IsValidAddress(nodeAddress)) return 0;
	return ReadPointer(nodeAddress + Offsets::TreeNode::NEXT_LINK);
}

// Check if entity node is deleted
bool IsEntityDeleted(uintptr_t entityAddress) {
	return ReadBool(entityAddress + Offsets::TreeNode::DELETED_FLAG);
}

// will manually set the entity to cleaned up from memory, but might not be done properly, as residue can be leftover ingame.
bool DeleteEntity(uintptr_t entityAddress) {
	return WriteMemory<int>(entityAddress + Offsets::TreeNode::DELETED_FLAG, 1);
}

// Toggle auto attack
// Modify SetAutoAttack to handle weapon types
bool SetAutoAttack(bool enable, int weaponType = 101) {
	if (!g_gameBaseAddress) return false;

	uintptr_t autoAttackAddr = g_gameBaseAddress + Offsets::Game::AUTO_ATTACK;

	if (!enable) {
		return WriteMemory<int>(autoAttackAddr, 0);
	}

	// Use 1 for physical (101), 2 for magical (anything else)
	int attackValue = (weaponType == 101) ? 1 : 2;
	return WriteMemory<int>(autoAttackAddr, attackValue);
}

bool SetMovementHuntingMode(int mode) {
	if (!g_gameBaseAddress) return false;

	// Get the scene address first
	uintptr_t sceneAddress = GetCurrentSceneAddress();
	if (!sceneAddress) return false;

	// Calculate the hunting mode address relative to the scene object
	uintptr_t huntingModeAddr = sceneAddress + 0x597E10;

	return WriteMemory<int>(huntingModeAddr, mode);
}

// Get hit counter value
int GetHitCounter() {
	if (!g_gameBaseAddress) return 0;

	uintptr_t hitCounterAddr = g_gameBaseAddress + Offsets::Game::HIT_COUNTER;
	return ReadInt(hitCounterAddr);
}

// Set camera zoom limit and optionally the current zoom level
bool SetCameraZoom(float maxLimit = -1.0f, float currentZoom = -1.0f) {
	std::lock_guard<std::mutex> lock(ZoomSystem::g_zoomMutex);

	uintptr_t sceneAddress = GetCurrentSceneAddress();
	if (!sceneAddress) return false;
	uint32_t sceneType = GetSceneType(sceneAddress);
	if (sceneType != ESCENE_FIELD) {
		return false;
	}

	uintptr_t cameraAddress = ReadMemory<uintptr_t>(sceneAddress + Offsets::TreeNode::NEXT_LINK);
	if (!cameraAddress) return false;

	bool success = true;

	if (maxLimit >= 0.0f) {
		success &= WriteMemory<float>(cameraAddress + Offsets::TMCamera::MAX_CAM_LEN, maxLimit);
	}

	if (currentZoom >= 0.0f) {
		success &= WriteMemory<float>(cameraAddress + Offsets::TMCamera::SIGHT_LENGTH, currentZoom);
	}

	return success;
}

// Read current zoom value from camera
float GetCurrentZoom() {
	uintptr_t sceneAddress = GetCurrentSceneAddress();
	if (!sceneAddress) return 20.0f; // Default value

	uint32_t sceneType = GetSceneType(sceneAddress);
	if (sceneType != ESCENE_FIELD) {
		return 20.0f;
	}

	// Get camera address
	uintptr_t cameraAddress = ReadMemory<uintptr_t>(sceneAddress + Offsets::TreeNode::NEXT_LINK);
	if (!cameraAddress) return 20.0f;

	// Read current zoom value
	return ReadMemory<float>(cameraAddress + Offsets::TMCamera::SIGHT_LENGTH);
}

// Initialize zoom system - call this on application startup
void InitializeZoomSystem() {
	using namespace ZoomSystem;

	if (g_zoomEnabled) {
		// Apply both maximum zoom limit and current zoom on initialization
		SetCameraZoom(g_maxZoomLimit, g_currentZoom);

		// Get actual current zoom from game and sync UI
		float actualZoom = GetCurrentZoom();
		if (actualZoom > 0) {
			g_currentZoom = actualZoom;
		}
		else {
			// If can't read, set our default
			SetCameraZoom(-1.0f, g_currentZoom);
		}
	}
}

int GetPlayerMana() {
	uintptr_t playerAddress = GetLocalPlayerAddress();
	if (!playerAddress) return 0;

	return ReadInt(playerAddress + Offsets::TMHuman::MANA);
}

int GetPlayerWeaponType() {
	uintptr_t playerAddress = GetLocalPlayerAddress();
	if (!playerAddress) return 0;

	return ReadInt(playerAddress + Offsets::TMHuman::WeaponType);
}

// Set movement speed by writing float value to memory
bool SetMovementSpeed(float speed) {
	if (!g_gameBaseAddress) return false;

	uintptr_t speedAddr = g_gameBaseAddress + Offsets::Game::MOVEMENT_SPEED;

	DWORD oldProtect;
	if (!VirtualProtect((LPVOID)speedAddr, sizeof(float), PAGE_EXECUTE_READWRITE, &oldProtect))
		return false;

	bool success = false;
	__try {
		*reinterpret_cast<float*>(speedAddr) = speed;
		success = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
	}

	VirtualProtect((LPVOID)speedAddr, sizeof(float), oldProtect, &oldProtect);
	return success;
}

//=========================================================================================
// Entity Collection Functions
//=========================================================================================

// Collect all entity addresses from the container
std::vector<uintptr_t> CollectEntityAddresses(uintptr_t containerAddress, uintptr_t localPlayerAddress) {
	std::vector<uintptr_t> addresses;
	std::vector<uint32_t> seenEntityIds;

	if (!IsValidAddress(containerAddress) || !IsValidAddress(localPlayerAddress)) {
		return addresses;
	}

	// Add local player ID to seen list
	uint32_t localPlayerId = ReadMemory<uint32_t>(localPlayerAddress + Offsets::TreeNode::ID);
	seenEntityIds.push_back(localPlayerId);

	// First downlink
	uintptr_t currentNode = GetFirstDownNode(containerAddress);
	int count = 0;
	const int MAX_ENTITIES = 500;

	// Collect all entity addresses
	while (IsValidAddress(currentNode) && count < MAX_ENTITIES) {
		// Skip if it's the player or deleted
		if (currentNode == localPlayerAddress || IsEntityDeleted(currentNode)) {
			currentNode = GetNextLinkedNode(currentNode);
			count++;
			continue;
		}

		// Get entity ID
		uint32_t entityId = ReadMemory<uint32_t>(currentNode + Offsets::TreeNode::ID);

		// Check if we've already seen this ID
		bool isDuplicate = false;
		for (const auto& id : seenEntityIds) {
			if (id == entityId) {
				isDuplicate = true;
				break;
			}
		}

		// Skip if duplicate
		if (isDuplicate) {
			currentNode = GetNextLinkedNode(currentNode);
			count++;
			continue;
		}

		// Add to our tracking list
		seenEntityIds.push_back(entityId);

		// Add address to our collection list
		addresses.push_back(currentNode);

		// Move to next node
		currentNode = GetNextLinkedNode(currentNode);
		count++;
	}

	return addresses;
}

// Filter entities based on current settings
bool ShouldIncludeEntity(const EntityInfo& entity) {
	return (entity.isNPC && g_showNPCs) || (!entity.isNPC && g_showPlayers);
}

// Process and filter entity addresses into EntityInfo objects
std::vector<EntityInfo> ProcessEntityAddresses(
	const std::vector<uintptr_t>& addresses,
	const EntityInfo& localPlayer) {

	std::vector<EntityInfo> entities;

	// Define entity names to filter - make this static to improve performance
	static const std::set<std::string> filteredEntityNames = {
		"Tiger^", "Condor^", "Wild Boar^", "Rage Wolf^",
		"Savage Bear^", "Trample Tiger^", "Stunning Gorilla^",
		"Black Dragon^", "Succubus^"
	};

	for (uintptr_t addr : addresses) {
		EntityInfo info;
		ReadEntityInfo(addr, info);

		// Calculate distance from player
		CalculateEntityDistance(info, localPlayer);

		// Skip entities with filtered names
		if (filteredEntityNames.find(info.name) != filteredEntityNames.end()) {
			continue;  // Skip this entity
		}

		// Apply regular type filtering
		if (ShouldIncludeEntity(info)) {
			entities.push_back(info);
		}
	}

	return entities;
}

// Sort entities by distance
void SortEntitiesByDistance(std::vector<EntityInfo>& entities) {
	std::sort(entities.begin(), entities.end(),
		[](const EntityInfo& a, const EntityInfo& b) {
			return a.distance < b.distance;
		});
}

// The main function to get all entities from the current scene
std::vector<EntityInfo> GetEntities() {
	std::vector<EntityInfo> entities;

	// Only proceed if we're in field mode
	if (!IsInFieldMode()) {
		return entities;
	}

	// Get local player address
	uintptr_t localPlayerAddress = GetLocalPlayerAddress();
	if (!IsValidAddress(localPlayerAddress)) {
		return entities;
	}

	// Add local player to entities
	EntityInfo localPlayer;
	ReadEntityInfo(localPlayerAddress, localPlayer);
	localPlayer.isLocalPlayer = true;
	entities.push_back(localPlayer);

	// Get human container
	uintptr_t containerAddress = GetHumanContainerAddress();
	if (!IsValidAddress(containerAddress)) {
		return entities;
	}

	// Collect all entity addresses
	auto entityAddresses = CollectEntityAddresses(containerAddress, localPlayerAddress);

	// Process entity addresses into EntityInfo objects
	auto nonPlayerEntities = ProcessEntityAddresses(entityAddresses, localPlayer);

	// Sort non-player entities by distance
	SortEntitiesByDistance(nonPlayerEntities);

	// Add sorted entities to the final list
	entities.insert(entities.end(), nonPlayerEntities.begin(), nonPlayerEntities.end());

	return entities;
}

//=========================================================================================
// Entity Scanner Thread Functions
//=========================================================================================

// Find entity by address
const EntityInfo* FindEntityByAddress(const std::vector<EntityInfo>& entities, uintptr_t address) {
	for (const auto& entity : entities) {
		if (entity.address == address) {
			return &entity;
		}
	}
	return nullptr;
}

// Select the nearest entity based on settings
void SelectNearestEntity(const std::vector<EntityInfo>& entities) {
	if (!g_autoSelectNearest || Menu::g_lastSelectedEntityAddress != 0 || entities.size() <= 1) {  // Add Menu::
		return;
	}

	// Select the first non-player entity (which is the nearest due to sorting)
	Menu::g_lastSelectedEntityAddress = entities[1].address;  // Add Menu::
}
// Main scanner thread function with integrated zoom maintenance
void ScannerThread() {
	using namespace std::chrono;

	auto lastScanTime = steady_clock::now();

	while (g_scannerRunning) {
		// Save current selected entity address
		uintptr_t selectedAddress = Menu::g_lastSelectedEntityAddress;

		// Throttle scanning when nothing is happening
		auto currentTime = steady_clock::now();
		auto elapsedTime = duration_cast<milliseconds>(currentTime - lastScanTime).count();

		if (elapsedTime < g_scanInterval) {
			std::this_thread::sleep_for(milliseconds(g_scanInterval - elapsedTime));
			continue;
		}

		lastScanTime = steady_clock::now();

		// Get fresh entities
		auto entities = GetEntities();

		if (!entities.empty()) {
			// Check if selected entity still exists
			bool selectedEntityStillExists = (selectedAddress != 0) &&
				(FindEntityByAddress(entities, selectedAddress) != nullptr);

			{
				std::lock_guard<std::mutex> lock(g_entitiesMutex);
				g_entities = entities;

				// Clear selection if entity is gone
				if (selectedAddress != 0 && !selectedEntityStillExists) {
					Menu::g_lastSelectedEntityAddress = 0;
				}

				// Auto-select nearest if enabled
				SelectNearestEntity(entities);
			}
		}
	}
}

// Start entity scanner
void StartScanner() {
	if (!g_scannerRunning) {
		g_scannerRunning = true;
		std::thread scanner(ScannerThread);
		scanner.detach();
	}
}

// Stop entity scanner
void StopScanner() {
	g_scannerRunning = false;
}

//=========================================================================================
// BOT Implementation - Lan [A]rcane
//=========================================================================================

// Bot control states
enum class BotState {
	IDLE,               // Initial state, grace period
	SCANNING_ROOMS,     // Scanning rooms for NPCs
	NAVIGATING,         // Moving between rooms
	COMBAT,             // Engaged in combat with NPCs
	CLEANUP             // Cleaning up after combat
};

// Coordinate structure for navigation
struct Coordinate {
	int x;
	int y;

	Coordinate() : x(0), y(0) {}
	Coordinate(int _x, int _y) : x(_x), y(_y) {}

	// Calculate distance to another coordinate
	float distanceTo(const Coordinate& other) const {
		float dx = static_cast<float>(other.x - x);
		float dy = static_cast<float>(other.y - y);
		return std::sqrt(dx * dx + dy * dy);
	}
};

// Room connection structure
struct RoomConnection {
	int targetRoom;
	Coordinate entryPoint;
	Coordinate exitPoint;
	// connectionId is now an implementation detail, not exposed in the API

	RoomConnection(int target, const Coordinate& entry, const Coordinate& exit)
		: targetRoom(target), entryPoint(entry), exitPoint(exit) {
	}
};

struct Room {
	int id;
	std::vector<RoomConnection> connections;

	Room(int roomId) : id(roomId) {}

	void connectTo(int targetRoom, const Coordinate& entry, const Coordinate& exit) {
		connections.emplace_back(targetRoom, entry, exit);
	}
};

struct RoomBoundary {
	Coordinate topLeft;
	Coordinate topRight;
	Coordinate bottomLeft;
	Coordinate bottomRight;
};


// NPC room info
struct RoomNPCInfo {
	int roomId = 0;
	std::vector<EntityInfo> npcs;
	int pathLength = 0;  // Number of rooms to traverse
	std::vector<int> path;  // Path of room IDs
};

struct RoomOccupancy {
	bool hasPlayer = false;
	bool hasNPCs = false;
	int playersCount = 0;
	std::vector<EntityInfo> players;  // Track which players are in the room
};

std::map<int, RoomOccupancy> g_roomOccupancy;

// Combat state tracking
struct CombatState {
	bool inCombat = false;
	int lastHitCounter = 0;
	int hitStagnationCount = 0;
	std::chrono::steady_clock::time_point lastHitTime;
	std::vector<uintptr_t> engagedNPCs;
	uintptr_t currentTargetAddress = 0;
	int weaponType = 0;
	int lastMana = 0;
	std::chrono::steady_clock::time_point lastManaChangeTime;
	bool isPhysicalWeapon = true;

	// Add these fields for NPC stagnation detection
	int lastTargetHP = 0;
	int stagnantHPCounter = 0;
	std::chrono::steady_clock::time_point lastHPChangeTime;

	// Movement stagnation detection
	bool trackingMovement = false;
	float lastKnownX = 0.0f;
	float lastKnownY = 0.0f;
	std::chrono::steady_clock::time_point lastMovementTime;
	enum MovementState { NORMAL, STAGNATED, ROOM_CENTER_FALLBACK, GIVE_UP } movementState = NORMAL;
	int movementAttempts = 0;
};

// Global variables for LAN bot navigation
std::unordered_map<int, Room*> g_rooms;
std::unordered_map<int, RoomBoundary> g_roomBoundaries;
int g_currentRoom = 0;
int g_targetRoom = 0;
std::deque<Coordinate> g_pathWaypoints;
BotState g_botState = BotState::IDLE;
bool g_lanBotRunning = false;
std::string g_botStatusMessage = "Bot is disabled";
std::vector<std::pair<std::string, ImVec4>> g_botDebugMessages;
std::mutex g_botDebugMutex;

bool g_verboseLogging = false; // Default to less verbose logging
std::chrono::steady_clock::time_point g_lastPositionLogTime;
std::chrono::steady_clock::time_point g_lastMovementLogTime;
const int LOG_INFO = 0;
const int LOG_IMPORTANT = 1;
const int LOG_ERROR = 2;

std::chrono::steady_clock::time_point g_enteredLanTime;
std::chrono::steady_clock::time_point g_lastRoomScanTime;
CombatState g_combatState;
std::map<int, RoomNPCInfo> g_roomNPCMap;

void setupRoomConnections() {
	// Room 1 connections
	g_rooms[1]->connectTo(2, Coordinate(3864, 3617), Coordinate(3864, 3622));
	g_rooms[1]->connectTo(7, Coordinate(3871, 3609), Coordinate(3876, 3609));

	// Room 2 connections  
	g_rooms[2]->connectTo(1, Coordinate(3864, 3622), Coordinate(3864, 3617));
	g_rooms[2]->connectTo(3, Coordinate(3868, 3632), Coordinate(3868, 3637));
	g_rooms[2]->connectTo(8, Coordinate(3871, 3624), Coordinate(3876, 3624));

	// Room 3 connections
	g_rooms[3]->connectTo(2, Coordinate(3868, 3637), Coordinate(3868, 3632));
	g_rooms[3]->connectTo(4, Coordinate(3864, 3647), Coordinate(3864, 3652));
	g_rooms[3]->connectTo(9, Coordinate(3871, 3641), Coordinate(3876, 3641));

	// Room 4 connections
	g_rooms[4]->connectTo(3, Coordinate(3864, 3652), Coordinate(3864, 3647));
	g_rooms[4]->connectTo(5, Coordinate(3868, 3662), Coordinate(3868, 3667));
	g_rooms[4]->connectTo(10, Coordinate(3871, 3655), Coordinate(3876, 3655));

	// Room 5 connections
	g_rooms[5]->connectTo(4, Coordinate(3868, 3667), Coordinate(3868, 3662));
	g_rooms[5]->connectTo(11, Coordinate(3871, 3672), Coordinate(3876, 3672));

	// Room 6 connections
	//g_rooms[6]->connectTo(5, Coordinate(3865, 3680), Coordinate(3865, 3674));
	//g_rooms[6]->connectTo(12, Coordinate(3870, 3681), Coordinate(3876, 3681));

	// Room 7 connections
	g_rooms[7]->connectTo(1, Coordinate(3876, 3609), Coordinate(3870, 3609));
	g_rooms[7]->connectTo(8, Coordinate(3881, 3614), Coordinate(3884, 3621));
	g_rooms[7]->connectTo(13, Coordinate(3885, 3610), Coordinate(3891, 3610));

	// Room 8 connections
	g_rooms[8]->connectTo(2, Coordinate(3876, 3624), Coordinate(3870, 3624));
	g_rooms[8]->connectTo(7, Coordinate(3881, 3620), Coordinate(3881, 3614));
	g_rooms[8]->connectTo(9, Coordinate(3877, 3629), Coordinate(3877, 3636));
	g_rooms[8]->connectTo(14, Coordinate(3885, 3625), Coordinate(3892, 3625));

	// Room 9 connections
	g_rooms[9]->connectTo(3, Coordinate(3876, 3640), Coordinate(3870, 3640));
	g_rooms[9]->connectTo(8, Coordinate(3877, 3636), Coordinate(3877, 3629));
	g_rooms[9]->connectTo(10, Coordinate(3884, 3644), Coordinate(3884, 3650));
	g_rooms[9]->connectTo(15, Coordinate(3885, 3636), Coordinate(3891, 3636));

	// Room 10 connections
	g_rooms[10]->connectTo(4, Coordinate(3876, 3651), Coordinate(3869, 3651));
	g_rooms[10]->connectTo(9, Coordinate(3884, 3650), Coordinate(3884, 3644));
	g_rooms[10]->connectTo(11, Coordinate(3880, 3659), Coordinate(3880, 3666));
	g_rooms[10]->connectTo(16, Coordinate(3885, 3655), Coordinate(3891, 3655));

	// Room 11 connections
	g_rooms[11]->connectTo(5, Coordinate(3876, 3670), Coordinate(3870, 3670));
	g_rooms[11]->connectTo(10, Coordinate(3880, 3666), Coordinate(3880, 3659));
	g_rooms[11]->connectTo(12, Coordinate(3884, 3674), Coordinate(3884, 3680));
	g_rooms[11]->connectTo(17, Coordinate(3886, 3666), Coordinate(3891, 3666));

	// Room 12 connections
	//g_rooms[12]->connectTo(6, Coordinate(3876, 3681), Coordinate(3869, 3681));
	g_rooms[12]->connectTo(11, Coordinate(3881, 3680), Coordinate(3881, 3674));
	g_rooms[12]->connectTo(18, Coordinate(3885, 3685), Coordinate(3891, 3685));

	// Room 13 connections
	g_rooms[13]->connectTo(7, Coordinate(3890, 3613), Coordinate(3884, 3613));
	g_rooms[13]->connectTo(14, Coordinate(3892, 3614), Coordinate(3892, 3620));
	g_rooms[13]->connectTo(19, Coordinate(3901, 3609), Coordinate(3906, 3609));

	// Room 14 connections
	g_rooms[14]->connectTo(8, Coordinate(3891, 3625), Coordinate(3885, 3625));
	g_rooms[14]->connectTo(13, Coordinate(3892, 3620), Coordinate(3892, 3614));
	g_rooms[14]->connectTo(15, Coordinate(3899, 3629), Coordinate(3899, 3634));
	g_rooms[14]->connectTo(20, Coordinate(3900, 3621), Coordinate(3906, 3621));

	// Room 15 connections
	g_rooms[15]->connectTo(9, Coordinate(3890, 3636), Coordinate(3885, 3636));
	g_rooms[15]->connectTo(14, Coordinate(3896, 3635), Coordinate(3896, 3629));
	g_rooms[15]->connectTo(16, Coordinate(3895, 3645), Coordinate(3895, 3650));
	g_rooms[15]->connectTo(21, Coordinate(3900, 3640), Coordinate(3906, 3640));

	// Room 16 connections
	g_rooms[16]->connectTo(10, Coordinate(3891, 3655), Coordinate(3886, 3655));
	g_rooms[16]->connectTo(15, Coordinate(3892, 3650), Coordinate(3892, 3644));
	g_rooms[16]->connectTo(17, Coordinate(3899, 3659), Coordinate(3899, 3665));
	g_rooms[16]->connectTo(22, Coordinate(3900, 3651), Coordinate(3906, 3651));

	// Room 17 connections
	g_rooms[17]->connectTo(11, Coordinate(3891, 3666), Coordinate(3885, 3666));
	g_rooms[17]->connectTo(16, Coordinate(3896, 3665), Coordinate(3896, 3659));
	g_rooms[17]->connectTo(18, Coordinate(3895, 3674), Coordinate(3895, 3680));
	g_rooms[17]->connectTo(23, Coordinate(3900, 3670), Coordinate(3906, 3670));

	// Room 18 connections
	g_rooms[18]->connectTo(12, Coordinate(3891, 3685), Coordinate(3885, 3685));
	g_rooms[18]->connectTo(17, Coordinate(3892, 3680), Coordinate(3892, 3674));
	g_rooms[18]->connectTo(24, Coordinate(3901, 3681), Coordinate(3906, 3681));

	// Room 19 connections
	g_rooms[19]->connectTo(13, Coordinate(3906, 3608), Coordinate(3900, 3608));
	g_rooms[19]->connectTo(20, Coordinate(3911, 3614), Coordinate(3912, 3620));
	g_rooms[19]->connectTo(25, Coordinate(3915, 3612), Coordinate(3922, 3612));

	// Room 20 connections
	g_rooms[20]->connectTo(14, Coordinate(3906, 3622), Coordinate(3900, 3623));
	g_rooms[20]->connectTo(19, Coordinate(3912, 3620), Coordinate(3912, 3614));
	g_rooms[20]->connectTo(21, Coordinate(3908, 3629), Coordinate(3908, 3635));
	g_rooms[20]->connectTo(26, Coordinate(3915, 3627), Coordinate(3921, 3627));

	// Room 21 connections
	g_rooms[21]->connectTo(15, Coordinate(3906, 3641), Coordinate(3900, 3641));
	g_rooms[21]->connectTo(20, Coordinate(3908, 3635), Coordinate(3908, 3629));
	g_rooms[21]->connectTo(22, Coordinate(3913, 3643), Coordinate(3913, 3650));
	g_rooms[21]->connectTo(27, Coordinate(3915, 3638), Coordinate(3921, 3638));

	// Room 22 connections
	g_rooms[22]->connectTo(16, Coordinate(3906, 3653), Coordinate(3900, 3653));
	g_rooms[22]->connectTo(21, Coordinate(3913, 3650), Coordinate(3913, 3643));
	g_rooms[22]->connectTo(23, Coordinate(3909, 3659), Coordinate(3909, 3665));
	g_rooms[22]->connectTo(28, Coordinate(3915, 3656), Coordinate(3921, 3656));

	// Room 23 connections
	g_rooms[23]->connectTo(17, Coordinate(3906, 3671), Coordinate(3900, 3671));
	g_rooms[23]->connectTo(22, Coordinate(3909, 3665), Coordinate(3909, 3659));
	g_rooms[23]->connectTo(24, Coordinate(3912, 3674), Coordinate(3912, 3680));
	g_rooms[23]->connectTo(29, Coordinate(3916, 3666), Coordinate(3922, 3666));

	// Room 24 connections
	g_rooms[24]->connectTo(18, Coordinate(3906, 3683), Coordinate(3900, 3683));
	g_rooms[24]->connectTo(23, Coordinate(3912, 3680), Coordinate(3912, 3674));
	g_rooms[24]->connectTo(30, Coordinate(3915, 3686), Coordinate(3922, 3686));

	// Room 25 connections
	g_rooms[25]->connectTo(19, Coordinate(3923, 3612), Coordinate(3914, 3612));
	g_rooms[25]->connectTo(26, Coordinate(3922, 3614), Coordinate(3922, 3620));
	//g_rooms[25]->connectTo(31, Coordinate(3930, 3608), Coordinate(3936, 3608));

	// Room 26 connections
	g_rooms[26]->connectTo(20, Coordinate(3921, 3625), Coordinate(3915, 3625));
	g_rooms[26]->connectTo(25, Coordinate(3923, 3620), Coordinate(3923, 3612));
	g_rooms[26]->connectTo(27, Coordinate(3927, 3629), Coordinate(3927, 3635));
	g_rooms[26]->connectTo(32, Coordinate(3930, 3623), Coordinate(3936, 3623));

	// Room 27 connections
	g_rooms[27]->connectTo(21, Coordinate(3921, 3638), Coordinate(3915, 3638));
	g_rooms[27]->connectTo(26, Coordinate(3927, 3635), Coordinate(3927, 3629));
	g_rooms[27]->connectTo(28, Coordinate(3924, 3644), Coordinate(3924, 3650));
	g_rooms[27]->connectTo(33, Coordinate(3930, 3641), Coordinate(3936, 3641));

	// Room 28 connections
	g_rooms[28]->connectTo(22, Coordinate(3921, 3656), Coordinate(3915, 3656));
	g_rooms[28]->connectTo(27, Coordinate(3924, 3650), Coordinate(3924, 3644));
	g_rooms[28]->connectTo(29, Coordinate(3928, 3659), Coordinate(3928, 3665));
	g_rooms[28]->connectTo(34, Coordinate(3930, 3653), Coordinate(3936, 3653));

	// Room 29 connections
	g_rooms[29]->connectTo(23, Coordinate(3921, 3668), Coordinate(3915, 3668));
	g_rooms[29]->connectTo(28, Coordinate(3928, 3665), Coordinate(3928, 3659));
	g_rooms[29]->connectTo(30, Coordinate(3924, 3674), Coordinate(3924, 3680));
	g_rooms[29]->connectTo(35, Coordinate(3929, 3671), Coordinate(3936, 3670));

	// Room 30 connections
	g_rooms[30]->connectTo(24, Coordinate(3921, 3685), Coordinate(3915, 3686));
	g_rooms[30]->connectTo(29, Coordinate(3924, 3680), Coordinate(3924, 3674));
	g_rooms[30]->connectTo(36, Coordinate(3930, 3681), Coordinate(3936, 3681));

	// Room 31 connections
	//g_rooms[31]->connectTo(25, Coordinate(3936, 3609), Coordinate(3930, 3609));
	//g_rooms[31]->connectTo(32, Coordinate(3941, 3614), Coordinate(3941, 3620));

	// Room 32 connections
	g_rooms[32]->connectTo(26, Coordinate(3936, 3623), Coordinate(3930, 3623));
	//g_rooms[32]->connectTo(31, Coordinate(3941, 3620), Coordinate(3941, 3613));
	g_rooms[32]->connectTo(33, Coordinate(3939, 3629), Coordinate(3939, 3635));

	// Room 33 connections
	g_rooms[33]->connectTo(27, Coordinate(3936, 3641), Coordinate(3930, 3641));
	g_rooms[33]->connectTo(32, Coordinate(3939, 3635), Coordinate(3939, 3629));
	g_rooms[33]->connectTo(34, Coordinate(3942, 3644), Coordinate(3942, 3650));

	// Room 34 connections
	g_rooms[34]->connectTo(28, Coordinate(3936, 3653), Coordinate(3930, 3653));
	g_rooms[34]->connectTo(33, Coordinate(3942, 3650), Coordinate(3942, 3644));
	g_rooms[34]->connectTo(35, Coordinate(3938, 3659), Coordinate(3938, 3665));

	// Room 35 connections
	g_rooms[35]->connectTo(29, Coordinate(3936, 3673), Coordinate(3928, 3672));
	g_rooms[35]->connectTo(34, Coordinate(3938, 3665), Coordinate(3938, 3659));
	g_rooms[35]->connectTo(36, Coordinate(3942, 3674), Coordinate(3942, 3680));

	// Room 36 connections
	g_rooms[36]->connectTo(30, Coordinate(3936, 3681), Coordinate(3930, 3681));
	g_rooms[36]->connectTo(35, Coordinate(3941, 3680), Coordinate(3941, 3674));

}

// Setup room boundaries
void setupRoomBoundaries() {
	// Insert each room boundary with expanded coordinates
	g_roomBoundaries.insert({ 1, {Coordinate(3859, 3617), Coordinate(3873, 3617), Coordinate(3859, 3603), Coordinate(3873, 3603)} });
	g_roomBoundaries.insert({ 2, {Coordinate(3859, 3632), Coordinate(3873, 3632), Coordinate(3859, 3617), Coordinate(3873, 3617)} });
	g_roomBoundaries.insert({ 3, {Coordinate(3859, 3647), Coordinate(3873, 3647), Coordinate(3859, 3632), Coordinate(3873, 3632)} });
	g_roomBoundaries.insert({ 4, {Coordinate(3859, 3662), Coordinate(3873, 3662), Coordinate(3859, 3647), Coordinate(3873, 3648)} });
	g_roomBoundaries.insert({ 5, {Coordinate(3859, 3677), Coordinate(3873, 3675), Coordinate(3859, 3662), Coordinate(3873, 3662)} });
	//	g_roomBoundaries.insert({ 6, {Coordinate(3859, 3692), Coordinate(3873, 3691), Coordinate(3859, 3677), Coordinate(3873, 3677)} });
	g_roomBoundaries.insert({ 7, {Coordinate(3873, 3616), Coordinate(3888, 3616), Coordinate(3873, 3603), Coordinate(3888, 3603)} });
	g_roomBoundaries.insert({ 8, {Coordinate(3873, 3631), Coordinate(3887, 3632), Coordinate(3873, 3618), Coordinate(3887, 3618)} });
	g_roomBoundaries.insert({ 9, {Coordinate(3873, 3647), Coordinate(3888, 3647), Coordinate(3873, 3632), Coordinate(3888, 3632)} });
	g_roomBoundaries.insert({ 10, {Coordinate(3873, 3662), Coordinate(3888, 3662), Coordinate(3873, 3647), Coordinate(3888, 3647)} });
	g_roomBoundaries.insert({ 11, {Coordinate(3873, 3677), Coordinate(3888, 3677), Coordinate(3873, 3662), Coordinate(3888, 3662)} });
	g_roomBoundaries.insert({ 12, {Coordinate(3873, 3692), Coordinate(3888, 3692), Coordinate(3873, 3677), Coordinate(3888, 3677)} });
	g_roomBoundaries.insert({ 13, {Coordinate(3888, 3617), Coordinate(3903, 3617), Coordinate(3888, 3602), Coordinate(3903, 3602)} });
	g_roomBoundaries.insert({ 14, {Coordinate(3888, 3632), Coordinate(3903, 3632), Coordinate(3888, 3617), Coordinate(3903, 3617)} });
	g_roomBoundaries.insert({ 15, {Coordinate(3888, 3647), Coordinate(3903, 3647), Coordinate(3888, 3632), Coordinate(3903, 3632)} });
	g_roomBoundaries.insert({ 16, {Coordinate(3888, 3662), Coordinate(3903, 3662), Coordinate(3888, 3647), Coordinate(3903, 3647)} });
	g_roomBoundaries.insert({ 17, {Coordinate(3888, 3677), Coordinate(3903, 3677), Coordinate(3888, 3662), Coordinate(3903, 3662)} });
	g_roomBoundaries.insert({ 18, {Coordinate(3888, 3692), Coordinate(3903, 3692), Coordinate(3888, 3677), Coordinate(3903, 3677)} });
	g_roomBoundaries.insert({ 19, {Coordinate(3903, 3617), Coordinate(3918, 3617), Coordinate(3903, 3602), Coordinate(3918, 3602)} });
	g_roomBoundaries.insert({ 20, {Coordinate(3903, 3632), Coordinate(3918, 3632), Coordinate(3903, 3617), Coordinate(3918, 3617)} });
	g_roomBoundaries.insert({ 21, {Coordinate(3903, 3647), Coordinate(3918, 3647), Coordinate(3903, 3632), Coordinate(3918, 3632)} });
	g_roomBoundaries.insert({ 22, {Coordinate(3903, 3662), Coordinate(3918, 3662), Coordinate(3903, 3647), Coordinate(3918, 3647)} });
	g_roomBoundaries.insert({ 23, {Coordinate(3903, 3677), Coordinate(3918, 3677), Coordinate(3903, 3662), Coordinate(3918, 3662)} });
	g_roomBoundaries.insert({ 24, {Coordinate(3903, 3692), Coordinate(3918, 3692), Coordinate(3903, 3677), Coordinate(3918, 3677)} });
	g_roomBoundaries.insert({ 25, {Coordinate(3918, 3617), Coordinate(3933, 3617), Coordinate(3918, 3602), Coordinate(3933, 3602)} });
	g_roomBoundaries.insert({ 26, {Coordinate(3918, 3632), Coordinate(3933, 3632), Coordinate(3918, 3617), Coordinate(3933, 3617)} });
	g_roomBoundaries.insert({ 27, {Coordinate(3918, 3647), Coordinate(3933, 3647), Coordinate(3918, 3632), Coordinate(3933, 3632)} });
	g_roomBoundaries.insert({ 28, {Coordinate(3918, 3662), Coordinate(3933, 3662), Coordinate(3918, 3647), Coordinate(3933, 3647)} });
	g_roomBoundaries.insert({ 29, {Coordinate(3918, 3677), Coordinate(3933, 3677), Coordinate(3918, 3662), Coordinate(3933, 3662)} });
	g_roomBoundaries.insert({ 30, {Coordinate(3918, 3692), Coordinate(3933, 3692), Coordinate(3918, 3677), Coordinate(3933, 3677)} });
	//	g_roomBoundaries.insert({ 31, {Coordinate(3933, 3617), Coordinate(3948, 3617), Coordinate(3933, 3602), Coordinate(3948, 3602)} });
	g_roomBoundaries.insert({ 32, {Coordinate(3933, 3632), Coordinate(3948, 3632), Coordinate(3933, 3617), Coordinate(3948, 3617)} });
	g_roomBoundaries.insert({ 33, {Coordinate(3933, 3647), Coordinate(3948, 3647), Coordinate(3933, 3632), Coordinate(3948, 3632)} });
	g_roomBoundaries.insert({ 34, {Coordinate(3933, 3662), Coordinate(3948, 3662), Coordinate(3933, 3647), Coordinate(3948, 3647)} });
	g_roomBoundaries.insert({ 35, {Coordinate(3933, 3677), Coordinate(3948, 3677), Coordinate(3933, 3662), Coordinate(3948, 3662)} });
	g_roomBoundaries.insert({ 36, {Coordinate(3933, 3692), Coordinate(3948, 3692), Coordinate(3933, 3677), Coordinate(3948, 3677)} });
}

// Check if a point is inside a polygon (room boundary)
bool isPointInPolygon(const Coordinate& point, const std::vector<Coordinate>& polygon) {
	bool inside = false;
	size_t i, j;  // Changed from int to size_t
	for (i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
		if (((polygon[i].y > point.y) != (polygon[j].y > point.y)) &&
			(point.x < (polygon[j].x - polygon[i].x) * (point.y - polygon[i].y) /
				(polygon[j].y - polygon[i].y) + polygon[i].x))
		{
			inside = !inside;
		}
	}
	return inside;
}

// Add debug message to the log with more compact formatting for ImGui display
void AddBotDebugMessage(const std::string& message, int severity = LOG_INFO) {
	// Only log INFO messages if verbose logging is enabled
	if (severity == LOG_INFO && !g_verboseLogging) {
		return;
	}
	std::lock_guard<std::mutex> lock(g_botDebugMutex);
	// Add timestamp with shorter format (no seconds)
	auto now = std::chrono::system_clock::now();
	auto time = std::chrono::system_clock::to_time_t(now);
	char timeStr[9];
	std::strftime(timeStr, sizeof(timeStr), "%H:%M", std::localtime(&time));
	// Use more compact severity indicators
	std::string prefix;
	ImVec4 color(1.0f, 1.0f, 1.0f, 1.0f); // Default white
	switch (severity) {
	case LOG_INFO:
		prefix = "i|";
		break;
	case LOG_IMPORTANT:
		prefix = "!|";
		color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // Yellow
		break;
	case LOG_ERROR:
		prefix = "E|";
		color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // Red
		break;
	}
	// Create the message with compact timestamp and prefix
	std::string fullMessage = std::string(timeStr) + prefix + message;
	// Use shorter truncation lengths for the narrower window
	const size_t maxLineLength = 28; // Reduced from 35

	if (fullMessage.length() > maxLineLength) {
		// For INFO messages, just truncate with ellipsis
		if (severity == LOG_INFO && fullMessage.length() > maxLineLength + 3) {
			fullMessage = fullMessage.substr(0, maxLineLength) + "...";
		}
		// For IMPORTANT/ERROR, create two lines if needed
		else if (severity != LOG_INFO && fullMessage.length() > maxLineLength + maxLineLength / 2) { // Changed from maxLineLength * 1.5
			// Find a good breaking point
			size_t breakPoint = maxLineLength;
			while (breakPoint > maxLineLength / 2 &&
				fullMessage[breakPoint] != ' ' &&
				fullMessage[breakPoint] != ',' &&
				fullMessage[breakPoint] != ';') {
				breakPoint--;
			}
			if (breakPoint <= maxLineLength / 2) {
				// If no good breaking point found, just truncate
				size_t maxTruncateLength = maxLineLength + maxLineLength / 2; // Changed from maxLineLength * 1.5
				if (maxTruncateLength >= 3) {
					fullMessage = fullMessage.substr(0, maxTruncateLength - 3) + "...";
				}
				else {
					fullMessage = fullMessage.substr(0, maxTruncateLength) + "...";
				}
			}
			else {
				// Insert newline at the breaking point
				fullMessage = fullMessage.substr(0, breakPoint) + "\n  " +
					fullMessage.substr(breakPoint + 1);
				// Truncate the second line if it's still too long
				if (fullMessage.length() - breakPoint > maxLineLength + 3) {
					fullMessage = fullMessage.substr(0, breakPoint + maxLineLength + 3) + "...";
				}
			}
		}
	}
	// Store color with message (using a pair)
	g_botDebugMessages.insert(g_botDebugMessages.begin(), std::make_pair(fullMessage, color));
	// Limit number of messages
	if (g_botDebugMessages.size() > 100) {
		g_botDebugMessages.pop_back();
	}
}

//=========================================================================================
// Function calls / hooks
//=========================================================================================

// Get current skill page from memory (0 = Page 1, 1 = Page 2)
int GetCurrentSkillPage() {
	uintptr_t sceneAddress = GetCurrentSceneAddress();
	if (!sceneAddress) return -1; // Error state

	__try {
		uintptr_t skillPageAddress = sceneAddress + 0x599E30;
		int pageValue = ReadInt(skillPageAddress);

		// Validate the value is within expected range
		if (pageValue == 0 || pageValue == 1) {
			return pageValue;
		}

		return -1; // Invalid value
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return -1; // Memory read failed
	}
}

//=========================================================================================
// AutoSkillUse Combat Detection Hook
//=========================================================================================

// Combat detection state
std::atomic<bool> g_autoSkillUseHookEnabled{ false };
std::atomic<int> g_autoSkillUseCallCount{ 0 };
std::atomic<std::chrono::steady_clock::time_point::rep> g_lastAutoSkillUseTime{ 0 };

// Hook function pointer
using AutoSkillUseFunction = int(__thiscall*)(void*, int, int, float, float, float, unsigned int, int, void*);
AutoSkillUseFunction g_originalAutoSkillUse = nullptr;

// Combat detection constants
constexpr int COMBAT_TIMEOUT_MS = 100;

// Timestamp update function
extern "C" void UpdateAutoSkillTimestamp() {
	auto now = std::chrono::steady_clock::now();
	g_lastAutoSkillUseTime.store(now.time_since_epoch().count());
}

// Lightweight hook that only tracks timing
__declspec(naked) int AutoSkillUseDetour() {
	__asm {
		// Minimal register preservation
		push eax
		push ecx

		// Update call count
		inc g_autoSkillUseCallCount

		// Update timestamp
		call UpdateAutoSkillTimestamp

		// Restore registers
		pop ecx
		pop eax

		// Jump to original function
		jmp g_originalAutoSkillUse
	}
}

// Query function for combat state
bool IsInCombatViaAutoSkill() {
	if (!g_autoSkillUseHookEnabled.load()) {
		return false;
	}

	auto lastCallTime = std::chrono::steady_clock::time_point(
		std::chrono::steady_clock::duration(g_lastAutoSkillUseTime.load())
	);

	auto now = std::chrono::steady_clock::now();
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCallTime);

	return elapsed.count() <= COMBAT_TIMEOUT_MS;
}

// Hook initialization
bool InitializeAutoSkillUseHook() {
	if (g_autoSkillUseHookEnabled.load()) {
		return true;
	}

	if (!g_gameBaseAddress) {
		g_gameBaseAddress = reinterpret_cast<uintptr_t>(GetModuleHandleA("wyd.exe"));
		if (!g_gameBaseAddress) {
			return false;
		}
	}

	uintptr_t targetAddress = g_gameBaseAddress + 0xB2340;

	MH_STATUS status = MH_CreateHook(
		reinterpret_cast<LPVOID>(targetAddress),
		reinterpret_cast<LPVOID>(&AutoSkillUseDetour),
		reinterpret_cast<LPVOID*>(&g_originalAutoSkillUse)
	);

	if (status != MH_OK) {
		return false;
	}

	status = MH_EnableHook(reinterpret_cast<LPVOID>(targetAddress));
	if (status != MH_OK) {
		MH_RemoveHook(reinterpret_cast<LPVOID>(targetAddress));
		return false;
	}

	g_autoSkillUseHookEnabled.store(true);
	return true;
}

// Hook cleanup
void CleanupAutoSkillUseHook() {
	if (!g_autoSkillUseHookEnabled.load()) {
		return;
	}

	uintptr_t targetAddress = g_gameBaseAddress + 0xB1F50;
	MH_DisableHook(reinterpret_cast<LPVOID>(targetAddress));
	MH_RemoveHook(reinterpret_cast<LPVOID>(targetAddress));

	g_autoSkillUseHookEnabled.store(false);
	g_originalAutoSkillUse = nullptr;
}

// Function to save precomputed pathfinding data
void SavePathfindingData() {
	std::ofstream outFile("lan_pathfinding.dat", std::ios::binary);
	if (!outFile) {
		AddBotDebugMessage("Failed to create pathfinding data file", LOG_ERROR);
		return;
	}

	// Save room count
	int roomCount = g_indexToRoom.size();
	outFile.write(reinterpret_cast<char*>(&roomCount), sizeof(roomCount));

	// Save room mapping
	for (int i = 0; i < roomCount; i++) {
		int roomId = g_indexToRoom[i];
		outFile.write(reinterpret_cast<char*>(&roomId), sizeof(roomId));
	}

	// Save distance matrix
	for (int i = 0; i < roomCount; i++) {
		for (int j = 0; j < roomCount; j++) {
			int distance = g_distanceMatrix[i][j];
			outFile.write(reinterpret_cast<char*>(&distance), sizeof(distance));
		}
	}

	// Save next hop matrix
	for (int i = 0; i < roomCount; i++) {
		for (int j = 0; j < roomCount; j++) {
			int nextHop = g_nextHopMatrix[i][j];
			outFile.write(reinterpret_cast<char*>(&nextHop), sizeof(nextHop));
		}
	}

	AddBotDebugMessage("Saved pathfinding data successfully", LOG_IMPORTANT);
}

// Initialize the fixed-size arrays for critical path operations
void InitializeFixedArrays() {
	// Clear the mappings
	memset(g_roomIDToFixedIndex, -1, sizeof(g_roomIDToFixedIndex));
	memset(g_fixedIndexToRoomID, -1, sizeof(g_fixedIndexToRoomID));

	// Ensure MAX_VALID_ROOMS is at least 36
	static_assert(MAX_VALID_ROOMS >= 36, "MAX_VALID_ROOMS must be at least 36");

	// First, prioritize assigning indices to all rooms (1-36)
	int index = 0;
	for (int roomId = 1; roomId <= 36; roomId++) {
		// Only assign if the room exists in the mapping
		if (g_roomToIndex.find(roomId) != g_roomToIndex.end()) {
			g_roomIDToFixedIndex[roomId] = index;
			g_fixedIndexToRoomID[index] = roomId;
			index++;
		}
	}
}

// Function to load precomputed pathfinding data
bool LoadPathfindingData() {
	std::ifstream inFile("lan_pathfinding.dat", std::ios::binary);
	if (!inFile) {
		return false;
	}

	// Load room count
	int roomCount;
	inFile.read(reinterpret_cast<char*>(&roomCount), sizeof(roomCount));

	// Clear existing data
	g_indexToRoom.clear();
	g_roomToIndex.clear();

	// Load room mapping
	for (int i = 0; i < roomCount; i++) {
		int roomId;
		inFile.read(reinterpret_cast<char*>(&roomId), sizeof(roomId));
		g_indexToRoom.push_back(roomId);
		g_roomToIndex[roomId] = i;
	}

	// Resize matrices
	g_distanceMatrix.resize(roomCount, std::vector<int>(roomCount));
	g_nextHopMatrix.resize(roomCount, std::vector<int>(roomCount));

	// Load distance matrix
	for (int i = 0; i < roomCount; i++) {
		for (int j = 0; j < roomCount; j++) {
			inFile.read(reinterpret_cast<char*>(&g_distanceMatrix[i][j]), sizeof(int));
		}
	}

	// Load next hop matrix
	for (int i = 0; i < roomCount; i++) {
		for (int j = 0; j < roomCount; j++) {
			inFile.read(reinterpret_cast<char*>(&g_nextHopMatrix[i][j]), sizeof(int));
		}
	}

	AddBotDebugMessage("Loaded precomputed pathfinding data", LOG_IMPORTANT);

	// Initialize fixed-size arrays for fast path operations
	InitializeFixedArrays();

	return true;
}


// Fast path existence check
bool PathExistsFast(int startRoom, int targetRoom) {
	// Quick check if both rooms are valid
	if (startRoom <= 0 || targetRoom <= 0) {
		return false;
	}

	// Get fixed indices
	int startIdx = g_roomIDToFixedIndex[startRoom];
	int targetIdx = g_roomIDToFixedIndex[targetRoom];

	// Check if indices are valid
	if (startIdx < 0 || startIdx >= MAX_VALID_ROOMS ||
		targetIdx < 0 || targetIdx >= MAX_VALID_ROOMS) {
		return false;
	}

	// Check if a path exists
	return g_distanceMatrix[startIdx][targetIdx] != INT_MAX;
}

// Fast path length check
int GetPathLengthFast(int startRoom, int targetRoom) {
	// Handle same room case
	if (startRoom == targetRoom) return 0;

	// Get fixed indices
	int startIdx = g_roomIDToFixedIndex[startRoom];
	int targetIdx = g_roomIDToFixedIndex[targetRoom];

	// Check if indices are valid
	if (startIdx < 0 || startIdx >= MAX_VALID_ROOMS ||
		targetIdx < 0 || targetIdx >= MAX_VALID_ROOMS) {
		return INT_MAX;
	}

	// Return the distance
	return g_distanceMatrix[startIdx][targetIdx];
}

// Generate a key for the path cache
std::string MakePathKey(int start, int target) {
	return std::to_string(start) + "->" + std::to_string(target);
}

//Floyd-Warshall pathfinding
// Reconstruct the path from start to target using the next hop matrix
std::vector<int> ReconstructPath(int start, int target) {
	// Handle edge cases
	if (start == target) {
		return { start };
	}

	// Check if rooms are valid
	if (g_roomToIndex.find(start) == g_roomToIndex.end() ||
		g_roomToIndex.find(target) == g_roomToIndex.end()) {
		return {};
	}

	// Convert room IDs to matrix indices
	int startIdx = g_roomToIndex[start];
	int targetIdx = g_roomToIndex[target];

	// Check if a path exists
	if (g_distanceMatrix[startIdx][targetIdx] == INT_MAX) {
		return {};
	}

	// Reconstruct the path
	std::vector<int> path;
	path.push_back(start);

	int currentRoom = start;
	while (currentRoom != target) {
		int currentIdx = g_roomToIndex[currentRoom];
		int targetIdx = g_roomToIndex[target];

		int nextRoom = g_nextHopMatrix[currentIdx][targetIdx];

		if (nextRoom == -1 || g_roomToIndex.find(nextRoom) == g_roomToIndex.end()) {
			return {};
		}

		path.push_back(nextRoom);
		currentRoom = nextRoom;

		// Protect against infinite loops
		if (path.size() > 50) {
			return {};
		}
	}

	return path;
}

// Get cached path or generate new one
std::vector<int> GetPathWithCache(int start, int target) {
	// Handle simple cases
	if (start == target) {
		return { start };
	}

	// Check for valid path quickly
	if (!PathExistsFast(start, target)) {
		return {};
	}

	// Generate cache key
	std::string key = MakePathKey(start, target);

	// Check if path is in cache
	auto now = std::chrono::steady_clock::now();
	if (g_pathCache.find(key) != g_pathCache.end()) {
		// Update last used time
		g_pathCache[key].lastUsed = now;
		return g_pathCache[key].path;
	}

	// Generate path using normal method
	std::vector<int> path = ReconstructPath(start, target);

	// Cache management - remove oldest entry if cache is full
	if (g_pathCache.size() >= MAX_CACHE_SIZE) {
		std::string oldestKey;
		auto oldestTime = now;

		for (const auto& entry : g_pathCache) {
			if (entry.second.lastUsed < oldestTime) {
				oldestTime = entry.second.lastUsed;
				oldestKey = entry.first;
			}
		}

		if (!oldestKey.empty()) {
			g_pathCache.erase(oldestKey);
		}
	}

	// Cache the new path
	g_pathCache[key] = { path, now };

	return path;
}

// Get the sector for a given room
int GetRoomSector(int roomId) {
	for (size_t i = 0; i < ROOM_SECTORS.size(); i++) {
		if (std::find(ROOM_SECTORS[i].begin(), ROOM_SECTORS[i].end(), roomId) != ROOM_SECTORS[i].end()) {
			return i;
		}
	}
	return -1;
}

//Floyd-Warshall pathfinding
// Get the length of the shortest path between two rooms
int GetPathLength(int start, int target) {
	// Handle same room case
	if (start == target) return 0;

	// Check if rooms are valid
	if (g_roomToIndex.find(start) == g_roomToIndex.end()) {
		return INT_MAX;
	}

	if (g_roomToIndex.find(target) == g_roomToIndex.end()) {
		return INT_MAX;
	}

	// Convert room IDs to matrix indices
	int startIdx = g_roomToIndex[start];
	int targetIdx = g_roomToIndex[target];

	// Return the distance from the matrix
	return g_distanceMatrix[startIdx][targetIdx];
}

// Initialize the Floyd-Warshall algorithm matrices
void InitializeFloydWarshall() {
	// Try to load precomputed data first
	if (LoadPathfindingData()) {
		return;  // Successfully loaded data, no need to recalculate
	}

	// If loading failed, perform normal calculation
	// Create room ID to matrix index mapping (now including all rooms)
	g_roomToIndex.clear();
	g_indexToRoom.clear();

	int index = 0;
	for (const auto& pair : g_rooms) {
		int roomId = pair.first;
		g_roomToIndex[roomId] = index;
		g_indexToRoom.push_back(roomId);
		index++;
	}

	// Get actual number of rooms
	const int roomCount = index;

	// Resize matrices to actual room count
	g_distanceMatrix.resize(roomCount, std::vector<int>(roomCount, INT_MAX));
	g_nextHopMatrix.resize(roomCount, std::vector<int>(roomCount, -1));

	// Set diagonal distances to 0
	for (int i = 0; i < roomCount; i++) {
		g_distanceMatrix[i][i] = 0;
		g_nextHopMatrix[i][i] = g_indexToRoom[i]; // Store actual room ID for self
	}

	// Initialize direct connections
	for (const auto& pair : g_rooms) {
		int fromRoom = pair.first;
		int fromIndex = g_roomToIndex[fromRoom];
		const Room* room = pair.second;

		for (const auto& connection : room->connections) {
			int toRoom = connection.targetRoom;
			int toIndex = g_roomToIndex[toRoom];
			g_distanceMatrix[fromIndex][toIndex] = 1;
			// Store the actual target room ID, not the index
			g_nextHopMatrix[fromIndex][toIndex] = toRoom;
		}
	}

	// Run Floyd-Warshall with optimized loop ordering for better cache locality
	for (int k = 0; k < roomCount; k++) {
		for (int i = 0; i < roomCount; i++) {
			// Skip if there's no path from i to k
			if (g_distanceMatrix[i][k] == INT_MAX) continue;

			for (int j = 0; j < roomCount; j++) {
				// Skip if there's no path from k to j
				if (g_distanceMatrix[k][j] == INT_MAX) continue;

				int newDist = g_distanceMatrix[i][k] + g_distanceMatrix[k][j];
				if (newDist < g_distanceMatrix[i][j]) {
					g_distanceMatrix[i][j] = newDist;
					g_nextHopMatrix[i][j] = g_nextHopMatrix[i][k];
				}
			}
		}
	}

	// Save the computed data for future sessions
	SavePathfindingData();
	// Initialize fixed-size arrays for fast path operations
	InitializeFixedArrays();
}


// Check if player is in LAN area
bool IsInLanArea() {
	int x = 0, y = 0;
	if (!GetPlayerPosition(x, y)) return false;

	// Check if within LAN area bounds (3500-3999)
	return (x >= 3500 && x <= 3999 && y >= 3500 && y <= 3999);
}

// structs for Move character to specified coordinates
struct TargetCoordinates {
	int x;
	int y;
};

// Original direct movement function
inline bool MoveToLocation(int x, int y) {
	uintptr_t sceneAddress = GetCurrentSceneAddress();
	if (!sceneAddress) return false;

	// Combine both coordinates into a single structure
	TargetCoordinates coords{ x, y };

	// Single memory write for both coordinates
	return WriteMemory<TargetCoordinates>(sceneAddress + Offsets::TMScene::TARGETX, coords);
}

// Get player position
bool GetPlayerPosition(int& x, int& y) {
	// Get local player
	uintptr_t playerAddress = GetLocalPlayerAddress();
	if (!playerAddress) return false;

	// Read position
	float posX = ReadFloat(playerAddress + Offsets::TMHuman::POSITION_X);
	float posY = ReadFloat(playerAddress + Offsets::TMHuman::POSITION_Y);

	// Convert to int
	x = static_cast<int>(posX);
	y = static_cast<int>(posY);

	return true;
}

// Check if player is moving
bool IsPlayerMoving() {
	uintptr_t playerAddress = GetLocalPlayerAddress();
	if (!playerAddress) return false;

	return ReadBool(playerAddress + Offsets::TMHuman::MOVING);
}

// Choose a random connected room
int chooseRandomNextRoom(int currentRoom) {
	if (g_rooms.find(currentRoom) == g_rooms.end() || g_rooms[currentRoom]->connections.empty()) {
		return 0;
	}

	// Get available connections
	const auto& connections = g_rooms[currentRoom]->connections;

	// Get all target rooms without filtering
	std::vector<int> validTargets;
	for (const auto& conn : connections) {
		validTargets.push_back(conn.targetRoom);
	}

	if (validTargets.empty()) {
		return 0;
	}

	// Choose random room from valid targets
	int randomIndex = rand() % validTargets.size();
	return validTargets[randomIndex];
}

// Separate function to find best connection - lives outside the class
RoomConnection FindBestRoomConnection(const Room* room, int targetRoom, int nextRoomAfterTarget) {
	// Vector to store matching connections
	std::vector<RoomConnection> matchingConnections;

	// Find all connections to the target room
	for (const auto& conn : room->connections) {
		if (conn.targetRoom == targetRoom) {
			matchingConnections.push_back(conn);
		}
	}

	// If only one connection exists, return it
	if (matchingConnections.size() <= 1) {
		return matchingConnections[0];
	}

	// If multiple connections exist and we know where we're going next
	if (nextRoomAfterTarget > 0) {
		// Check if next room boundary exists
		if (g_roomBoundaries.find(nextRoomAfterTarget) != g_roomBoundaries.end()) {
			const auto& boundary = g_roomBoundaries[nextRoomAfterTarget];

			// Calculate center of next room
			int centerX = (boundary.topLeft.x + boundary.bottomRight.x) / 2;
			int centerY = (boundary.topLeft.y + boundary.bottomRight.y) / 2;

			// Find connection with exit point closest to the center of the next room
			RoomConnection bestConn = matchingConnections[0];
			float bestDistance = FLT_MAX;

			for (const auto& conn : matchingConnections) {
				// Add explicit casts to float to prevent warnings
				float dx = static_cast<float>(conn.exitPoint.x - centerX);
				float dy = static_cast<float>(conn.exitPoint.y - centerY);
				float distance = std::sqrt(dx * dx + dy * dy);

				if (distance < bestDistance) {
					bestDistance = distance;
					bestConn = conn;
				}
			}

			AddBotDebugMessage("Selected connection with exit at (" +
				std::to_string(bestConn.exitPoint.x) + "," +
				std::to_string(bestConn.exitPoint.y) +
				") as closest to next destination", LOG_INFO);

			return bestConn;
		}
	}
	// Default to the first connection
	return matchingConnections[0];
}

// Get the waypoints to navigate to the next room
std::deque<Coordinate> getPathToRoom(int currentRoom, int targetRoom, int nextRoomAfterTarget = 0) {
	std::deque<Coordinate> waypoints;

	// Find the best connection between current and target room
	if (g_rooms.find(currentRoom) != g_rooms.end()) {
		RoomConnection bestConn = FindBestRoomConnection(g_rooms[currentRoom], targetRoom, nextRoomAfterTarget);

		// Add entry point first, then exit point
		waypoints.push_back(bestConn.entryPoint);
		waypoints.push_back(bestConn.exitPoint);

		AddBotDebugMessage("Selected path from room " + std::to_string(currentRoom) +
			" to room " + std::to_string(targetRoom), LOG_INFO);
	}

	return waypoints;
}

// Determine which room the player is in
int detectCurrentRoom() {
	// Get player position
	int x = 0, y = 0;
	if (!GetPlayerPosition(x, y)) return 0;

	// Check each room boundary
	for (const auto& pair : g_roomBoundaries) {
		int roomId = pair.first;
		const RoomBoundary& b = pair.second;

		// Get all x coordinates
		int x1 = b.topLeft.x, x2 = b.topRight.x;
		int x3 = b.bottomLeft.x, x4 = b.bottomRight.x;

		// Get all y coordinates
		int y1 = b.topLeft.y, y2 = b.topRight.y;
		int y3 = b.bottomLeft.y, y4 = b.bottomRight.y;

		// Calculate bounds using standard library functions
		int minX = std::min({ x1, x2, x3, x4 });
		int maxX = std::max({ x1, x2, x3, x4 });
		int minY = std::min({ y1, y2, y3, y4 });
		int maxY = std::max({ y1, y2, y3, y4 });

		// Rectangle containment check
		if (x >= minX && x <= maxX && y >= minY && y <= maxY) {
			return roomId;
		}
	}

	return 0; // Not in any room
}

// Add this function to double-check room detection
bool VerifyRoomDetection(int detectedRoom) {
	int x = 0, y = 0;
	if (!GetPlayerPosition(x, y)) return false;

	// Get room boundary
	if (g_roomBoundaries.find(detectedRoom) == g_roomBoundaries.end()) {
		return false;
	}

	const RoomBoundary& boundary = g_roomBoundaries[detectedRoom];

	// Calculate boundary rectangle
	int minX = std::min({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
	int maxX = std::max({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
	int minY = std::min({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });
	int maxY = std::max({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });

	// Verify we're actually in this boundary
	bool inBoundary = (x >= minX && x <= maxX && y >= minY && y <= maxY);

	if (!inBoundary) {
		AddBotDebugMessage("Room detection mismatch! Detected " + std::to_string(detectedRoom) +
			" but position (" + std::to_string(x) + "," + std::to_string(y) +
			") is outside bounds", LOG_ERROR);
	}

	return inBoundary;
}

// Cleanup rooms
void CleanupRooms() {
	for (auto& roomPair : g_rooms) {
		delete roomPair.second;
	}
	g_rooms.clear();
	g_roomBoundaries.clear();
}

// Initialize all rooms
void InitializeRooms() {
	// Clear existing data
	for (auto& roomPair : g_rooms) {
		delete roomPair.second;
	}
	g_rooms.clear();
	g_roomBoundaries.clear();

	// Create all rooms (1-36)
	for (int i = 1; i <= 36; i++) {
		g_rooms[i] = new Room(i);
	}

	// Setup room connections and boundaries
	setupRoomConnections();
	setupRoomBoundaries();

	// Initialize Floyd-Warshall algorithm
	InitializeFloydWarshall();

	// Initialize random number generator
	srand(static_cast<unsigned int>(time(nullptr)));

	// Clear path cache
	g_pathCache.clear();

	AddBotDebugMessage("Room data initialized with Floyd-Warshall pathfinding");
}

// Find NPCs in a specific room
std::vector<EntityInfo> FindNPCsInRoom(int roomId, const std::vector<EntityInfo>& allEntities) {
	std::vector<EntityInfo> roomNPCs;

	// Get room boundaries
	if (g_roomBoundaries.find(roomId) == g_roomBoundaries.end()) {
		return roomNPCs;
	}

	const RoomBoundary& boundary = g_roomBoundaries[roomId];

	// Find min/max coords of the room
	int minX = std::min({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
	int maxX = std::max({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
	int minY = std::min({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });
	int maxY = std::max({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });

	// Find living NPCs in this boundary
	for (const auto& entity : allEntities) {
		if (entity.isNPC && entity.currentHP > 0) { // Only include living NPCs
			int entityX = static_cast<int>(entity.posX);
			int entityY = static_cast<int>(entity.posY);

			if (entityX >= minX && entityX <= maxX && entityY >= minY && entityY <= maxY) {
				roomNPCs.push_back(entity);
			}
		}
	}

	return roomNPCs;
}

// Scan all rooms for NPCs
std::map<int, RoomNPCInfo> ScanRoomsForNPCs() {
	std::map<int, RoomNPCInfo> roomNPCMap;

	// Get all entities
	std::vector<EntityInfo> entities;
	{
		std::lock_guard<std::mutex> lock(g_entitiesMutex);
		entities = g_entities;
	}

	if (entities.empty()) {
		AddBotDebugMessage("No entities found during room scan", LOG_ERROR);
		return roomNPCMap;
	}

	// Find NPCs in each room
	for (const auto& roomPair : g_rooms) {
		int roomId = roomPair.first;
		auto roomNPCs = FindNPCsInRoom(roomId, entities);

		if (!roomNPCs.empty()) {
			RoomNPCInfo roomInfo;
			roomInfo.roomId = roomId;
			roomInfo.npcs = roomNPCs;
			roomInfo.pathLength = INT_MAX; // Will be calculated later

			roomNPCMap[roomId] = roomInfo;

			AddBotDebugMessage("Room " + std::to_string(roomId) + " has " +
				std::to_string(roomNPCs.size()) + " NPCs", LOG_IMPORTANT);
		}
	}

	return roomNPCMap;
}

// Modify or create this function to detect players and NPCs in rooms
std::map<int, RoomOccupancy> ScanRoomOccupancy() {
	std::map<int, RoomOccupancy> roomOccupancy;

	// Get all entities
	std::vector<EntityInfo> entities;
	{
		std::lock_guard<std::mutex> lock(g_entitiesMutex);
		entities = g_entities;
	}

	if (entities.empty()) {
		return roomOccupancy;
	}

	// Analyze each room for occupancy
	for (const auto& roomPair : g_rooms) {
		int roomId = roomPair.first;
		RoomOccupancy occupancy;

		// Check each entity to see if it's in this room
		for (size_t i = 0; i < entities.size(); i++) {
			const auto& entity = entities[i];

			// Check if entity is in this room
			if (g_roomBoundaries.find(roomId) != g_roomBoundaries.end()) {
				const auto& boundary = g_roomBoundaries[roomId];

				int minX = std::min({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
				int maxX = std::max({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
				int minY = std::min({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });
				int maxY = std::max({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });

				int entityX = static_cast<int>(entity.posX);
				int entityY = static_cast<int>(entity.posY);

				if (entityX >= minX && entityX <= maxX && entityY >= minY && entityY <= maxY) {
					if (entity.isNPC && entity.currentHP > 0) {
						occupancy.hasNPCs = true;
					}
					else if (entity.id > 0 && entity.id < 999 && !entity.isLocalPlayer && entity.currentHP > 0) {
						occupancy.hasPlayer = true;
						occupancy.playersCount++;
						occupancy.players.push_back(entity);
					}
				}
			}
		}

		// Only store if room has entities
		if (occupancy.hasPlayer || occupancy.hasNPCs) {
			roomOccupancy[roomId] = occupancy;
		}
	}

	return roomOccupancy;
}

// Get shortest path between rooms using pre-computed Floyd-Warshall data and caching
std::vector<int> FindShortestPath(int startRoom, int targetRoom) {
	// Add diagnostic logging if target is room 6
	if (targetRoom == 6) {
		AddBotDebugMessage("PATHFINDING DEBUG: Attempting to find path from room " +
			std::to_string(startRoom) + " to room 6", LOG_ERROR);

		// Check if indexes exist
		if (g_roomToIndex.find(startRoom) != g_roomToIndex.end() &&
			g_roomToIndex.find(6) != g_roomToIndex.end()) {

			int startIdx = g_roomToIndex[startRoom];
			int targetIdx = g_roomToIndex[6];

			int distance = g_distanceMatrix[startIdx][targetIdx];
			int nextHop = g_nextHopMatrix[startIdx][targetIdx];

			AddBotDebugMessage("PATHFINDING DEBUG: Distance = " +
				std::to_string(distance) +
				", Next Hop = " + std::to_string(nextHop), LOG_ERROR);

			// Check direct connections
			if (g_rooms.find(startRoom) != g_rooms.end()) {
				bool directConnection = false;
				for (const auto& conn : g_rooms[startRoom]->connections) {
					if (conn.targetRoom == 6) {
						directConnection = true;
						break;
					}
				}
				AddBotDebugMessage("PATHFINDING DEBUG: Direct connection exists: " +
					std::string(directConnection ? "YES" : "NO"), LOG_ERROR);
			}
		}
		else {
			AddBotDebugMessage("PATHFINDING DEBUG: Room indexes not found in mapping", LOG_ERROR);
		}
	}

	// Use the cached path system for efficient path retrieval
	return GetPathWithCache(startRoom, targetRoom);
}

// Calculate paths to all rooms with NPCs
void CalculatePathsToNPCRooms(int currentRoom) {
	if (g_roomNPCMap.empty()) {
		AddBotDebugMessage("No rooms with NPCs found", LOG_IMPORTANT);
		return;
	}

	// Calculate paths and distances for each room with NPCs
	for (auto& roomPair : g_roomNPCMap) {
		auto& roomInfo = roomPair.second;

		// Use the fast path length function
		int pathLength = GetPathLengthFast(currentRoom, roomInfo.roomId);

		if (pathLength != INT_MAX) {
			// Store the path length
			roomInfo.pathLength = pathLength;

			// Generate the full path using the cache system
			roomInfo.path = GetPathWithCache(currentRoom, roomInfo.roomId);

			AddBotDebugMessage("Path to room " + std::to_string(roomInfo.roomId) +
				" length: " + std::to_string(roomInfo.pathLength), LOG_INFO);
		}
		else {
			AddBotDebugMessage("No path found to room " + std::to_string(roomInfo.roomId), LOG_ERROR);
		}
	}
}

// Find optimal room with NPCs based on fixed scoring
int FindOptimalNPCRoom() {
	if (g_roomNPCMap.empty()) {
		return 0;
	}

	// Storage for room scores with claim status
	std::vector<std::pair<int, std::pair<bool, double>>> roomScoresWithClaimStatus;

	// Use fixed weights for scoring components
	const double npcWeight = 3.0;          // Weight for NPC count
	const double distanceWeight = 10.0;    // Weight for distance
	const double clusterWeight = 3.0;      // Weight for NPC clustering

	AddBotDebugMessage("ROOM SELECTION: Using fixed weights - NPC: " + std::to_string(npcWeight) +
		", Distance: " + std::to_string(distanceWeight) +
		", Cluster: " + std::to_string(clusterWeight), LOG_IMPORTANT);

	// Calculate scores for all rooms with NPCs
	for (const auto& roomPair : g_roomNPCMap) {
		const auto& roomInfo = roomPair.second;
		int roomId = roomPair.first;

		// Skip rooms that are too far away (more than 5 room transitions)
		if (roomInfo.pathLength > 5) {
			AddBotDebugMessage("ROOM SELECTION: Skipping room " + std::to_string(roomId) +
				" as it's too far away (path length: " + std::to_string(roomInfo.pathLength) + ")", LOG_INFO);
			continue;
		}

		// Skip room 16 when SONIC mode is enabled
		if (g_sonicModeEnabled && roomId == 16) {
			AddBotDebugMessage("ROOM SELECTION: Skipping room 16 because SONIC mode is enabled", LOG_IMPORTANT);
			continue;
		}

		// Check if room is claimed
		bool roomIsClaimed = false;
		if (g_roomOccupancy.find(roomId) != g_roomOccupancy.end()) {
			const auto& occupancy = g_roomOccupancy[roomId];
			if (occupancy.hasPlayer && occupancy.hasNPCs) {
				roomIsClaimed = true;
				AddBotDebugMessage("ROOM SELECTION: Skipping claimed room " + std::to_string(roomId), LOG_INFO);
				continue;  // Skip claimed rooms entirely - don't even calculate scores
			}
		}

		// Skip rooms with invalid paths
		if (roomInfo.pathLength == INT_MAX || roomInfo.path.empty()) {
			AddBotDebugMessage("ROOM SELECTION: Skipping room " + std::to_string(roomId) +
				" with invalid path", LOG_INFO);
			continue;
		}

		// === Score Components ===
		// NPC count factor
		double npcScore = roomInfo.npcs.size() * npcWeight;

		// Distance factor - exponential penalty for distance
		double distanceScore = 30.0 / std::pow(roomInfo.pathLength * 0.8 + 1.0, 1.5) * distanceWeight;

		// Cluster score - prioritize adjacent NPCs
		double clusterScore = 0.0;
		int adjacentNpcCount = 0;

		// Count NPCs in adjacent rooms
		if (g_rooms.find(roomId) != g_rooms.end()) {
			for (const auto& connection : g_rooms[roomId]->connections) {
				int adjacentRoom = connection.targetRoom;

				// Skip claimed adjacent rooms
				bool isAdjRoomClaimed = false;
				if (g_roomOccupancy.find(adjacentRoom) != g_roomOccupancy.end()) {
					const auto& adjOccupancy = g_roomOccupancy[adjacentRoom];
					if (adjOccupancy.hasPlayer && adjOccupancy.hasNPCs) {
						isAdjRoomClaimed = true;
					}
				}

				// Add score contribution from unclaimed adjacent rooms with NPCs
				if (!isAdjRoomClaimed && g_roomNPCMap.find(adjacentRoom) != g_roomNPCMap.end()) {
					int adjNpcs = g_roomNPCMap[adjacentRoom].npcs.size();
					adjacentNpcCount += adjNpcs;
					clusterScore += adjNpcs * 0.8 * clusterWeight;
				}
			}
		}

		// Add density score
		double npcDensity = roomInfo.npcs.size() / (roomInfo.pathLength > 0 ? roomInfo.pathLength : 1);
		double densityScore = npcDensity * 2.0;

		// === Player Proximity Penalty ===
		double playerProximityPenalty = 0.0;
		for (const auto& occPair : g_roomOccupancy) {
			int occupiedRoomId = occPair.first;
			const auto& occupancy = occPair.second;

			// Skip rooms without players
			if (!occupancy.hasPlayer || occupancy.playersCount == 0)
				continue;

			// Calculate distance between rooms
			int distance = GetPathLengthFast(roomId, occupiedRoomId);
			if (distance == INT_MAX || distance <= 0)
				continue;

			// Apply exponential decay penalty based on distance
			double penalty = occupancy.playersCount * 10.0 * exp(-distance * 0.7);
			playerProximityPenalty += penalty;
		}

		// === Calculate final score ===
		double finalScore = npcScore + densityScore + distanceScore + clusterScore;
		finalScore -= playerProximityPenalty;

		// Apply bonus for rooms that excel in all 3 priority areas
		if (roomInfo.npcs.size() >= 3 && roomInfo.pathLength <= 2 && adjacentNpcCount >= 3) {
			finalScore *= 1.3;  // 30% bonus for rooms that match all key criteria
			AddBotDebugMessage("ROOM SELECTION: Room " + std::to_string(roomId) +
				" gets 30% priority bonus for meeting all criteria", LOG_IMPORTANT);
		}

		// Apply penalty to single-NPC rooms (fixed penalty instead of metrics-based)
		if (roomInfo.npcs.size() <= 1) {
			finalScore *= 0.2;  // 80% penalty for single NPC rooms
		}

		// Log detailed score components for debugging
		AddBotDebugMessage("ROOM SELECTION: Room " + std::to_string(roomId) +
			" scores: NPCs=" + std::to_string(roomInfo.npcs.size()) +
			", distance=" + std::to_string(roomInfo.pathLength) +
			", adjNPCs=" + std::to_string(adjacentNpcCount) +
			", npcScore=" + std::to_string(npcScore) +
			", distScore=" + std::to_string(distanceScore) +
			", cluster=" + std::to_string(clusterScore) +
			", final=" + std::to_string(finalScore), LOG_IMPORTANT);

		// Store the room score with claim status (false = unclaimed)
		roomScoresWithClaimStatus.push_back(std::make_pair(roomId, std::make_pair(roomIsClaimed, finalScore)));
	}

	// No valid rooms found
	if (roomScoresWithClaimStatus.empty()) {
		AddBotDebugMessage("ROOM SELECTION: No valid rooms found", LOG_IMPORTANT);
		return 0;
	}

	// Sort rooms by claim status first (unclaimed first), then by score
	std::sort(roomScoresWithClaimStatus.begin(), roomScoresWithClaimStatus.end(),
		[](const auto& a, const auto& b) {
			// If claim status is different, unclaimed (false) comes first
			if (a.second.first != b.second.first) {
				return a.second.first < b.second.first;
			}
			// Otherwise, sort by score (higher is better)
			return a.second.second > b.second.second;
		});

	// Extract just the room IDs and scores for the rest of the function
	std::vector<std::pair<int, double>> roomScores;
	for (const auto& item : roomScoresWithClaimStatus) {
		roomScores.push_back(std::make_pair(item.first, item.second.second));
	}

	// Choose from top N rooms with weighted probability
	int numChoices = std::min(3, static_cast<int>(roomScores.size()));

	// If only one choice, return it directly
	if (numChoices <= 1) {
		AddBotDebugMessage("ROOM SELECTION: Selected single available room " +
			std::to_string(roomScores[0].first) +
			" (score: " + std::to_string(roomScores[0].second) + ")", LOG_IMPORTANT);
		return roomScores[0].first;
	}

	// Create deterministic but unique seed 
	unsigned int seed = static_cast<unsigned int>(GetCurrentProcessId());
	// Use current time as part of the seed instead of metrics
	seed ^= static_cast<unsigned int>(
		std::chrono::steady_clock::now().time_since_epoch().count());

	std::mt19937 gen(seed);

	// Create weighted distribution favoring higher scores
	std::vector<double> weights(numChoices);
	for (int i = 0; i < numChoices; i++) {
		// Exponential decay of probability
		weights[i] = exp(-i * 0.7);
	}

	std::discrete_distribution<int> dist(weights.begin(), weights.end());
	int selectedIndex = dist(gen);

	int chosenRoomId = roomScores[selectedIndex].first;

	AddBotDebugMessage("ROOM SELECTION: Selected room " + std::to_string(chosenRoomId) +
		" (ranked #" + std::to_string(selectedIndex + 1) + " of " +
		std::to_string(numChoices) + ", score: " +
		std::to_string(roomScores[selectedIndex].second) + ")", LOG_IMPORTANT);

	return chosenRoomId;
}

// Add this function to detect if our room is compromised
bool IsOurRoomClaimed() {
	// We are defending our room, so we don't care if others enter
	// This function is for future use if needed
	if (g_botState == BotState::COMBAT && g_currentRoom != 0) {
		if (g_roomOccupancy.find(g_currentRoom) != g_roomOccupancy.end()) {
			const auto& occupancy = g_roomOccupancy[g_currentRoom];
			// We only care if there are players AND we haven't started combat yet
			return occupancy.hasPlayer && occupancy.playersCount > 0;
		}
	}
	return false;
}

// Move toward closest NPC in room for combat
bool MoveToNearestNPC() {
	std::vector<EntityInfo> entities;
	{
		std::lock_guard<std::mutex> lock(g_entitiesMutex);
		entities = g_entities;
	}

	if (entities.empty() || entities.size() < 2) {
		return false;
	}

	// Player is always the first entity
	const EntityInfo& player = entities[0];
	auto currentTime = std::chrono::steady_clock::now();

	// Movement stagnation detection
	if (g_combatState.trackingMovement) {
		// Check if position has changed significantly (account for floating-point precision)
		float deltaX = abs(player.posX - g_combatState.lastKnownX);
		float deltaY = abs(player.posY - g_combatState.lastKnownY);
		bool positionChanged = (deltaX > 1.0f || deltaY > 1.0f);

		if (positionChanged) {
			// Movement successful - reset tracking
			g_combatState.trackingMovement = false;
			g_combatState.movementState = CombatState::NORMAL;
			g_combatState.movementAttempts = 0;
		}
		else {
			// Check if 500 ms have passed without movement
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				currentTime - g_combatState.lastMovementTime).count();

			if (elapsed >= 500) {
				switch (g_combatState.movementState) {
				case CombatState::NORMAL:
					// First stagnation detected - try room center approach
					g_combatState.movementState = CombatState::ROOM_CENTER_FALLBACK;
					g_combatState.movementAttempts++;
					AddBotDebugMessage("Movement stagnation detected, attempting room center fallback", LOG_IMPORTANT);

					// Calculate room center and move there
					if (g_roomBoundaries.find(g_currentRoom) != g_roomBoundaries.end()) {
						const auto& boundary = g_roomBoundaries[g_currentRoom];
						int centerX = (boundary.topLeft.x + boundary.bottomRight.x) / 2;
						int centerY = (boundary.topLeft.y + boundary.bottomRight.y) / 2;

						// Update tracking for room center movement
						g_combatState.lastKnownX = player.posX;
						g_combatState.lastKnownY = player.posY;
						g_combatState.lastMovementTime = currentTime;

						return MoveToLocation(centerX, centerY);
					}
					else {
						// No room boundary data - give up immediately
						g_combatState.movementState = CombatState::GIVE_UP;
					}
					break;

				case CombatState::ROOM_CENTER_FALLBACK:
					// Room center approach also failed - give up
					g_combatState.movementState = CombatState::GIVE_UP;
					g_combatState.movementAttempts++;
					AddBotDebugMessage("Room center fallback failed, abandoning movement approach", LOG_ERROR);
					break;

				case CombatState::GIVE_UP:
					// Reset state and return failure
					g_combatState.trackingMovement = false;
					g_combatState.movementState = CombatState::NORMAL;
					g_combatState.movementAttempts = 0;
					AddBotDebugMessage("Movement completely failed after " +
						std::to_string(g_combatState.movementAttempts) + " attempts", LOG_ERROR);
					return false;
				}
			}
		}

		// Still in stagnation handling mode - don't proceed with normal NPC targeting
		if (g_combatState.movementState != CombatState::NORMAL) {
			return true; // Return true to indicate we're still trying
		}
	}

	// Find closest living NPC - existing logic unchanged
	const EntityInfo* closestNPC = nullptr;
	float closestDistance = FLT_MAX;
	int npcCount = 0;

	for (size_t i = 1; i < entities.size(); i++) {
		const auto& entity = entities[i];

		if (entity.isNPC && entity.currentHP > 0) {
			bool shouldConsider = false;

			if (g_combatState.inCombat) {
				shouldConsider = true;
			}
			else {
				Coordinate entityPos(static_cast<int>(entity.posX), static_cast<int>(entity.posY));

				if (g_roomBoundaries.find(g_currentRoom) != g_roomBoundaries.end()) {
					const auto& boundary = g_roomBoundaries[g_currentRoom];

					int minX = std::min({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
					int maxX = std::max({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
					int minY = std::min({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });
					int maxY = std::max({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });

					if (entityPos.x >= minX && entityPos.x <= maxX && entityPos.y >= minY && entityPos.y <= maxY) {
						shouldConsider = true;
						npcCount++;
					}
				}
			}

			if (shouldConsider && entity.distance < closestDistance) {
				closestDistance = entity.distance;
				closestNPC = &entity;
			}
		}
	}

	if (closestNPC) {
		// Special handling for room 28 stuck position - existing logic unchanged
		if (g_currentRoom == 28) {
			if (abs(static_cast<int>(closestNPC->posX) - 3931) <= 2 &&
				abs(static_cast<int>(closestNPC->posY) - 3660) <= 2) {

				AddBotDebugMessage("NPC detected in stuck position, moving to special attack position", LOG_IMPORTANT);
				g_combatState.currentTargetAddress = closestNPC->address;
				return MoveToLocation(3928, 3660);
			}
		}

		float attackRange = g_combatState.isPhysicalWeapon ? 5.0f : 6.0f;

		// If already within attack range, no movement needed - existing logic unchanged
		if (closestDistance <= attackRange) {
			g_combatState.currentTargetAddress = closestNPC->address;
			AddBotDebugMessage("Already in range of " + closestNPC->name +
				" (distance: " + std::to_string(closestDistance) + ")", LOG_INFO);
			return true;
		}

		// Calculate optimal position - existing logic unchanged
		const float optimalDistance = 3.5f;

		float dx = player.posX - closestNPC->posX;
		float dy = player.posY - closestNPC->posY;
		float currentDistance = std::sqrt(dx * dx + dy * dy);

		if (currentDistance > 0) {
			dx /= currentDistance;
			dy /= currentDistance;
		}

		float targetX = closestNPC->posX + (dx * optimalDistance);
		float targetY = closestNPC->posY + (dy * optimalDistance);

		AddBotDebugMessage("Moving toward " + closestNPC->name +
			" (current distance: " + std::to_string(closestDistance) +
			", target distance: " + std::to_string(optimalDistance) + ")", LOG_IMPORTANT);

		g_combatState.currentTargetAddress = closestNPC->address;

		// Initialize movement tracking before issuing movement command
		g_combatState.trackingMovement = true;
		g_combatState.lastKnownX = player.posX;
		g_combatState.lastKnownY = player.posY;
		g_combatState.lastMovementTime = currentTime;
		g_combatState.movementState = CombatState::NORMAL;

		return MoveToLocation(static_cast<int>(targetX), static_cast<int>(targetY));
	}

	return false;
}

// Check if all NPCs in room are defeated
bool AreAllNPCsDefeated() {
	std::vector<EntityInfo> entities;
	{
		std::lock_guard<std::mutex> lock(g_entitiesMutex);
		entities = g_entities;
	}

	// MODIFIED: During combat, check all NPCs regardless of location
	if (g_combatState.inCombat) {
		// In combat: look for any living NPCs anywhere
		for (size_t i = 1; i < entities.size(); i++) {
			const auto& entity = entities[i];
			if (entity.isNPC && entity.currentHP > 0) {
				return false; // Found living NPC anywhere
			}
		}
		return true; // No living NPCs found anywhere
	}

	// Original logic for when not in combat: Look for any living NPCs in current room
	for (size_t i = 1; i < entities.size(); i++) {
		const auto& entity = entities[i];

		if (entity.isNPC && entity.currentHP > 0) {
			// Check if entity is in current room
			Coordinate entityPos(static_cast<int>(entity.posX), static_cast<int>(entity.posY));

			if (g_roomBoundaries.find(g_currentRoom) != g_roomBoundaries.end()) {
				const auto& boundary = g_roomBoundaries[g_currentRoom];

				int minX = std::min({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
				int maxX = std::max({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
				int minY = std::min({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });
				int maxY = std::max({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });

				if (entityPos.x >= minX && entityPos.x <= maxX && entityPos.y >= minY && entityPos.y <= maxY) {
					return false; // Found living NPC in room
				}
			}
		}
	}

	return true; // No living NPCs found in room
}

// Remove this room from the NPC room map
void RemoveRoomFromNPCMap(int roomId) {
	if (g_roomNPCMap.find(roomId) != g_roomNPCMap.end()) {
		g_roomNPCMap.erase(roomId);
	}
}

// Count the number of living NPCs in the current room
int CountNPCsInCurrentRoom() {
	int count = 0;
	std::vector<EntityInfo> entities;

	{
		std::lock_guard<std::mutex> lock(g_entitiesMutex);
		entities = g_entities;
	}

	// Get room boundary
	if (g_roomBoundaries.find(g_currentRoom) == g_roomBoundaries.end()) {
		return 0;
	}

	const auto& boundary = g_roomBoundaries[g_currentRoom];

	// Calculate min/max coordinates of the room
	int minX = std::min({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
	int maxX = std::max({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
	int minY = std::min({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });
	int maxY = std::max({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });

	// Count living NPCs in this boundary
	for (const auto& entity : entities) {
		if (entity.isNPC && entity.currentHP > 0) {
			int entityX = static_cast<int>(entity.posX);
			int entityY = static_cast<int>(entity.posY);

			if (entityX >= minX && entityX <= maxX && entityY >= minY && entityY <= maxY) {
				count++;
			}
		}
	}

	return count;
}

// Combat update function - Simplified version focusing on primary detection methods
void UpdateCombat() {
	auto currentTime = std::chrono::steady_clock::now();

	// === PRIMARY COMBAT DETECTION ===
	// For magical weapons: Use AutoSkillUse hook (counts spell casts)
	// For physical weapons: Use hit counter (counts physical attacks)

	bool combatDetected = false;

	if (g_combatState.isPhysicalWeapon) {
		// Physical weapon detection via hit counter
		int currentHitCounter = GetHitCounter();

		if (currentHitCounter != g_combatState.lastHitCounter) {
			// Hit counter changed - we're definitely in combat
			g_combatState.lastHitCounter = currentHitCounter;
			g_combatState.lastHitTime = currentTime;
			combatDetected = true;

			AddBotDebugMessage("Physical combat detected - hit counter: " +
				std::to_string(currentHitCounter), LOG_INFO);
		}
		else {
			// Check if we're still within the combat timeout window
			auto timeSinceLastHit = std::chrono::duration_cast<std::chrono::milliseconds>(
				currentTime - g_combatState.lastHitTime).count();

			combatDetected = (timeSinceLastHit <= 500); // 500ms timeout for physical combat
		}
	}
	else {
		// Magical weapon detection via AutoSkillUse hook
		combatDetected = IsInCombatViaAutoSkill();

		if (combatDetected) {
			// Update our internal timing for consistency
			g_combatState.lastHitTime = currentTime;
		}
	}

	// === COMBAT STATE MANAGEMENT ===
	if (combatDetected) {
		if (!g_combatState.inCombat) {
			// Combat just started
			AddBotDebugMessage("Combat started (" +
				std::string(g_combatState.isPhysicalWeapon ? "Physical" : "Magical") + ")",
				LOG_IMPORTANT);
			g_combatState.inCombat = true;
		}
		// Combat is ongoing - state remains active
	}
	else {
		if (g_combatState.inCombat) {
			// Combat just ended
			AddBotDebugMessage("Combat ended", LOG_IMPORTANT);
			g_combatState.inCombat = false;

			// Check if we need to engage another target
			if (!AreAllNPCsDefeated()) {
				AddBotDebugMessage("Seeking new target - NPCs still alive", LOG_IMPORTANT);
				if (MoveToNearestNPC()) {
					// Successfully found and moved to new target
					g_combatState.inCombat = true;
					g_combatState.lastHitTime = currentTime;
				}
				else {
					AddBotDebugMessage("No valid targets found for engagement", LOG_INFO);
				}
			}
			else {
				AddBotDebugMessage("All NPCs defeated - combat complete", LOG_IMPORTANT);
			}
		}
	}

	// === TARGET HEALTH MONITORING ===
	// This isn't for combat detection - it's for knowing when our current target dies
	if (g_combatState.currentTargetAddress != 0) {
		int targetHealth = ReadInt(g_combatState.currentTargetAddress + Offsets::TMHuman::HEALTH);

		if (targetHealth <= 0) {
			AddBotDebugMessage("Current target defeated", LOG_IMPORTANT);
			g_combatState.currentTargetAddress = 0;
			g_combatState.lastTargetHP = 0;

			// Brief pause to let the game update, then look for next target
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			MoveToNearestNPC();
		}
	}

	// === NPC STAGNATION DETECTION ===
	// This handles the edge case of NPCs that get stuck and can't be damaged
	if (g_combatState.currentTargetAddress != 0 && g_combatState.inCombat) {
		int currentHP = ReadInt(g_combatState.currentTargetAddress + Offsets::TMHuman::HEALTH);
		int npcCount = CountNPCsInCurrentRoom();
		bool singleNpcInRoom = (npcCount == 1);

		// Only check stagnation in single-NPC rooms to avoid false positives
		if (singleNpcInRoom) {
			// Initialize HP tracking if needed
			if (g_combatState.lastTargetHP == 0 && currentHP > 0) {
				g_combatState.lastTargetHP = currentHP;
				g_combatState.lastHPChangeTime = currentTime;
				g_combatState.stagnantHPCounter = 0;
			}

			// Check for HP changes
			if (currentHP != g_combatState.lastTargetHP) {
				// HP changed - reset stagnation tracking
				g_combatState.lastTargetHP = currentHP;
				g_combatState.lastHPChangeTime = currentTime;
				g_combatState.stagnantHPCounter = 0;
			}
			else {
				// HP hasn't changed - check how long it's been stagnant
				auto stagnantTime = std::chrono::duration_cast<std::chrono::seconds>(
					currentTime - g_combatState.lastHPChangeTime).count();

				// Increment counter every 5 seconds
				if (stagnantTime >= 5 && stagnantTime % 5 == 0 &&
					std::chrono::duration_cast<std::chrono::milliseconds>(
						currentTime - g_combatState.lastHPChangeTime).count() % 5000 < 100) {

					g_combatState.stagnantHPCounter++;

					if (g_combatState.stagnantHPCounter >= 4) { // 20 seconds of no HP change
						AddBotDebugMessage("Detected stuck NPC - attempting entity deletion", LOG_ERROR);

						// Try to delete the stuck entity
						DeleteEntity(g_combatState.currentTargetAddress);

						// Force exit from combat
						SetAutoAttack(false, g_combatState.weaponType);
						g_combatState.inCombat = false;
						g_combatState.currentTargetAddress = 0;
						g_combatState.lastTargetHP = 0;
						g_combatState.stagnantHPCounter = 0;

						// Force state transition to cleanup
						g_botState = BotState::CLEANUP;
						g_botStatusMessage = "Exiting room with stuck NPC";
						return;
					}
				}
			}
		}
		else {
			// Reset stagnation tracking in multi-NPC rooms
			g_combatState.stagnantHPCounter = 0;
		}
	}
}

void ValidateRoomBoundaries(int roomId, int x, int y) {
	if (g_roomBoundaries.find(roomId) == g_roomBoundaries.end()) {
		AddBotDebugMessage("Room " + std::to_string(roomId) + " has no boundary data!", LOG_ERROR);
		return;
	}

	const RoomBoundary& boundary = g_roomBoundaries[roomId];

	// Calculate min/max coordinates
	int minX = std::min({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
	int maxX = std::max({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
	int minY = std::min({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });
	int maxY = std::max({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });

	// Check if position is within bounds
	bool inBounds = (x >= minX && x <= maxX && y >= minY && y <= maxY);

	std::string boundaryInfo =
		"Room " + std::to_string(roomId) +
		" bounds: (" + std::to_string(minX) + "," + std::to_string(minY) + ") to (" +
		std::to_string(maxX) + "," + std::to_string(maxY) + ")";

	if (!inBounds) {
		AddBotDebugMessage("Position (" + std::to_string(x) + "," + std::to_string(y) +
			") outside " + boundaryInfo, LOG_ERROR);
	}
	else {
		AddBotDebugMessage("Position (" + std::to_string(x) + "," + std::to_string(y) +
			") inside " + boundaryInfo, LOG_INFO);
	}
}

int SelectWanderTarget(int currentRoom) {
	// Define preferred rooms with higher probability (central rooms with good coverage)
	std::vector<std::pair<int, int>> preferredRooms = {
		{15, 5},  // Room 15 - central position
		{16, 4},  // Room 16 - good coverage
		{21, 4},  // Room 21 - good coverage
		{22, 4},  // Room 22 - good coverage
		{14, 3},  // Room 14 - decent coverage
		{20, 3},  // Room 20 - decent coverage
		{27, 3},  // Room 27 - decent coverage
		{28, 3}   // Room 28 - decent coverage
	};

	// Create weighted list based on preference
	std::vector<int> candidateRooms;
	for (const auto& [roomId, weight] : preferredRooms) {
		// Skip current room
		if (roomId == currentRoom) continue;

		// Add room multiple times based on weight
		for (int i = 0; i < weight; i++) {
			candidateRooms.push_back(roomId);
		}
	}

	// Add all other valid rooms with lower weight
	for (const auto& roomPair : g_rooms) {
		int roomId = roomPair.first;

		// Skip already added rooms and current room
		if (roomId == currentRoom) continue;

		bool alreadyAdded = false;
		for (const auto& [prefRoom, _] : preferredRooms) {
			if (prefRoom == roomId) {
				alreadyAdded = true;
				break;
			}
		}

		if (!alreadyAdded) {
			candidateRooms.push_back(roomId);
		}
	}

	// Select random room from candidates
	if (!candidateRooms.empty()) {
		int randomIndex = rand() % candidateRooms.size();
		return candidateRooms[randomIndex];
	}

	return 0;
}

// Add this function to send key inputs to the game
bool SendKeyPress(WORD key)
{
	// Create an input event for key down
	INPUT input[2] = {};
	input[0].type = INPUT_KEYBOARD;
	input[0].ki.wVk = key;

	// Create an input event for key up
	input[1].type = INPUT_KEYBOARD;
	input[1].ki.wVk = key;
	input[1].ki.dwFlags = KEYEVENTF_KEYUP;

	// Send the inputs
	UINT result = SendInput(2, input, sizeof(INPUT));
	return result == 2;
}

// Increments the skill selection in the hotkey bar
bool IncSkillSel()
{
	if (!IsInFieldMode()) return false;

	__try {
		uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("wyd.exe"));
		uintptr_t sceneAddress = GetCurrentSceneAddress();
		if (!sceneAddress) return false;

		// The function expects the scene as 'this' pointer
		// It accesses the ObjectManager at 1ACAB38 internally
		using IncSkillSelFn = void(__thiscall*)(void*);
		IncSkillSelFn incSkillFunc = reinterpret_cast<IncSkillSelFn>(moduleBase + 0xBCA70);

		// Pass the scene as the 'this' pointer, not the ObjectManager
		incSkillFunc(reinterpret_cast<void*>(sceneAddress));
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Isolated function for raw memory operations only - no C++ objects
static int CallOnControlEventRaw(uintptr_t sceneAddress, int controlId) {
	__try {
		uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("wyd.exe"));
		if (!moduleBase || !sceneAddress) {
			return -1; // Error indicator
		}

		using OnControlEventFn = int(__thiscall*)(void*, int, int);
		OnControlEventFn controlEventFunc = reinterpret_cast<OnControlEventFn>(moduleBase + 0x9C120);

		return controlEventFunc(reinterpret_cast<void*>(sceneAddress), controlId, 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return -2; // Exception indicator
	}
}

bool ToggleSkillPage() {
	if (!IsInFieldMode()) {
		AddBotDebugMessage("ToggleSkillPage: Not in field mode", LOG_ERROR);
		return false;
	}

	// Read current page before attempting switch
	int currentPage = GetCurrentSkillPage();
	if (currentPage == -1) {
		AddBotDebugMessage("Failed to read current skill page", LOG_ERROR);
		return false;
	}

	// Sync our internal state with actual game state before switching
	g_onPage2 = (currentPage == 1);

	AddBotDebugMessage("Current skill page detected: " + std::to_string(currentPage + 1) +
		" (switching to " + std::to_string((currentPage == 0) ? 2 : 1) + ")", LOG_IMPORTANT);

	// Get scene address outside of try block
	uintptr_t sceneAddress = GetCurrentSceneAddress();
	if (!sceneAddress) {
		AddBotDebugMessage("Failed to get scene address for skill page switch", LOG_ERROR);
		return false;
	}

	// Determine control ID based on current page (switch to opposite)
	int controlId = (currentPage == 0) ? 65647 : 65646;

	AddBotDebugMessage("Calling OnControlEvent with controlId: " + std::to_string(controlId), LOG_IMPORTANT);

	// Call the isolated function for protected memory access
	int result = CallOnControlEventRaw(sceneAddress, controlId);

	if (result == -2) {
		AddBotDebugMessage("Exception occurred during skill page switch", LOG_ERROR);
		return false;
	}

	if (result == -1) {
		AddBotDebugMessage("Failed to get module base or scene address", LOG_ERROR);
		return false;
	}

	AddBotDebugMessage("OnControlEvent returned: " + std::to_string(result), LOG_IMPORTANT);

	// CRITICAL: Call IncSkillSel immediately after page switch attempt
	if (!IncSkillSel()) {
		AddBotDebugMessage("Failed to increment skill selection after page switch", LOG_ERROR);
		// Continue anyway - page switch may have succeeded
	}
	else {
		AddBotDebugMessage("Skill selection incremented after page switch", LOG_IMPORTANT);
	}

	if (result != 0) {
		AddBotDebugMessage("OnControlEvent failed with result: " + std::to_string(result), LOG_ERROR);
		return false;
	}

	// Wait for the change to take effect
	std::this_thread::sleep_for(std::chrono::milliseconds(250));

	// Verify the switch actually worked
	int newPage = GetCurrentSkillPage();
	if (newPage == -1) {
		AddBotDebugMessage("Failed to verify skill page switch", LOG_ERROR);
		return false;
	}

	// Check if the page actually changed
	if (newPage != currentPage) {
		g_onPage2 = (newPage == 1);
		AddBotDebugMessage("Skill page switch verified: Now on Page " +
			std::to_string(newPage + 1), LOG_IMPORTANT);
		return true;
	}
	else {
		AddBotDebugMessage("Skill page switch failed - page didn't change", LOG_ERROR);
		return false;
	}
}

// Synchronize internal skill page state with actual game state
bool SynchronizeSkillPageState() {
	int actualPage = GetCurrentSkillPage();
	if (actualPage == -1) {
		return false;
	}

	bool wasDesync = (g_onPage2 != (actualPage == 1));
	g_onPage2 = (actualPage == 1);

	if (wasDesync) {
		AddBotDebugMessage("Skill page state synchronized: Actually on Page " +
			std::to_string(actualPage + 1), LOG_IMPORTANT);
	}

	return true;
}

//Method 1: Centralized State Transition (Recommended)
void TransitionBotState(BotState newState, const std::string& statusMessage) {
	// Skip if we're already in this state
	if (g_botState == newState)
		return;

	// Log the state transition for debugging
	std::string oldStateName;
	std::string newStateName;

	// Convert enum to strings for logging
	switch (g_botState) {
	case BotState::IDLE: oldStateName = "IDLE"; break;
	case BotState::SCANNING_ROOMS: oldStateName = "SCANNING_ROOMS"; break;
	case BotState::NAVIGATING: oldStateName = "NAVIGATING"; break;
	case BotState::COMBAT: oldStateName = "COMBAT"; break;
	case BotState::CLEANUP: oldStateName = "CLEANUP"; break;
	default: oldStateName = "UNKNOWN"; break;
	}

	switch (newState) {
	case BotState::IDLE: newStateName = "IDLE"; break;
	case BotState::SCANNING_ROOMS: newStateName = "SCANNING_ROOMS"; break;
	case BotState::NAVIGATING: newStateName = "NAVIGATING"; break;
	case BotState::COMBAT: newStateName = "COMBAT"; break;
	case BotState::CLEANUP: newStateName = "CLEANUP"; break;
	default: newStateName = "UNKNOWN"; break;
	}

	AddBotDebugMessage("STATE TRANSITION: " + oldStateName + " -> " + newStateName, LOG_IMPORTANT);

	// Change state and update status message
	g_botState = newState;
	g_botStatusMessage = statusMessage;
}

void LanBotThread() {
	AddBotDebugMessage("LAN Bot thread started", LOG_IMPORTANT);

	// Set initial movement speed
	SetMovementSpeed(g_movementSpeed);

	// System timing parameters
	auto lastMovementTime = std::chrono::steady_clock::now();
	auto lastPositionCheckTime = std::chrono::steady_clock::now();
	auto lastPositionLogTime = std::chrono::steady_clock::now();
	int lastX = 0, lastY = 0;
	bool initializedLanTime = false;
	static bool hasInitializedLan = false;

	// Timing constants - strategically chosen to prevent overlapping
	constexpr int WAYPOINT_WAIT_TIME_MS = 100;         // Time to wait at waypoints
	constexpr int MOVEMENT_RETRY_INTERVAL_MS = 1000;  // Time between movement retries
	constexpr int POSITION_CHECK_INTERVAL_MS = 33;     // Position check frequency
	constexpr int INITIAL_GRACE_PERIOD_SEC = 8;       // Initial wait period
	constexpr int ROOM_SCAN_INTERVAL_SEC = 30;        // Time between room scans
	constexpr int SEQUENCE_TIMEOUT_SEC = 10;          // Maximum time for waypoint sequence

	// State initialization
	g_currentRoom = 0;
	g_targetRoom = 0;
	g_pathWaypoints.clear();
	g_botState = BotState::IDLE;

	// Initialize combat state
	g_combatState = CombatState{
		false, 0, 0, std::chrono::steady_clock::now(),
		{}, 0,
		// Fields for magical attacks
		0, 0, std::chrono::steady_clock::now(), true
	};

	// Add weapon type detection at startup
	bool weaponDetected = false;
	while (g_lanBotRunning && !weaponDetected) {
		if (IsInLanArea()) {
			g_combatState.weaponType = GetPlayerWeaponType();
			if (g_combatState.weaponType != 0) {
				g_combatState.isPhysicalWeapon = (g_combatState.weaponType == 101);
				weaponDetected = true;

				AddBotDebugMessage("Weapon type detected: " + std::to_string(g_combatState.weaponType) +
					" (" + (g_combatState.isPhysicalWeapon ? "Physical" : "Magical") + ")", LOG_IMPORTANT);

				// Initialize mana tracking for magical weapons
				if (!g_combatState.isPhysicalWeapon) {
					g_combatState.lastMana = GetPlayerMana();
				}
			}
		}

		if (!weaponDetected) {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
	}

	while (g_lanBotRunning) {
		// Get current time early for calculations
		auto currentTime = std::chrono::steady_clock::now();

		// Check if in LAN area
		if (!IsInLanArea()) {
			// Only reset if we were previously in LAN
			if (hasInitializedLan) {
				hasInitializedLan = false;
				initializedLanTime = false;

				// Disable sonic mode when leaving LAN area
				if (g_sonicModeEnabled) {
					g_sonicModeEnabled = false;
					AddBotDebugMessage("SONIC mode disabled (left LAN area)", LOG_IMPORTANT);
				}

				// Reset all enhanced values back to default - use tracking to avoid redundant writes

				// 1. Reset movement speed to default if currently enhanced
				if (g_movementSpeed != 1010.0f) {
					g_movementSpeed = 1010.0f;  // Default game movement speed
					SetMovementSpeed(g_movementSpeed);
					g_usingSonicSpeed = false;
					AddBotDebugMessage("LAN exit: Reset movement speed to default", LOG_IMPORTANT);
				}

				// 2. Reset combat hack values if not manually overridden
				if (g_usingEnhancedCombatValues && !g_manualCombatValueOverride && !ValueTracker::g_usingDefaultCombatValues) {
					if (ApplyCombatHackValues(0, true)) {
						g_usingEnhancedCombatValues = false;
						AddBotDebugMessage("LAN exit: Reset combat values to default", LOG_IMPORTANT);
					}
				}

				// 3. Reset IMUL multiplier if not already at default
				if (ValueTracker::g_currentImulValue != 1000) {
					if (SetImulMultiplierValue(1000)) {
						// Reset the manual toggle state if it was enabled
						if (g_autoAttackNoCDEnabled) {
							g_autoAttackNoCDEnabled = false;
							AddBotDebugMessage("LAN exit: AutoAttack No CD disabled", LOG_IMPORTANT);
						}
						else {
							AddBotDebugMessage("LAN exit: Reset IMUL multiplier to 1000", LOG_IMPORTANT);
						}
					}
				}

				// Existing reset code
				g_isWandering = false;
				g_pathWaypoints.clear();
				g_inWaypointSequence = false;

				// Ensure auto-attack is disabled when leaving LAN
				SetAutoAttack(false, g_combatState.weaponType);

				// Reset combat state but preserve weapon type information
				int savedWeaponType = g_combatState.weaponType;
				bool savedIsPhysical = g_combatState.isPhysicalWeapon;
				g_combatState = CombatState{};
				g_combatState.weaponType = savedWeaponType;
				g_combatState.isPhysicalWeapon = savedIsPhysical;

				// Clear any NPC room tracking
				g_roomNPCMap.clear();
				g_roomOccupancy.clear();

				AddBotDebugMessage("Performed complete state reset on LAN exit", LOG_IMPORTANT);
			}

			// ENSURE we're in IDLE state while outside LAN
			TransitionBotState(BotState::IDLE, "Waiting to enter LAN area");
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;
		}

		// Initialize LAN timing if first entry
		if (!initializedLanTime && !hasInitializedLan) {
			g_enteredLanTime = std::chrono::steady_clock::now();
			g_lastRoomScanTime = g_enteredLanTime;
			initializedLanTime = true;
			hasInitializedLan = true;  // Prevent re-initialization

			// Set initial movement speed and reset player detection timer
			SetMovementSpeed(g_movementSpeed);
			g_lastPlayerDetectedTime = std::chrono::steady_clock::now();
			g_usingSonicSpeed = false;
			AddBotDebugMessage("Entered LAN area - set movement speed to enhanced normal", LOG_IMPORTANT);

			// Enable auto-attack immediately for buffing
			SetAutoAttack(true, g_combatState.weaponType);
			AddBotDebugMessage("Entered LAN area, enabling auto-attack for buffing", LOG_IMPORTANT);

			// Disable movement hunting modes during grace period
			SetMovementHuntingMode(2);  // 2 = disable both hunting modes
			AddBotDebugMessage("Disabled movement hunting modes", LOG_IMPORTANT);

			AddBotDebugMessage("Starting grace period of " + std::to_string(INITIAL_GRACE_PERIOD_SEC) +
				" seconds", LOG_IMPORTANT);
			g_botStatusMessage = "Grace period: " + std::to_string(INITIAL_GRACE_PERIOD_SEC) + "s";
		}

		// Check if we're in grace period and skip the rest of the loop if we are
		if (initializedLanTime) {
			auto timeInLan = std::chrono::duration_cast<std::chrono::seconds>(
				currentTime - g_enteredLanTime).count();

			if (timeInLan < INITIAL_GRACE_PERIOD_SEC) {
				// Update status with countdown
				g_botStatusMessage = "Grace period: " +
					std::to_string(INITIAL_GRACE_PERIOD_SEC - timeInLan) + "s";

				// Log the grace period progress every few seconds
				if (g_verboseLogging && timeInLan % 2 == 0) {
					AddBotDebugMessage("Grace period: " + std::to_string(timeInLan) + "/" +
						std::to_string(INITIAL_GRACE_PERIOD_SEC) + " seconds elapsed", LOG_INFO);
				}

				// Sleep and skip the rest of the loop
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
			else if (g_botState == BotState::IDLE) {
				// Grace period just ended, transition to scanning
				AddBotDebugMessage("GRACE PERIOD ENDED - Beginning bot operations now", LOG_IMPORTANT);
				TransitionBotState(BotState::SCANNING_ROOMS, "Scanning rooms for NPCs");

				// Disable auto-attack after grace period
				SetAutoAttack(false, g_combatState.weaponType);
			}
		}

		// Get player position and update current room
		int playerX = 0, playerY = 0;
		if (GetPlayerPosition(playerX, playerY)) {
			int detectedRoom = detectCurrentRoom();
			if (detectedRoom != 0) {
				if (detectedRoom != g_currentRoom) {
					AddBotDebugMessage("Room transition detected: " + std::to_string(g_currentRoom) +
						" -> " + std::to_string(detectedRoom), LOG_IMPORTANT);

					// Log boundary information in verbose mode
					if (g_verboseLogging) {
						ValidateRoomBoundaries(detectedRoom, playerX, playerY);
					}

					g_currentRoom = detectedRoom;

					// Reset room transition sequence if complete
					if (g_inWaypointSequence) {
						AddBotDebugMessage("Room transition completed during waypoint sequence", LOG_IMPORTANT);
						g_inWaypointSequence = false;
						g_pathWaypoints.clear();  // Clear waypoints to prevent stale navigation
						AddBotDebugMessage("Clearing waypoints due to room transition", LOG_IMPORTANT);
					}

					// CHANGE: Always check for NPCs during any room transition
					// Get entities and explicitly check for NPCs
					std::vector<EntityInfo> entities = GetEntities();
					auto roomNPCs = FindNPCsInRoom(detectedRoom, entities);

					// ALWAYS log the results, even if no NPCs found
					AddBotDebugMessage("Found " + std::to_string(roomNPCs.size()) +
						" NPCs in room " + std::to_string(detectedRoom) +
						(g_isWandering ? " during wandering" : " during navigation"), LOG_IMPORTANT);

					if (!roomNPCs.empty()) {
						// Only interrupt current path if these NPCs are higher priority
						// Check if room is claimed
						g_roomOccupancy = ScanRoomOccupancy();
						bool roomIsClaimed = false;

						if (g_roomOccupancy.find(detectedRoom) != g_roomOccupancy.end()) {
							const auto& occupancy = g_roomOccupancy[detectedRoom];
							if (occupancy.hasPlayer && occupancy.hasNPCs) {
								roomIsClaimed = true;
								AddBotDebugMessage("Room " + std::to_string(detectedRoom) +
									" has NPCs but is claimed by " + std::to_string(occupancy.playersCount) +
									" player(s), continuing to destination", LOG_IMPORTANT);
							}
						}

						// Only engage if room isn't claimed
						if (!roomIsClaimed) {
							// If we're already heading to a room with NPCs, compare distances
							bool shouldInterrupt = true;

							// Only evaluate interruption if not in wandering mode
							if (!g_isWandering && g_botState == BotState::NAVIGATING && g_targetRoom != 0) {
								// Get current room NPC count
								int currentRoomNPCs = roomNPCs.size();

								// Get target room NPC count
								int targetRoomNPCs = 0;
								if (g_roomNPCMap.find(g_targetRoom) != g_roomNPCMap.end()) {
									targetRoomNPCs = g_roomNPCMap[g_targetRoom].npcs.size();
								}

								// Check how many more rooms to destination
								auto pathToCurrentTarget = FindShortestPath(detectedRoom, g_targetRoom);
								int roomsToTarget = pathToCurrentTarget.size() - 1;

								// CRITICAL CHANGE: Always interrupt if current room has equal or more NPCs
								if (currentRoomNPCs >= targetRoomNPCs) {
									shouldInterrupt = true;
									AddBotDebugMessage("Current room has " + std::to_string(currentRoomNPCs) +
										" NPCs vs target's " + std::to_string(targetRoomNPCs) +
										". Interrupting path to engage here.", LOG_IMPORTANT);
								}
								// Also interrupt if target is more than 1 room away AND current room has decent NPCs
								else if (roomsToTarget > 1 && currentRoomNPCs >= 3) {
									shouldInterrupt = true;
									AddBotDebugMessage("Found " + std::to_string(currentRoomNPCs) +
										" NPCs in current room. Target is " + std::to_string(roomsToTarget) +
										" rooms away. Engaging here for efficiency.", LOG_IMPORTANT);
								}
								else {
									shouldInterrupt = false;
									AddBotDebugMessage("Found " + std::to_string(currentRoomNPCs) +
										" NPCs in room " + std::to_string(detectedRoom) +
										" but continuing to target room " + std::to_string(g_targetRoom) +
										" which has " + std::to_string(targetRoomNPCs) +
										" NPCs and is " + std::to_string(roomsToTarget) +
										" room(s) away", LOG_IMPORTANT);
								}
							}

							if (shouldInterrupt) {
								AddBotDebugMessage("Found " + std::to_string(roomNPCs.size()) +
									" NPCs in room " + std::to_string(detectedRoom) +
									"! Interrupting current path to engage", LOG_IMPORTANT);

								// FORCE the waypoint sequence to end
								g_pathWaypoints.clear();
								g_inWaypointSequence = false;
								g_isWandering = false;

								// Stop current navigation and set up for combat
								g_targetRoom = detectedRoom;
								TransitionBotState(BotState::COMBAT, "Engaging NPCs found during room transition");
							}
						}
					}
				}
			}
			else if (g_currentRoom == 0) {
				// Need initial room detection
				AddBotDebugMessage("Could not detect current room, waiting...", LOG_IMPORTANT);
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				continue;
			}
		}
		else {
			AddBotDebugMessage("Failed to get player position", LOG_ERROR);
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			continue;
		}

		// Movement speed management
		bool playerDetected = false;
		{
			std::lock_guard<std::mutex> lock(g_entitiesMutex);

			// Define excluded entity names (won't trigger detection)
			static const std::unordered_set<std::string> excludedNames = {
				"Beep", "AkaAka", "Knuget"
			};

			// Check if any non-self player entities exist in LAN
			for (const auto& entity : g_entities) {
				if (!entity.isLocalPlayer && !entity.isNPC && entity.id > 0 && entity.id < 999) {
					// Skip entities with excluded names
					if (excludedNames.find(entity.name) != excludedNames.end()) {
						continue;
					}

					playerDetected = true;
					break;
				}
			}
		}

		// Update player detection timer and speed
		if (playerDetected) {
			// Reset timer when player detected
			g_lastPlayerDetectedTime = currentTime;

			// Set normal speed if currently using sonic speed
			if (g_usingSonicSpeed) {
				g_movementSpeed = 1010.0f;
				if (SetMovementSpeed(g_movementSpeed)) {
					g_usingSonicSpeed = false;
					AddBotDebugMessage("Player detected - reduced to normal speed", LOG_IMPORTANT);

					// Add this code to switch skill page back when player detected
					if (g_skillSwitchEnabled && g_onPage2) {
						ToggleSkillPage();
					}
				}
			}

			// Reset combat values to normal if using enhanced (unless manually overridden)
			if (g_usingEnhancedCombatValues && !g_manualCombatValueOverride && !ValueTracker::g_usingDefaultCombatValues) {
				if (ApplyCombatHackValues(0, true)) {
					g_usingEnhancedCombatValues = false;
					AddBotDebugMessage("Player detected - combat values reset to normal", LOG_IMPORTANT);
				}
			}

			// Reset IMUL multiplier regardless of manual setting when in LAN area
			if (ValueTracker::g_currentImulValue != 1000) {
				if (SetImulMultiplierValue(1000)) {
					// If manual mode was enabled, turn it off
					if (g_autoAttackNoCDEnabled) {
						g_autoAttackNoCDEnabled = false;
						AddBotDebugMessage("Player detected - AutoAttack No CD disabled (safety override)", LOG_IMPORTANT);
					}
					else {
						AddBotDebugMessage("Player detected - reset IMUL multiplier to 1000", LOG_IMPORTANT);
					}
				}
			}
		}
		else {
			// Check if time threshold reached
			auto timeWithoutPlayers = std::chrono::duration_cast<std::chrono::seconds>(
				currentTime - g_lastPlayerDetectedTime).count();

			if (timeWithoutPlayers >= 50 &&
				g_botState != BotState::COMBAT &&
				!g_combatState.inCombat) {

				// Apply the correct speed value based on sonic mode if not already applied
				if (!g_usingSonicSpeed) {
					float targetSpeed = g_sonicModeEnabled ? 200.0f : 1010.0f;

					// Only change if current speed doesn't match target
					if (g_movementSpeed != targetSpeed) {
						g_movementSpeed = targetSpeed;
						if (SetMovementSpeed(g_movementSpeed)) {
							g_usingSonicSpeed = true;
							AddBotDebugMessage("No players detected - " +
								std::string(g_sonicModeEnabled ? "SONIC" : "normal") +
								" speed activated: " + std::to_string(g_movementSpeed), LOG_IMPORTANT);

							// Add this code to switch skill page when SONIC mode activates
							if (g_skillSwitchEnabled && g_sonicModeEnabled && !g_onPage2) {
								ToggleSkillPage();
							}
						}
					}
				}

				// Apply enhanced combat values if not already using them and not manually overridden
				if (!g_usingEnhancedCombatValues && !g_manualCombatValueOverride) {
					if (ApplyCombatHackValues(g_enhancedCombatValue)) {
						g_usingEnhancedCombatValues = true;
						AddBotDebugMessage("No players detected - enhanced combat values activated: " +
							std::to_string(g_enhancedCombatValue), LOG_IMPORTANT);
					}

					// Set IMUL multiplier to 0 if not already
					if (ValueTracker::g_currentImulValue != 0) {
						if (SetImulMultiplierValue(0)) {
							AddBotDebugMessage("No players detected - set IMUL multiplier to 0", LOG_IMPORTANT);
						}
					}
				}
			}
		}



		// Main bot state machine control flow
		// currentTime is already defined at the top of the loop

		// Waypoint sequence timeout protection
		if (g_inWaypointSequence) {
			auto sequenceTime = std::chrono::duration_cast<std::chrono::seconds>(
				currentTime - g_sequenceStartTime).count();
			if (sequenceTime > SEQUENCE_TIMEOUT_SEC) {
				AddBotDebugMessage("Waypoint sequence timeout - force resetting navigation", LOG_ERROR);
				g_inWaypointSequence = false;
				TransitionBotState(BotState::SCANNING_ROOMS, "Resetting navigation due to timeout");
			}
		}

		// Periodic room scan - only if not in waypoint sequence
		if (!g_inWaypointSequence &&
			std::chrono::duration_cast<std::chrono::seconds>(
				currentTime - g_lastRoomScanTime).count() >= ROOM_SCAN_INTERVAL_SEC &&
			g_botState != BotState::SCANNING_ROOMS) {

			if (g_botState != BotState::COMBAT) {
				AddBotDebugMessage("Scheduled room scan triggered", LOG_IMPORTANT);
				TransitionBotState(BotState::SCANNING_ROOMS, "Scanning rooms for NPCs");
				g_lastRoomScanTime = currentTime;
			}
		}

		switch (g_botState) {
		case BotState::IDLE:
		{
			// Check if grace period has elapsed
			auto timeInLan = std::chrono::duration_cast<std::chrono::seconds>(
				currentTime - g_enteredLanTime).count();

			if (timeInLan < INITIAL_GRACE_PERIOD_SEC) {
				// Update status with countdown
				g_botStatusMessage = "Grace period: " +
					std::to_string(INITIAL_GRACE_PERIOD_SEC - timeInLan) + "s";
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
			}
			else {
				// Grace period over, begin scanning
				TransitionBotState(BotState::SCANNING_ROOMS, "Scanning rooms for NPCs");

				// Disable auto-attack after grace period
				SetAutoAttack(false, g_combatState.weaponType);
				AddBotDebugMessage("Grace period ended, disabling auto-attack and starting room scan", LOG_IMPORTANT);
			}
		}
		break;

		case BotState::SCANNING_ROOMS:
		{
			// Reset waypoint sequence state when entering scan state
			g_inWaypointSequence = false;

			// Check current room for NPCs FIRST before scanning others
			std::vector<EntityInfo> currentRoomEntities = GetEntities();
			auto currentRoomNPCs = FindNPCsInRoom(g_currentRoom, currentRoomEntities);

			if (!currentRoomNPCs.empty()) {
				// Check if room is claimed
				g_roomOccupancy = ScanRoomOccupancy();
				bool roomIsClaimed = false;

				if (g_roomOccupancy.find(g_currentRoom) != g_roomOccupancy.end()) {
					const auto& occupancy = g_roomOccupancy[g_currentRoom];
					if (occupancy.hasPlayer && occupancy.hasNPCs) {
						roomIsClaimed = true;
						AddBotDebugMessage("Current room " + std::to_string(g_currentRoom) +
							" has NPCs but is claimed by " + std::to_string(occupancy.playersCount) +
							" player(s), will scan for other rooms", LOG_IMPORTANT);
					}
				}

				// If NPCs found in current room and not claimed, engage immediately
				if (!roomIsClaimed) {
					AddBotDebugMessage("Found " + std::to_string(currentRoomNPCs.size()) +
						" NPCs in current room, engaging immediately", LOG_IMPORTANT);

					g_targetRoom = g_currentRoom;  // Current room is the target
					TransitionBotState(BotState::COMBAT, "Engaging NPCs in current room " + std::to_string(g_currentRoom));
					break;  // Exit the state machine case
				}
			}

			// Verify current room before scanning
			int actualRoom = detectCurrentRoom();
			if (actualRoom != 0 && actualRoom != g_currentRoom) {
				AddBotDebugMessage("Room detection mismatch before scanning! Updating from " +
					std::to_string(g_currentRoom) + " to " +
					std::to_string(actualRoom), LOG_ERROR);
				g_currentRoom = actualRoom;
			}

			AddBotDebugMessage("Scanning for NPCs in all rooms", LOG_IMPORTANT);

			// Scan room occupancy BEFORE scanning for NPCs
			g_roomOccupancy = ScanRoomOccupancy();

			// Scan all rooms for NPCs
			g_roomNPCMap = ScanRoomsForNPCs();

			if (g_roomNPCMap.empty()) {
				// No rooms have NPCs - trigger wandering
				AddBotDebugMessage("No rooms with NPCs found, wandering to expand search range", LOG_IMPORTANT);

				// Select a wander target
				g_wanderTargetRoom = SelectWanderTarget(g_currentRoom);

				if (g_wanderTargetRoom == 0) {
					AddBotDebugMessage("Failed to select wander target, waiting for next scan", LOG_ERROR);
					TransitionBotState(BotState::IDLE, "Waiting for next scan");
					break;
				}

				// Set up navigation to wander target
				g_isWandering = true;
				g_targetRoom = g_wanderTargetRoom;

				AddBotDebugMessage("Wandering to room " + std::to_string(g_wanderTargetRoom), LOG_IMPORTANT);

				// Calculate path to wander target
				auto wanderPath = FindShortestPath(g_currentRoom, g_wanderTargetRoom);

				if (wanderPath.size() < 2) {
					AddBotDebugMessage("Invalid wander path, waiting for next scan", LOG_ERROR);
					g_isWandering = false;
					TransitionBotState(BotState::IDLE, "Waiting for next scan");
					break;
				}

				// Log the complete wander path for debugging
				std::string pathLog = "Wander path: ";
				for (size_t i = 0; i < wanderPath.size(); i++) {
					pathLog += std::to_string(wanderPath[i]);
					if (i < wanderPath.size() - 1) pathLog += " -> ";
				}
				AddBotDebugMessage(pathLog, LOG_IMPORTANT);

				// Get next room in path
				int nextRoom = wanderPath[1];

				// Get waypoints to next room
				g_pathWaypoints = getPathToRoom(g_currentRoom, nextRoom);

				AddBotDebugMessage("getPathToRoom(" + std::to_string(g_currentRoom) + ", " +
					std::to_string(nextRoom) + ") returned " +
					std::to_string(g_pathWaypoints.size()) + " waypoints", LOG_IMPORTANT);

				if (!g_pathWaypoints.empty()) {
					std::string waypointLog = "Waypoints from getPathToRoom: ";
					for (const auto& wp : g_pathWaypoints) {
						waypointLog += "(" + std::to_string(wp.x) + "," + std::to_string(wp.y) + ") ";
					}
					AddBotDebugMessage(waypointLog, LOG_IMPORTANT);
				}

				// Start wandering navigation
				const Coordinate& waypoint = g_pathWaypoints.front();
				MoveToLocation(waypoint.x, waypoint.y);

				TransitionBotState(BotState::NAVIGATING, "Wandering to room " + std::to_string(g_wanderTargetRoom));

				// Initialize navigation state
				lastMovementTime = currentTime;
				g_waitingAtWaypoint = false;
				g_inWaypointSequence = true;
				g_sequenceStartTime = currentTime;
				break;
			}

			// Calculate paths to rooms with NPCs
			CalculatePathsToNPCRooms(g_currentRoom);

			// Find optimal room to navigate to (automatically skips claimed rooms)
			g_targetRoom = FindOptimalNPCRoom();

			if (g_targetRoom == 0) {
				// All rooms with NPCs are claimed - also trigger wandering
				AddBotDebugMessage("All rooms with NPCs are claimed by other players, wandering to expand search", LOG_IMPORTANT);

				// Select a wander target
				g_wanderTargetRoom = SelectWanderTarget(g_currentRoom);

				if (g_wanderTargetRoom == 0) {
					AddBotDebugMessage("Failed to select wander target, waiting for next scan", LOG_ERROR);
					TransitionBotState(BotState::IDLE, "Waiting for next scan");
					break;
				}

				// Set up navigation to wander target
				g_isWandering = true;
				g_targetRoom = g_wanderTargetRoom;

				AddBotDebugMessage("Wandering to room " + std::to_string(g_wanderTargetRoom), LOG_IMPORTANT);

				// Calculate path to wander target
				auto wanderPath = FindShortestPath(g_currentRoom, g_wanderTargetRoom);

				if (wanderPath.size() < 2) {
					AddBotDebugMessage("Invalid wander path, waiting for next scan", LOG_ERROR);
					g_isWandering = false;
					TransitionBotState(BotState::IDLE, "Waiting for next scan");
					break;
				}

				// Get next room in path
				int nextRoom = wanderPath[1];

				// Get waypoints to next room
				g_pathWaypoints = getPathToRoom(g_currentRoom, nextRoom);

				if (g_pathWaypoints.empty()) {
					AddBotDebugMessage("Failed to get wander waypoints", LOG_ERROR);
					g_isWandering = false;
					TransitionBotState(BotState::IDLE, "Waiting for next scan");
					break;
				}

				// Start wandering navigation
				const Coordinate& waypoint = g_pathWaypoints.front();
				MoveToLocation(waypoint.x, waypoint.y);

				TransitionBotState(BotState::NAVIGATING, "Wandering to room " + std::to_string(g_wanderTargetRoom));

				// Initialize navigation state
				lastMovementTime = currentTime;
				g_waitingAtWaypoint = false;
				g_inWaypointSequence = true;
				g_sequenceStartTime = currentTime;
				break;
			}

			// One final check before navigation
			// Refresh room occupancy data one last time
			// Refresh the room occupancy data right before navigation begins
			// Cancel navigation if the target room has become claimed
			// Remove the claimed room from consideration
			// Immediately rescan for a new target
			g_roomOccupancy = ScanRoomOccupancy();

			// Check if target room is now claimed before committing to navigation
			if (g_targetRoom != 0 && g_roomOccupancy.find(g_targetRoom) != g_roomOccupancy.end()) {
				const auto& occupancy = g_roomOccupancy[g_targetRoom];
				if (occupancy.hasPlayer && occupancy.hasNPCs) {
					AddBotDebugMessage("Target room " + std::to_string(g_targetRoom) +
						" has become claimed, cancelling navigation", LOG_IMPORTANT);
					RemoveRoomFromNPCMap(g_targetRoom);  // Remove this room from consideration
					TransitionBotState(BotState::SCANNING_ROOMS, "Target room claimed, rescanning");
					break;
				}
			}

			// Check if already in target room with verification
			if (g_targetRoom == g_currentRoom || g_targetRoom == detectCurrentRoom()) {
				AddBotDebugMessage("Already in room with NPCs, engaging", LOG_IMPORTANT);
				TransitionBotState(BotState::COMBAT, "Engaging NPCs in room " + std::to_string(g_currentRoom));
				break;
			}

			// Path validation with room synchronization
			if (g_roomNPCMap.find(g_targetRoom) != g_roomNPCMap.end()) {
				// Verify room state synchronization
				actualRoom = detectCurrentRoom();
				if (actualRoom != 0 && actualRoom != g_currentRoom) {
					AddBotDebugMessage("Room state desync detected! Updating from " +
						std::to_string(g_currentRoom) + " to " +
						std::to_string(actualRoom), LOG_ERROR);
					g_currentRoom = actualRoom;

					// Recalculate path based on new room
					CalculatePathsToNPCRooms(g_currentRoom);
				}

				std::vector<int> path = g_roomNPCMap[g_targetRoom].path;

				if (path.size() < 2) {
					AddBotDebugMessage("Invalid path to room " + std::to_string(g_targetRoom), LOG_ERROR);
					TransitionBotState(BotState::IDLE, "Waiting for next scan");
					break;
				}

				// Get next room in path
				int nextRoom = path[1]; // Path[0] is current room

				// Verify next room isn't current room
				if (nextRoom == g_currentRoom || nextRoom == actualRoom) {
					AddBotDebugMessage("Invalid path - next room same as current!", LOG_ERROR);

					// Force a complete recalculation next scan
					g_targetRoom = 0;
					g_lastRoomScanTime = currentTime - std::chrono::seconds(ROOM_SCAN_INTERVAL_SEC - 5);
					TransitionBotState(BotState::IDLE, "Waiting for next scan");
					break;
				}

				// Determine the room after the next room (if available)
				int nextRoomAfterTarget = (path.size() > 2) ? path[2] : 0;

				// Get waypoints to next room
				g_pathWaypoints = getPathToRoom(g_currentRoom, nextRoom, nextRoomAfterTarget);

				if (g_pathWaypoints.empty()) {
					AddBotDebugMessage("Failed to get waypoints to room " + std::to_string(nextRoom), LOG_ERROR);
					TransitionBotState(BotState::IDLE, "Waiting for next scan");
					break;
				}

				AddBotDebugMessage("Navigating to room " + std::to_string(g_targetRoom) +
					" via room " + std::to_string(nextRoom), LOG_IMPORTANT);

				// Log waypoints for debugging
				std::string waypointLog = "Waypoints: ";
				for (const auto& wp : g_pathWaypoints) {
					waypointLog += "(" + std::to_string(wp.x) + "," + std::to_string(wp.y) + ") ";
				}
				AddBotDebugMessage(waypointLog, LOG_INFO);

				// Move to first waypoint
				const Coordinate& waypoint = g_pathWaypoints.front();
				MoveToLocation(waypoint.x, waypoint.y);

				TransitionBotState(BotState::NAVIGATING, "Moving to room " + std::to_string(g_targetRoom));

				// Initialize navigation state
				lastMovementTime = currentTime;
				g_waitingAtWaypoint = false;

				// Initialize waypoint sequence tracking
				g_inWaypointSequence = true;
				g_sequenceStartTime = currentTime;
				AddBotDebugMessage("Starting protected waypoint sequence", LOG_IMPORTANT);
			}
			else {
				AddBotDebugMessage("Target room not found in NPC map", LOG_ERROR);
				TransitionBotState(BotState::IDLE, "Waiting for next scan");
			}
		}
		break;

		case BotState::NAVIGATING:
			// Check if we've reached the target room
			if (g_currentRoom == g_targetRoom || detectCurrentRoom() == g_targetRoom) {
				AddBotDebugMessage("Reached room " + std::to_string(g_currentRoom), LOG_IMPORTANT);
				g_pathWaypoints.clear();  // Clear any remaining waypoints
				g_inWaypointSequence = false;
				if (g_isWandering) {
					// Check if this is actually the final wander destination
					auto wanderPath = FindShortestPath(g_currentRoom, g_targetRoom);
					// If there's still more rooms to go, this isn't the final destination
					if (wanderPath.size() > 1) {
						AddBotDebugMessage("This is not the final wander destination (still need to go to room " +
							std::to_string(g_targetRoom) + "), continuing", LOG_IMPORTANT);

						// Find current position in path
						auto it = std::find(wanderPath.begin(), wanderPath.end(), g_currentRoom);
						if (it != wanderPath.end() && (it + 1) != wanderPath.end()) {
							int nextRoom = *(it + 1);

							// Determine room after next (if available)
							int nextRoomAfterTarget = 0;
							if (it + 2 != wanderPath.end()) {
								nextRoomAfterTarget = *(it + 2);
							}

							// Get waypoints with the "after next" information
							g_pathWaypoints = getPathToRoom(g_currentRoom, nextRoom, nextRoomAfterTarget);

							if (!g_pathWaypoints.empty()) {
								const Coordinate& waypoint = g_pathWaypoints.front();
								MoveToLocation(waypoint.x, waypoint.y);

								// Initialize navigation state
								lastMovementTime = currentTime;
								g_waitingAtWaypoint = false;
								g_inWaypointSequence = true;
								g_sequenceStartTime = currentTime;
							}
						}

						// Continue wandering - don't change states
						break;
					}
					// If we get here, we've reached the actual final destination
					AddBotDebugMessage("Reached final wander destination (room " + std::to_string(g_targetRoom) + ")", LOG_IMPORTANT);
					TransitionBotState(BotState::IDLE, "Wandering complete, waiting for next scan");
					g_isWandering = false;
				}
				else {
					// Normal behavior - engage NPCs
					TransitionBotState(BotState::COMBAT, "Engaging NPCs in room " + std::to_string(g_currentRoom));
				}
				break;
			}

			// Add continuous room scanning during navigation to quickly detect claimed rooms
			if (g_targetRoom != 0 && !g_isWandering) {
				static auto lastClaimCheckTime = std::chrono::steady_clock::now();

				// Check target room claimed status every 500ms during navigation
				if (std::chrono::duration_cast<std::chrono::milliseconds>(
					currentTime - lastClaimCheckTime).count() >= 500) {

					// Specifically check just the target room for better performance
					bool targetRoomClaimed = false;
					std::vector<EntityInfo> entities;
					{
						std::lock_guard<std::mutex> lock(g_entitiesMutex);
						entities = g_entities;
					}

					// Only check boundary of target room instead of all rooms
					if (g_roomBoundaries.find(g_targetRoom) != g_roomBoundaries.end()) {
						const auto& boundary = g_roomBoundaries[g_targetRoom];

						int minX = std::min({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
						int maxX = std::max({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
						int minY = std::min({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });
						int maxY = std::max({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });

						bool hasNPC = false;
						bool hasPlayer = false;

						// Check each entity to see if it's in the target room
						for (const auto& entity : entities) {
							int entityX = static_cast<int>(entity.posX);
							int entityY = static_cast<int>(entity.posY);

							if (entityX >= minX && entityX <= maxX && entityY >= minY && entityY <= maxY) {
								if (entity.isNPC && entity.currentHP > 0) {
									hasNPC = true;
								}
								else if (entity.id > 0 && entity.id < 999 && !entity.isLocalPlayer && entity.currentHP > 0) {
									hasPlayer = true;
								}

								// Early exit if we already found both
								if (hasNPC && hasPlayer) {
									targetRoomClaimed = true;
									break;
								}
							}
						}

						// If destination room itself is claimed, abort navigation
						if (hasNPC && hasPlayer) {
							AddBotDebugMessage("DESTINATION room " + std::to_string(g_targetRoom) +
								" detected as claimed during navigation - ABORTING PATH", LOG_ERROR);

							// Clear waypoints and stop sequence
							g_pathWaypoints.clear();
							g_inWaypointSequence = false;

							// Remove room from NPC map to prevent reselection
							RemoveRoomFromNPCMap(g_targetRoom);

							// Go back to scanning immediately
							TransitionBotState(BotState::SCANNING_ROOMS, "Target room claimed during navigation");
							break; // Exit switch-case to immediately process new state
						}
					}

					lastClaimCheckTime = currentTime;
				}
			}

			// Perform periodic global scanning during wandering to detect NPCs in any room
			if (g_isWandering) {
				static auto lastGlobalScanTime = std::chrono::steady_clock::now();
				static auto lastWanderStartTime = std::chrono::steady_clock::now();
				auto currentTime = std::chrono::steady_clock::now();
				const int MINIMUM_WANDER_TIME_SEC = 8; // Minimum seconds to commit to wandering

				// Initialize wander start time on first run
				static bool firstWanderCheck = true;
				if (firstWanderCheck) {
					lastWanderStartTime = currentTime;
					firstWanderCheck = false;
				}

				// Perform global scan every 1 seconds during wandering
				if (std::chrono::duration_cast<std::chrono::seconds>(
					currentTime - lastGlobalScanTime).count() >= 1) {

					AddBotDebugMessage("Performing global room scan during wandering", LOG_IMPORTANT);

					// CRITICAL FIX: Check room occupancy BEFORE scanning for NPCs
					g_roomOccupancy = ScanRoomOccupancy();

					// Then scan all rooms for NPCs
					auto tempScan = ScanRoomsForNPCs();

					// Filter out claimed rooms from the scan results
					for (auto it = tempScan.begin(); it != tempScan.end();) {
						int roomId = it->first;
						bool roomIsClaimed = false;

						if (g_roomOccupancy.find(roomId) != g_roomOccupancy.end()) {
							const auto& occupancy = g_roomOccupancy[roomId];
							roomIsClaimed = (occupancy.hasPlayer && occupancy.hasNPCs);
						}

						if (roomIsClaimed) {
							it = tempScan.erase(it);
						}
						else {
							++it;
						}
					}

					// Check if we've been wandering long enough to allow interruption
					auto timeSpentWandering = std::chrono::duration_cast<std::chrono::seconds>(
						currentTime - lastWanderStartTime).count();

					// Only interrupt if:
					// 1. We have UNCLAIMED rooms with NPCs after filtering
					// 2. We've been wandering for minimum time to avoid immediate re-interruption
					if (!tempScan.empty() && timeSpentWandering >= MINIMUM_WANDER_TIME_SEC) {
						AddBotDebugMessage("Found UNCLAIMED rooms with NPCs during global scan! Interrupting wandering", LOG_IMPORTANT);
						g_roomNPCMap = tempScan;
						g_isWandering = false;
						g_pathWaypoints.clear();
						g_inWaypointSequence = false;
						firstWanderCheck = true; // Reset for next wandering session
						TransitionBotState(BotState::SCANNING_ROOMS, "Found NPCs during wandering, re-evaluating");
						break; // Exit the switch-case to immediately process the new state
					}

					lastGlobalScanTime = currentTime;
				}
			}

			// Check if we have waypoints to follow
			if (g_pathWaypoints.empty()) {
				// Reset waypoint sequence state when empty
				g_inWaypointSequence = false;

				// Get new waypoints for the next room in the path
				if (g_isWandering) {
					// For wandering, we use the calculated path
					auto wanderPath = FindShortestPath(g_currentRoom, g_targetRoom);

					// Find current position in path
					auto it = std::find(wanderPath.begin(), wanderPath.end(), g_currentRoom);
					if (it != wanderPath.end() && (it + 1) != wanderPath.end()) {
						int nextRoom = *(it + 1);

						// Get waypoints to next room
						g_pathWaypoints = getPathToRoom(g_currentRoom, nextRoom);

						// Add debug logging
						AddBotDebugMessage("getPathToRoom(" + std::to_string(g_currentRoom) + ", " +
							std::to_string(nextRoom) + ") returned " +
							std::to_string(g_pathWaypoints.size()) + " waypoints", LOG_IMPORTANT);

						if (!g_pathWaypoints.empty()) {
							std::string waypointLog = "Waypoints from getPathToRoom: ";
							for (const auto& wp : g_pathWaypoints) {
								waypointLog += "(" + std::to_string(wp.x) + "," + std::to_string(wp.y) + ") ";
							}
							AddBotDebugMessage(waypointLog, LOG_IMPORTANT);

							// Move to first waypoint
							const Coordinate& waypoint = g_pathWaypoints.front();
							MoveToLocation(waypoint.x, waypoint.y);

							AddBotDebugMessage("Continuing wander to room " + std::to_string(nextRoom), LOG_IMPORTANT);

							lastMovementTime = currentTime;
							g_waitingAtWaypoint = false;

							// Start new waypoint sequence
							g_inWaypointSequence = true;
							g_sequenceStartTime = currentTime;
						}
						else {
							AddBotDebugMessage("Failed to get waypoints for wandering", LOG_ERROR);
							TransitionBotState(BotState::IDLE, "Waypoint generation failed");
							g_isWandering = false;
						}
					}
					else {
						AddBotDebugMessage("Reached end of wander path", LOG_IMPORTANT);
						TransitionBotState(BotState::IDLE, "Wandering complete");
						g_isWandering = false;
					}
				}
				else {
					// For normal NPC navigation
					if (g_roomNPCMap.find(g_targetRoom) != g_roomNPCMap.end()) {
						// Verify room state synchronization
						int actualRoom = detectCurrentRoom();
						if (actualRoom != 0 && actualRoom != g_currentRoom) {
							AddBotDebugMessage("Room state desync detected during navigation! Updating from " +
								std::to_string(g_currentRoom) + " to " +
								std::to_string(actualRoom), LOG_ERROR);
							g_currentRoom = actualRoom;

							// Recalculate path based on new room
							CalculatePathsToNPCRooms(g_currentRoom);
						}

						std::vector<int> path = g_roomNPCMap[g_targetRoom].path;

						// Find current position in path
						auto it = std::find(path.begin(), path.end(), g_currentRoom);
						if (it != path.end() && (it + 1) != path.end()) {
							int nextRoom = *(it + 1);

							// Verify next room isn't current room
							if (nextRoom == g_currentRoom || nextRoom == actualRoom) {
								AddBotDebugMessage("Invalid next room - same as current!", LOG_ERROR);
								TransitionBotState(BotState::SCANNING_ROOMS, "Invalid next room detected");
								break;
							}

							// Check if next room is claimed before starting to move there
							bool nextRoomClaimed = false;
							std::vector<EntityInfo> entities;
							{
								std::lock_guard<std::mutex> lock(g_entitiesMutex);
								entities = g_entities;
							}

							if (g_roomBoundaries.find(nextRoom) != g_roomBoundaries.end()) {
								const auto& boundary = g_roomBoundaries[nextRoom];

								int minX = std::min({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
								int maxX = std::max({ boundary.topLeft.x, boundary.topRight.x, boundary.bottomLeft.x, boundary.bottomRight.x });
								int minY = std::min({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });
								int maxY = std::max({ boundary.topLeft.y, boundary.topRight.y, boundary.bottomLeft.y, boundary.bottomRight.y });

								bool hasNPC = false;
								bool hasPlayer = false;

								// Check if the room contains both NPCs and players
								for (const auto& entity : entities) {
									int entityX = static_cast<int>(entity.posX);
									int entityY = static_cast<int>(entity.posY);

									if (entityX >= minX && entityX <= maxX && entityY >= minY && entityY <= maxY) {
										if (entity.isNPC && entity.currentHP > 0) {
											hasNPC = true;
										}
										else if (entity.id > 0 && entity.id < 999 && !entity.isLocalPlayer && entity.currentHP > 0) {
											hasPlayer = true;
										}

										// Early exit if we found both
										if (hasNPC && hasPlayer) {
											nextRoomClaimed = true;
											break;
										}
									}
								}

								// If next room is claimed, check if it's the destination or just transit
								if (nextRoomClaimed) {
									// Check if this is the final destination or just a transit room
									bool isTransitRoom = (nextRoom != g_targetRoom);

									if (isTransitRoom) {
										// Allow transit through claimed room with warning
										AddBotDebugMessage("Transit through claimed room " + std::to_string(nextRoom) +
											" - proceeding with caution", LOG_IMPORTANT);

										// Continue with navigation - don't reroute
									}
									else {
										// The target room itself is claimed - reroute
										AddBotDebugMessage("Target room " + std::to_string(nextRoom) +
											" is claimed, seeking alternative room", LOG_IMPORTANT);

										// Remove this room from consideration
										RemoveRoomFromNPCMap(g_targetRoom);
										TransitionBotState(BotState::SCANNING_ROOMS, "Target room in path is claimed");
										break;
									}
								}
							}

							// Get waypoints to next room
							g_pathWaypoints = getPathToRoom(g_currentRoom, nextRoom);

							// Add debug logging
							AddBotDebugMessage("getPathToRoom(" + std::to_string(g_currentRoom) + ", " +
								std::to_string(nextRoom) + ") returned " +
								std::to_string(g_pathWaypoints.size()) + " waypoints", LOG_IMPORTANT);

							if (!g_pathWaypoints.empty()) {
								std::string waypointLog = "Waypoints from getPathToRoom: ";
								for (const auto& wp : g_pathWaypoints) {
									waypointLog += "(" + std::to_string(wp.x) + "," + std::to_string(wp.y) + ") ";
								}
								AddBotDebugMessage(waypointLog, LOG_IMPORTANT);

								// Move to first waypoint
								const Coordinate& waypoint = g_pathWaypoints.front();
								MoveToLocation(waypoint.x, waypoint.y);

								AddBotDebugMessage("Continuing to room " + std::to_string(nextRoom), LOG_IMPORTANT);

								// Log waypoints for debugging
								std::string waypointLog2 = "Waypoints: ";
								for (const auto& wp : g_pathWaypoints) {
									waypointLog2 += "(" + std::to_string(wp.x) + "," + std::to_string(wp.y) + ") ";
								}
								AddBotDebugMessage(waypointLog2, LOG_INFO);

								lastMovementTime = currentTime;
								g_waitingAtWaypoint = false;

								// Start new waypoint sequence
								g_inWaypointSequence = true;
								g_sequenceStartTime = currentTime;
								AddBotDebugMessage("Starting new protected waypoint sequence", LOG_IMPORTANT);
							}
							else {
								AddBotDebugMessage("Failed to get waypoints to next room", LOG_ERROR);
								TransitionBotState(BotState::SCANNING_ROOMS, "Waypoint generation failed");
							}
						}
						else {
							AddBotDebugMessage("Current room not found in path or at end of path", LOG_ERROR);
							TransitionBotState(BotState::SCANNING_ROOMS, "Path verification failed");
						}
					}
					else {
						AddBotDebugMessage("Target room not in NPC map anymore", LOG_ERROR);
						TransitionBotState(BotState::SCANNING_ROOMS, "Target room no longer valid");
					}
				}
				break;
			}

			// Waypoint waiting logic
			if (g_waitingAtWaypoint) {
				auto waitTime = std::chrono::duration_cast<std::chrono::milliseconds>(
					currentTime - g_waypointWaitStartTime).count();

				if (waitTime >= WAYPOINT_WAIT_TIME_MS) {
					g_waitingAtWaypoint = false;

					// For last waypoint in sequence, verify room transition
					if (g_pathWaypoints.size() == 1) {
						// Get expected next room
						int expectedNextRoom = 0;

						if (g_isWandering) {
							auto wanderPath = FindShortestPath(g_currentRoom, g_targetRoom);
							auto it = std::find(wanderPath.begin(), wanderPath.end(), g_currentRoom);
							expectedNextRoom = (it != wanderPath.end() && (it + 1) != wanderPath.end()) ? *(it + 1) : 0;
						}
						else {
							std::vector<int> path = g_roomNPCMap[g_targetRoom].path;
							auto it = std::find(path.begin(), path.end(), g_currentRoom);
							expectedNextRoom = (it != path.end() && (it + 1) != path.end()) ? *(it + 1) : 0;
						}

						// Verify room transition 
						int newRoom = detectCurrentRoom();
						if (newRoom != g_currentRoom && newRoom != 0) {
							AddBotDebugMessage("Room transition complete: " + std::to_string(g_currentRoom) +
								" -> " + std::to_string(newRoom), LOG_IMPORTANT);
							g_currentRoom = newRoom;
							g_inWaypointSequence = false;  // End sequence on successful transition
							g_pathWaypoints.clear();  // Clear old waypoints immediately
						}
						else if (expectedNextRoom != 0) {
							// Room transition failed - try direct approach
							AddBotDebugMessage("Room transition failed - still in room " +
								std::to_string(g_currentRoom) + " attempting direct transition", LOG_ERROR);

							// Try to move directly to next room center
							if (g_roomBoundaries.find(expectedNextRoom) != g_roomBoundaries.end()) {
								const RoomBoundary& nextBoundary = g_roomBoundaries[expectedNextRoom];
								// Calculate center of next room
								int centerX = (nextBoundary.topLeft.x + nextBoundary.bottomRight.x) / 2;
								int centerY = (nextBoundary.topLeft.y + nextBoundary.bottomRight.y) / 2;

								// Move directly toward center
								AddBotDebugMessage("Forcing move to center of room " +
									std::to_string(expectedNextRoom), LOG_IMPORTANT);
								MoveToLocation(centerX, centerY);
								lastMovementTime = currentTime;

								// Don't empty waypoints - maintain sequence
								break;
							}
						}
					}

					// We've finished waiting, remove waypoint
					if (!g_pathWaypoints.empty()) {
						g_pathWaypoints.pop_front();
					}

					// Move to next waypoint if available
					if (!g_pathWaypoints.empty()) {
						const Coordinate& nextWaypoint = g_pathWaypoints.front();
						MoveToLocation(nextWaypoint.x, nextWaypoint.y);

						AddBotDebugMessage("Moving to next waypoint (" +
							std::to_string(nextWaypoint.x) + ", " +
							std::to_string(nextWaypoint.y) + ")", LOG_IMPORTANT);
						lastMovementTime = currentTime;
					}
					else {
						// All waypoints processed but no room transition
						if (g_inWaypointSequence) {
							AddBotDebugMessage("All waypoints processed but room transition failed", LOG_ERROR);
							// Force recalculation
							g_inWaypointSequence = false;
							TransitionBotState(BotState::SCANNING_ROOMS, "Room transition failed");
						}
					}
				}
				break;
			}

			// Position check for waypoint arrival
			if (!g_waitingAtWaypoint && std::chrono::duration_cast<std::chrono::milliseconds>(
				currentTime - lastPositionCheckTime).count() > POSITION_CHECK_INTERVAL_MS) {

				// Movement refresh logic - send movement command again if not moving for a while
				bool isMoving = IsPlayerMoving();
				if (!isMoving && std::chrono::duration_cast<std::chrono::milliseconds>(
					currentTime - lastMovementTime).count() > MOVEMENT_RETRY_INTERVAL_MS) {

					// Resend movement command
					const Coordinate& waypoint = g_pathWaypoints.front();
					MoveToLocation(waypoint.x, waypoint.y);

					AddBotDebugMessage("Resending movement to waypoint (" +
						std::to_string(waypoint.x) + ", " +
						std::to_string(waypoint.y) + ")", LOG_INFO);

					lastMovementTime = currentTime;
				}

				if (!g_pathWaypoints.empty()) {
					const Coordinate& waypoint = g_pathWaypoints.front();

					// Determine if this is primarily X or Y axis movement
					bool isPrimaryAxisX = true;  // Default to X

					if (g_pathWaypoints.size() >= 2) {
						// Get the entry and exit points
						const Coordinate& nextWaypoint = g_pathWaypoints[1];
						int dx = abs(nextWaypoint.x - waypoint.x);
						int dy = abs(nextWaypoint.y - waypoint.y);

						// If Y change is larger, it's primarily a Y-axis movement
						isPrimaryAxisX = (dx >= dy);
					}

					// Set axis-specific tolerances
					int xTolerance = isPrimaryAxisX ? 1 : 1;
					int yTolerance = isPrimaryAxisX ? 1 : 1;

					// Exact waypoint position check with axis-specific tolerance
					if (abs(playerX - waypoint.x) <= xTolerance &&
						abs(playerY - waypoint.y) <= yTolerance) {
						g_waitingAtWaypoint = true;
						g_waypointWaitStartTime = currentTime;
						AddBotDebugMessage("Reached waypoint using " +
							std::string(isPrimaryAxisX ? "X-axis" : "Y-axis") +
							" tolerance", LOG_IMPORTANT);
					}
				}

				// Update last position for comparison
				lastX = playerX;
				lastY = playerY;
				lastPositionCheckTime = currentTime;
			}
			break;

		case BotState::COMBAT:
		{
			// Only check room claims when first entering combat
			static bool inCombatMode = false;
			static bool roomChecked = false;

			if (!inCombatMode || !roomChecked) {
				// Reset flags
				inCombatMode = true;
				roomChecked = true;

				// Enhanced claim detection with multiple checks
				bool roomIsClaimed = false;
				int claimedDetections = 0;
				int totalChecks = 3;

				AddBotDebugMessage("Performing " + std::to_string(totalChecks) +
					" claim checks for room " + std::to_string(g_currentRoom), LOG_IMPORTANT);

				// Multiple checks for reliability
				for (int i = 0; i < totalChecks; i++) {
					g_roomOccupancy = ScanRoomOccupancy();

					if (g_roomOccupancy.find(g_currentRoom) != g_roomOccupancy.end()) {
						const auto& occupancy = g_roomOccupancy[g_currentRoom];

						if (occupancy.hasPlayer && occupancy.hasNPCs) {
							claimedDetections++;
							AddBotDebugMessage("Check " + std::to_string(i + 1) + ": Room " +
								std::to_string(g_currentRoom) + " appears claimed", LOG_IMPORTANT);
						}
						else {
							AddBotDebugMessage("Check " + std::to_string(i + 1) + ": Room " +
								std::to_string(g_currentRoom) + " appears unclaimed", LOG_IMPORTANT);
						}
					}

					// Small delay between checks
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}

				// Room is claimed if majority of checks detected it
				roomIsClaimed = (claimedDetections > totalChecks / 2);

				if (roomIsClaimed) {
					AddBotDebugMessage("Room " + std::to_string(g_currentRoom) +
						" confirmed claimed (" + std::to_string(claimedDetections) +
						"/" + std::to_string(totalChecks) + " checks), switching to scanning", LOG_IMPORTANT);

					// Disable auto-attack since we're leaving combat
					SetAutoAttack(false, g_combatState.weaponType);

					// Reset combat state flags
					inCombatMode = false;
					roomChecked = false;

					TransitionBotState(BotState::SCANNING_ROOMS, "Avoiding claimed room, scanning for alternatives");
					break;
				}

				AddBotDebugMessage("Room " + std::to_string(g_currentRoom) +
					" confirmed unclaimed, proceeding with combat", LOG_IMPORTANT);
			}

			// Only proceed with combat if we're still in the COMBAT state
			// Make sure auto-attack is enabled
			if (!g_combatState.inCombat) {
				AddBotDebugMessage("Enabling auto-attack", LOG_IMPORTANT);
				SetAutoAttack(true, g_combatState.weaponType);

				// Move to nearest NPC to engage
				if (!MoveToNearestNPC()) {
					// No valid NPCs found, check if room is cleared
					if (AreAllNPCsDefeated()) {
						AddBotDebugMessage("All NPCs in room defeated, cleaning up", LOG_IMPORTANT);
						TransitionBotState(BotState::CLEANUP, "Room cleared, analyzing next move");

						// Reset combat flags when leaving combat
						inCombatMode = false;
						roomChecked = false;
					}
					else {
						// Scan for NPCs again
						AddBotDebugMessage("Can't find NPCs to engage, rescanning room", LOG_IMPORTANT);
						TransitionBotState(BotState::SCANNING_ROOMS, "No NPCs found, rescanning");

						// Reset combat flags when leaving combat
						inCombatMode = false;
						roomChecked = false;
					}
					break;
				}

				// Initialize combat state
				g_combatState.inCombat = true;
				g_combatState.lastHitCounter = GetHitCounter();
				g_combatState.lastHitTime = currentTime;
				g_combatState.hitStagnationCount = 0;

				// Initialize mana tracking for magical weapons
				if (!g_combatState.isPhysicalWeapon) {
					g_combatState.lastMana = GetPlayerMana();
					g_combatState.lastManaChangeTime = currentTime;
				}
			}
			else {
				// Update combat and check if we need to approach
				UpdateCombat();

				// If combat ended, move to cleanup
				if (!g_combatState.inCombat) {
					AddBotDebugMessage("Combat ended, cleaning up", LOG_IMPORTANT);
					TransitionBotState(BotState::CLEANUP, "Combat ended, analyzing next move");

					// Disable auto-attack
					SetAutoAttack(false, g_combatState.weaponType);

					// Reset combat flags when leaving combat
					inCombatMode = false;
					roomChecked = false;
				}
			}
		}
		break;

		case BotState::CLEANUP:
		{
			// End any waypoint sequence if active
			g_inWaypointSequence = false;

			// Remove this room from NPC map
			RemoveRoomFromNPCMap(g_currentRoom);

			// Reset combat state
			g_combatState = CombatState{
				false, 0, 0, std::chrono::steady_clock::now(),
				{}, 0,
				// Preserve weapon type information during reset
				g_combatState.weaponType, 0, std::chrono::steady_clock::now(), g_combatState.isPhysicalWeapon
			};

			// Scan for more rooms with NPCs
			AddBotDebugMessage("Room cleared, scanning for more NPCs", LOG_IMPORTANT);
			TransitionBotState(BotState::SCANNING_ROOMS, "Scanning for more NPCs");
		}
		break;
		}
		// Short sleep for responsiveness
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	// Cleanup when bot stops
	SetAutoAttack(false, g_combatState.weaponType);
	AddBotDebugMessage("LAN Bot thread stopped", LOG_IMPORTANT);
}

// Start the LAN bot
void StartLanBot() {
	if (!g_lanBotRunning) {
		g_lanBotRunning = true;
		g_botState = BotState::IDLE;
		g_botStatusMessage = "Bot is active";
		AddBotDebugMessage("Bot started");

		// Update SONIC mode status log message
		AddBotDebugMessage("SONIC mode is " +
			std::string(g_sonicModeEnabled ? "enabled" : "disabled") +
			" (Speed: " + std::to_string(g_sonicModeEnabled ? 200.0f : 1000.0f) +
			" when no players detected)",
			LOG_IMPORTANT);

		// Initialize rooms if needed
		if (g_rooms.empty()) {
			InitializeRooms();
		}

		// Start bot thread
		std::thread botThread(LanBotThread);
		botThread.detach();
	}
}

// Stop the LAN bot
void StopLanBot() {
	if (g_lanBotRunning) {
		g_lanBotRunning = false;
		g_botStatusMessage = "Bot is disabled";
		AddBotDebugMessage("Bot stopped by user");

		// Disable sonic mode when stopping the bot
		if (g_sonicModeEnabled) {
			g_sonicModeEnabled = false;
			g_usingSonicSpeed = false;
			AddBotDebugMessage("SONIC mode disabled (bot stopped)", LOG_IMPORTANT);
		}

		// Reset combat values if no manual override
		if (!g_manualCombatValueOverride && g_usingEnhancedCombatValues) {
			ApplyCombatHackValues(0, true); // Apply default values (3,2,1)
			g_usingEnhancedCombatValues = false;
			AddBotDebugMessage("Reset combat values to normal: 3,2,1", LOG_IMPORTANT);
		}
	}
}

//=========================================================================================
// Menu Implementation - Window Setup
//=========================================================================================

bool Menu::setup_wnd_class(const char* class_name) noexcept {
	wnd_class.cbSize = sizeof(WNDCLASSEX);
	wnd_class.style = CS_HREDRAW | CS_VREDRAW;
	wnd_class.lpfnWndProc = DefWindowProc;
	wnd_class.cbClsExtra = 0;
	wnd_class.cbWndExtra = 0;
	wnd_class.hInstance = GetModuleHandle(NULL);
	wnd_class.hIcon = 0;
	wnd_class.hCursor = 0;
	wnd_class.hbrBackground = 0;
	wnd_class.lpszMenuName = 0;
	wnd_class.lpszClassName = class_name;
	wnd_class.hIconSm = 0;

	return RegisterClassEx(&wnd_class) != 0;
}

void Menu::destroy_wnd_class() noexcept {
	UnregisterClass(wnd_class.lpszClassName, wnd_class.hInstance);
}

bool Menu::setup_hwnd(const char* name) noexcept {
	hwnd = CreateWindow(
		wnd_class.lpszClassName,
		name,
		WS_OVERLAPPEDWINDOW,
		0, 0, 10, 10,
		0, 0,
		wnd_class.hInstance,
		0
	);

	return (hwnd != NULL);
}

void Menu::destroy_hwnd() noexcept {
	if (hwnd) {
		DestroyWindow(hwnd);
		hwnd = NULL;
	}
}

bool Menu::SetupDX() noexcept {
	const auto handle = GetModuleHandle("d3d9.dll");
	if (!handle) return false;

	using CreateFn = LPDIRECT3D9(__stdcall*)(UINT);
	const auto create = reinterpret_cast<CreateFn>(GetProcAddress(handle, "Direct3DCreate9"));
	if (!create) return false;

	d3d9 = create(D3D_SDK_VERSION);
	if (!d3d9) return false;

	D3DPRESENT_PARAMETERS params = {};
	params.BackBufferWidth = 0;
	params.BackBufferHeight = 0;
	params.BackBufferFormat = D3DFMT_UNKNOWN;
	params.BackBufferCount = 0;
	params.MultiSampleType = D3DMULTISAMPLE_NONE;
	params.MultiSampleQuality = 0;
	params.SwapEffect = D3DSWAPEFFECT_DISCARD;
	params.hDeviceWindow = hwnd;
	params.Windowed = 1;
	params.EnableAutoDepthStencil = 0;
	params.AutoDepthStencilFormat = D3DFMT_UNKNOWN;
	params.Flags = 0;
	params.FullScreen_RefreshRateInHz = 0;
	params.PresentationInterval = 0;

	HRESULT result = d3d9->CreateDevice(
		D3DADAPTER_DEFAULT,
		D3DDEVTYPE_NULLREF,
		hwnd,
		D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT,
		&params,
		&device
	);

	return (result >= 0);
}

void Menu::DestroyDX() noexcept {
	if (device) {
		device->Release();
		device = NULL;
	}

	if (d3d9) {
		d3d9->Release();
		d3d9 = NULL;
	}
}

void Menu::Core() {
	if (!setup_wnd_class("class1")) {
		throw std::runtime_error("Failed to create window class!");
	}

	if (!setup_hwnd("ImGuiBaseProB1")) {
		throw std::exception("Failed to create window");
	}

	if (!SetupDX()) {
		throw std::runtime_error("Failed to create device");
	}

	destroy_hwnd();
	destroy_wnd_class();
}

//=========================================================================================
// Menu Implementation - Setup and Cleanup
//=========================================================================================

void Menu::SetupMenu(LPDIRECT3DDEVICE9 device) noexcept {
	auto params = D3DDEVICE_CREATION_PARAMETERS{};
	device->GetCreationParameters(&params);

	hwnd = params.hFocusWindow;
	org_wndproc = reinterpret_cast<WNDPROC>(
		SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WNDProc))
		);

	ImGui::CreateContext();
	ImGui::StyleColorsDark();

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX9_Init(device);

	setup = true;

	// Initialize scanner
	InitializeCache();
	StartScanner();

	// Initialize zoom system with default values
	InitializeZoomSystem();
	ApplyAllPatches();

	// Initialize AutoSkillUse hook
	InitializeAutoSkillUseHook();
}

void Menu::Destroy() noexcept {
	// Cleanup entity scanner
	StopScanner();

	// Cleanup LAN bot
	if (g_lanBotRunning) {
		StopLanBot();
	}
	CleanupRooms();

	ImGui_ImplDX9_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(org_wndproc));

	DestroyDX();

	// Cleanup AutoSkillUse hook
	CleanupAutoSkillUseHook();
}

//=========================================================================================
// Menu UI Component Functions
//=========================================================================================

// Opens the bank UI window
bool OpenBank()
{
	if (!IsInFieldMode()) return false;

	__try {
		uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("wyd.exe"));
		uintptr_t sceneAddress = GetCurrentSceneAddress();
		if (!sceneAddress) return false;

		using OpenBankFn = void(__thiscall*)(void*, int);
		OpenBankFn openBankFunc = reinterpret_cast<OpenBankFn>(moduleBase + 0xB7C40);

		// Always use mode 1 as parameter
		openBankFunc(reinterpret_cast<void*>(sceneAddress), 1);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Opens the shop UI window
bool OpenShop()
{
	if (!IsInFieldMode()) return false;

	__try {
		uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("wyd.exe"));
		uintptr_t sceneAddress = GetCurrentSceneAddress();
		if (!sceneAddress) return false;

		// Based on your assembly and source code, SetVisibleShop function is at B76E0
		using SetVisibleShopFn = void(__thiscall*)(void*, int);
		SetVisibleShopFn setVisibleShopFunc = reinterpret_cast<SetVisibleShopFn>(moduleBase + 0xB7AA0);

		// Call with parameter 1 to show the shop
		setVisibleShopFunc(reinterpret_cast<void*>(sceneAddress), 1);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Function to call TMHuman::CheckAffect (CheckBuffs)
//bool CheckPlayerBuffs() {
	//if (!IsInFieldMode()) return false;

	// Get local player address
	//uintptr_t playerAddress = GetLocalPlayerAddress();
	//if (!playerAddress) return false;

//	__try {
	//	uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("wyd.exe"));
	//	if (!moduleBase) return false;

		// Function address from disassembly (wyd.exe+109530)
	//	using CheckAffectFn = void(__thiscall*)(void*);
	//	CheckAffectFn checkAffectFunc = reinterpret_cast<CheckAffectFn>(moduleBase + 0x109530);

		// Call the function with the player character as 'this'
	//	checkAffectFunc(reinterpret_cast<void*>(playerAddress));
	//	return true;
//	}
//	__except (EXCEPTION_EXECUTE_HANDLER) {
	//	return false;
	//}
//}

// Controls air movement/flying mode
bool AirMove(bool enable = true)
{
	if (!IsInFieldMode()) return false;

	__try {
		uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("wyd.exe"));
		uintptr_t sceneAddress = GetCurrentSceneAddress();

		if (!sceneAddress) return false;

		using AirMoveFn = bool(__thiscall*)(void*, bool);
		AirMoveFn airMoveFunc = reinterpret_cast<AirMoveFn>(moduleBase + 0xD52C0);

		return airMoveFunc(reinterpret_cast<void*>(sceneAddress), enable);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void Menu::RenderLanBotTab() {
	// Bot status
	ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.0f, 1.0f), "Status: %s", g_botStatusMessage.c_str());

	ImGui::Spacing();

	// LAN Timer - Corrected calculation (19:50 = remaining time format)
	auto now = std::chrono::system_clock::now();
	std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
	std::tm* localTime = std::localtime(&currentTime);

	int currentMinute = localTime->tm_min;
	int currentSecond = localTime->tm_sec;
	int currentHour = localTime->tm_hour;

	// Convert current time to total seconds since midnight
	int currentTotalSeconds = (currentHour * 3600) + (currentMinute * 60) + currentSecond;

	// Reference: Swedish 15:55:00 had 19:50 remaining, so session started at 15:54:52 
	int referenceSessionStart = (15 * 3600) + (54 * 60) + 52; // 

	// Calculate seconds elapsed since reference session start
	int secondsSinceReference = currentTotalSeconds - referenceSessionStart;

	// Handle day boundary crossing
	if (secondsSinceReference < 0) {
		secondsSinceReference += 24 * 3600;
	}

	// Find position in current 20-minute cycle
	int secondsIntoCurrentSession = secondsSinceReference % 1200; // 1200 = 20 minutes

	// Calculate remaining time in current session
	int secondsRemaining = 1200 - secondsIntoCurrentSession;

	int displayMinutes = secondsRemaining / 60;
	int displaySeconds = secondsRemaining % 60;

	// Color coding based on time remaining
	ImVec4 timerColor;
	if (displayMinutes >= 15) {
		timerColor = ImVec4(0.0f, 0.8f, 0.2f, 1.0f); // Green
	}
	else if (displayMinutes >= 5) {
		timerColor = ImVec4(0.9f, 0.9f, 0.0f, 1.0f); // Yellow
	}
	else {
		timerColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); // Red
	}

	// Display the timer with appropriate styling
	ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "LAN Time:");
	ImGui::SameLine();
	ImGui::TextColored(timerColor, "%d:%02d", displayMinutes, displaySeconds);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Start/Stop and SONIC mode buttons
	if (g_lanBotRunning) {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.4f, 0.4f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9f, 0.5f, 0.5f, 1.0f));

		if (ImGui::Button("Stop Bot", ImVec2(ImGui::GetContentRegionAvail().x * 0.48f, 0))) {
			StopLanBot();
		}

		ImGui::PopStyleColor(3);
	}
	else {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.8f, 0.4f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.9f, 0.5f, 1.0f));

		if (ImGui::Button("Start Bot", ImVec2(ImGui::GetContentRegionAvail().x * 0.48f, 0))) {
			StartLanBot();
		}

		ImGui::PopStyleColor(3);
	}

	ImGui::SameLine();

	// SONIC mode toggle button - WITH TIMER
	// Check if we're allowed to use SONIC mode
	bool canEnableSonic = IsInLanArea() && g_lanBotRunning;

	// Choose button color based on enable status and availability
	ImVec4 sonicColor;
	if (!canEnableSonic) {
		// Gray out the button when unavailable
		sonicColor = ImVec4(0.4f, 0.4f, 0.4f, 0.7f);
	}
	else {
		// Use the standard colors when available
		sonicColor = g_sonicModeEnabled ?
			ImVec4(0.0f, 0.7f, 0.9f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
	}

	// Create the button text with appropriate message
	std::string buttonText = "SONIC Mode";
	if (!canEnableSonic) {
		if (!IsInLanArea()) {
			buttonText = "SONIC Mode (Requires LAN Area)";
		}
		else if (!g_lanBotRunning) {
			buttonText = "SONIC Mode (Requires Bot Enabled)";
		}
	}
	else if (g_sonicModeEnabled) {
		auto currentTime = std::chrono::steady_clock::now();
		int timeWithoutPlayers = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>( // FIXED: Added explicit cast
			currentTime - g_lastPlayerDetectedTime).count());

		if (g_usingSonicSpeed) {
			buttonText = "SONIC Mode (Active)";
		}
		else {
			int timeRemaining = 50 - timeWithoutPlayers;
			if (timeRemaining > 0) {
				buttonText = "SONIC Mode (" + std::to_string(timeRemaining) + "s)";
			}
			else {
				buttonText = "SONIC Mode (Activating...)";
			}
		}
	}

	ImGui::PushStyleColor(ImGuiCol_Button, sonicColor);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
		ImVec4(sonicColor.x + 0.1f, sonicColor.y + 0.1f, sonicColor.z + 0.1f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,
		ImVec4(sonicColor.x + 0.2f, sonicColor.y + 0.2f, sonicColor.z + 0.2f, 1.0f));

	if (ImGui::Button(buttonText.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
		// Check if Shift is being held and SONIC mode is already enabled but waiting on timer
		bool shiftHeld = ImGui::GetIO().KeyShift;
		bool isWaitingForTimer = g_sonicModeEnabled && !g_usingSonicSpeed &&
			(static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::steady_clock::now() - g_lastPlayerDetectedTime).count()) < 50);

		if (shiftHeld && isWaitingForTimer) {
			// Reduce timer by 10 seconds by adjusting g_lastPlayerDetectedTime
			g_lastPlayerDetectedTime -= std::chrono::seconds(10);

			// Calculate time remaining
			int timeWithoutPlayers = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>( // Added an explicit cast
				std::chrono::steady_clock::now() - g_lastPlayerDetectedTime).count());
			int timeRemaining = 50 - timeWithoutPlayers;

			// Ensure we don't go negative
			if (timeRemaining < 0) timeRemaining = 0;

			AddBotDebugMessage("SONIC activation timer reduced! " +
				std::to_string(timeRemaining) + " seconds remaining", LOG_IMPORTANT);
		}
		else if (canEnableSonic) {
			// Regular toggle logic (unchanged)
			g_sonicModeEnabled = !g_sonicModeEnabled;

			// If currently using sonic speed, update immediately
			if (g_usingSonicSpeed) {
				g_movementSpeed = g_sonicModeEnabled ? 200.0f : 1010.0f;
				SetMovementSpeed(g_movementSpeed);

				AddBotDebugMessage("SONIC mode " +
					std::string(g_sonicModeEnabled ? "activated" : "deactivated") +
					" - Speed set to " + std::to_string(g_movementSpeed), LOG_IMPORTANT);
			}
			else {
				AddBotDebugMessage("SONIC mode " +
					std::string(g_sonicModeEnabled ? "activated" : "deactivated") +
					" - Will apply when no players detected", LOG_IMPORTANT);
			}
		}
		else {
			// Error messages (unchanged)
			if (!IsInLanArea()) {
				AddBotDebugMessage("Cannot enable SONIC mode outside LAN area", LOG_ERROR);
			}
			else if (!g_lanBotRunning) {
				AddBotDebugMessage("Cannot enable SONIC mode while bot is inactive", LOG_ERROR);
			}
		}
	}

	ImGui::PopStyleColor(3);

	ImGui::Spacing();

	// Skill Page Switching button
	ImVec4 skillSwitchColor = g_skillSwitchEnabled ?
		ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

	ImGui::PushStyleColor(ImGuiCol_Button, skillSwitchColor);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
		ImVec4(skillSwitchColor.x + 0.1f, skillSwitchColor.y + 0.1f, skillSwitchColor.z + 0.1f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,
		ImVec4(skillSwitchColor.x + 0.2f, skillSwitchColor.y + 0.2f, skillSwitchColor.z + 0.2f, 1.0f));

	if (ImGui::Button("Auto Skill Page Switch", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
		g_skillSwitchEnabled = !g_skillSwitchEnabled;

		AddBotDebugMessage("Auto Skill Page Switch " +
			std::string(g_skillSwitchEnabled ? "enabled" : "disabled"), LOG_IMPORTANT);

		// If disabling while on page 2, switch back to page 1
		if (!g_skillSwitchEnabled && g_onPage2) {
			ToggleSkillPage();
		}
	}

	ImGui::PopStyleColor(3);

	if (g_skillSwitchEnabled) {
		// Synchronize state before displaying
		SynchronizeSkillPageState();

		int actualPage = GetCurrentSkillPage();
		if (actualPage != -1) {
			ImGui::TextColored(ImVec4(0.7f, 0.7f, 1.0f, 1.0f),
				"Current: Page %d %s", actualPage + 1,
				(actualPage == 0) ? "(Normal)" : "(No Players)");
		}
		else {
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
				"Current: Unable to detect skill page");
		}
	}

	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) {
		ImGui::BeginTooltip();
		ImGui::Text("Automatically switches skill pages when SONIC mode activates");
		ImGui::Text("Page 1: Used when players are present (normal mode)");
		ImGui::Text("Page 2: Used when no players detected (SONIC mode)");
		ImGui::EndTooltip();
	}

	ImGui::Spacing();

	// Copy Logs button
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.7f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.6f, 0.8f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.7f, 0.9f, 1.0f));

	if (ImGui::Button("Copy Logs", ImVec2(-1, 0))) {
		// Build a string with all debug messages
		std::string allLogs;
		int messageCount = 0;

		{
			std::lock_guard<std::mutex> lock(g_botDebugMutex);

			// Simply collect all messages with their timestamps
			for (const auto& msgPair : g_botDebugMessages) {
				allLogs += msgPair.first + "\n";
				messageCount++;

				// Limit to a reasonable number to avoid huge clipboard data
				if (messageCount >= 200) break;
			}
		}

		// Make sure we actually have content to copy
		if (!allLogs.empty()) {
			// Copy to clipboard using the ImGui function
			ImGui::SetClipboardText(allLogs.c_str());
			AddBotDebugMessage("Copied " + std::to_string(messageCount) + " log entries to clipboard", LOG_IMPORTANT);
		}
		else {
			AddBotDebugMessage("No log entries to copy", LOG_ERROR);
		}
	}

	ImGui::PopStyleColor(3);

	ImGui::Spacing();

	// Verbose logging toggle
	ImGui::Checkbox("Verbose Logging", &g_verboseLogging);
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) {
		ImGui::BeginTooltip();
		ImGui::Text("Show detailed movement and position messages");
		ImGui::EndTooltip();
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Current room info
	ImGui::Text("Current Room: %d", g_currentRoom);
	ImGui::Text("Target Room: %d", g_targetRoom);
	ImGui::Text("Waypoints Remaining: %zu", g_pathWaypoints.size());

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Debug messages
	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.15f, 1.0f));

	float debugHeight = ImGui::GetContentRegionAvail().y;
	ImGui::BeginChild("DebugMessages", ImVec2(0, debugHeight), true);

	{
		std::lock_guard<std::mutex> lock(g_botDebugMutex);
		for (const auto& msgPair : g_botDebugMessages) {
			ImGui::TextColored(msgPair.second, "%s", msgPair.first.c_str());
		}
	}

	ImGui::EndChild();

	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
}

void Menu::RenderMiscTab() {
	// Zoom hack controls
	ImGui::Text("Zoom Control");
	ImGui::Separator();

	static float currentZoom = ZoomSystem::g_currentZoom;
	static float maxZoomLimit = ZoomSystem::g_maxZoomLimit;
	bool zoomChanged = false;

	// Current zoom slider
	ImGui::Text("Current Zoom:");
	if (ImGui::SliderFloat("##CurrentZoom", &currentZoom, 5.0f, maxZoomLimit, "%.1f")) {
		zoomChanged = true;
	}

	// Max zoom limit slider
	ImGui::Text("Zoom Limit:");
	if (ImGui::SliderFloat("##ZoomLimit", &maxZoomLimit, 30.0f, 250.0f, "%.1f")) {
		zoomChanged = true;
	}

	// Apply zoom changes
	if (zoomChanged) {
		ZoomSystem::g_currentZoom = currentZoom;
		ZoomSystem::g_maxZoomLimit = maxZoomLimit;
		SetCameraZoom(maxZoomLimit, currentZoom);
	}

	// Toggle for zoom enabled
	bool zoomEnabled = ZoomSystem::g_zoomEnabled;
	if (ImGui::Checkbox("Enable Zoom Hack", &zoomEnabled)) {
		ZoomSystem::g_zoomEnabled = zoomEnabled;
		if (zoomEnabled) {
			SetCameraZoom(maxZoomLimit, currentZoom);
		}
	}

	ImGui::Spacing();
	ImGui::Separator();

	// Combat Enhancement Options
	ImGui::Text("Combat Enhancements");
	ImGui::Separator();

	// AutoAttack No CD button with color feedback
	ImVec4 noCDColor = g_autoAttackNoCDEnabled ?
		ImVec4(0.2f, 0.7f, 0.3f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

	ImGui::PushStyleColor(ImGuiCol_Button, noCDColor);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
		ImVec4(noCDColor.x + 0.1f, noCDColor.y + 0.1f, noCDColor.z + 0.1f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,
		ImVec4(noCDColor.x + 0.2f, noCDColor.y + 0.2f, noCDColor.z + 0.2f, 1.0f));

	if (ImGui::Button("Toggle AutoAttack No CD", ImVec2(-1, 0))) {
		// Toggle the state
		g_autoAttackNoCDEnabled = !g_autoAttackNoCDEnabled;

		// Set the appropriate value
		int targetValue = g_autoAttackNoCDEnabled ? 0 : 1000;

		// Apply the change if necessary
		if (ValueTracker::g_currentImulValue != targetValue) {
			if (SetImulMultiplierValue(targetValue)) {
				AddBotDebugMessage("AutoAttack No CD " +
					std::string(g_autoAttackNoCDEnabled ? "enabled (cooldown removed)" : "disabled (normal cooldown)"),
					LOG_IMPORTANT);
			}
			else {
				// Toggle back if it failed
				g_autoAttackNoCDEnabled = !g_autoAttackNoCDEnabled;
				AddBotDebugMessage("Failed to change AutoAttack cooldown setting", LOG_ERROR);
			}
		}
	}

	ImGui::PopStyleColor(3);

	// Display current status
	ImGui::Text("AutoAttack Cooldown: %s",
		g_autoAttackNoCDEnabled ? "Disabled (No Delay)" : "Enabled (Normal)");
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) {
		ImGui::BeginTooltip();
		ImGui::Text("Removes the cooldown between auto-attacks");
		ImGui::Text("This affects both physical and magical attacks");
		ImGui::Text("Note: Will be automatically disabled when players are detected");
		ImGui::Text("      or when leaving the LAN area (safety feature)");
		ImGui::EndTooltip();
	}

	// AoE Hack button (moved to Combat Enhancements section)
	ImGui::Spacing();

	// Check if players are nearby in LAN
	bool inLan = IsInLanArea();
	bool playersNearby = false;

	if (inLan && g_lanBotRunning) {
		auto currentTime = std::chrono::steady_clock::now();
		auto timeWithoutPlayers = std::chrono::duration_cast<std::chrono::seconds>(
			currentTime - g_lastPlayerDetectedTime).count();
		playersNearby = (timeWithoutPlayers < 50);

		// If players are nearby in LAN, force disable enhanced values
		if (playersNearby && (g_usingEnhancedCombatValues || g_manualCombatValueOverride)) {
			g_usingEnhancedCombatValues = false;
			g_manualCombatValueOverride = false;

			// Apply differentiated default values (3,2,1)
			ApplyCombatHackValues(0, true); // Value is ignored when isDefault=true
		}
	}

	ImVec4 combatValueColor = (g_manualCombatValueOverride || g_usingEnhancedCombatValues) ?
		ImVec4(0.9f, 0.5f, 0.1f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

	ImGui::PushStyleColor(ImGuiCol_Button, combatValueColor);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
		ImVec4(combatValueColor.x + 0.1f, combatValueColor.y + 0.1f, combatValueColor.z + 0.1f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,
		ImVec4(combatValueColor.x + 0.2f, combatValueColor.y + 0.2f, combatValueColor.z + 0.2f, 1.0f));

	if (ImGui::Button("Toggle AoE Hack", ImVec2(-1, 0))) {
		g_manualCombatValueOverride = !g_manualCombatValueOverride;

		if (g_manualCombatValueOverride) {
			if (ApplyCombatHackValues(g_enhancedCombatValue)) {
				g_usingEnhancedCombatValues = true;
				AddBotDebugMessage("Enhanced AoE range: " +
					std::to_string(g_enhancedCombatValue), LOG_IMPORTANT);
			}
		}
		else {
			// Using isDefault=true applies the differentiated values (3,2,1)
			if (ApplyCombatHackValues(0, true)) { // Value is ignored when isDefault=true
				g_usingEnhancedCombatValues = false;
				AddBotDebugMessage("Normal AoE range", LOG_IMPORTANT);
			}
		}
	}

	ImGui::PopStyleColor(3);
	ImGui::Text("AoE Range: %s", (g_manualCombatValueOverride || g_usingEnhancedCombatValues) ?
		"Enhanced" : "Normal");

	ImGui::Spacing();
	ImGui::Separator();

	// Game functions
	ImGui::Text("Game Functions");
	ImGui::Separator();

	// Button to open bank
	if (ImGui::Button("Open Bank", ImVec2(-1, 0))) {
		if (OpenBank()) {
			AddBotDebugMessage("Bank opened", LOG_IMPORTANT);
		}
		else {
			AddBotDebugMessage("Failed to open bank", LOG_ERROR);
		}
	}

	// Button to open shop
	ImGui::Spacing();
	if (ImGui::Button("Open Latest Shop", ImVec2(-1, 0))) {
		if (OpenShop()) {
			AddBotDebugMessage("Shop opened", LOG_IMPORTANT);
		}
		else {
			AddBotDebugMessage("Failed to open shop", LOG_ERROR);
		}
	}

	ImGui::Spacing();

	// Button for Air Move toggle
	static bool airMoveEnabled = false;
	ImVec4 airMoveColor = airMoveEnabled ?
		ImVec4(0.2f, 0.7f, 0.9f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

	ImGui::PushStyleColor(ImGuiCol_Button, airMoveColor);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
		ImVec4(airMoveColor.x + 0.1f, airMoveColor.y + 0.1f, airMoveColor.z + 0.1f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,
		ImVec4(airMoveColor.x + 0.2f, airMoveColor.y + 0.2f, airMoveColor.z + 0.2f, 1.0f));

	if (ImGui::Button("Toggle Air Move", ImVec2(-1, 0))) {
		airMoveEnabled = !airMoveEnabled;
		if (AirMove(airMoveEnabled)) {
			AddBotDebugMessage("Air Move " + std::string(airMoveEnabled ? "enabled" : "disabled"), LOG_IMPORTANT);
		}
		else {
			AddBotDebugMessage("Failed to toggle Air Move", LOG_ERROR);
		}
	}
}

void Menu::RenderTelemetryDisplay() noexcept {
	// Enhanced styling for professional appearance
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 6));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

	// Enhanced color scheme
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.07f, 0.11f, 0.92f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.97f, 0.99f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.16f, 0.24f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.25f, 0.38f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.35f, 0.52f, 1.0f));

	// Window sizing - Dynamic width based on content requirements
	ImGuiIO& io = ImGui::GetIO();
	float telemetryWidth = IsInLanArea() ? 650.0f : 425.0f;  // Increased LAN width for SONIC timer text
	float mainMenuWidth = 300.0f;
	float spacing = 15.0f;
	float rightMargin = 5.0f;
	float topMargin = 5.0f;

	ImVec2 telemetryPos(
		io.DisplaySize.x - mainMenuWidth - telemetryWidth - spacing - rightMargin,
		topMargin
	);

	ImGui::SetNextWindowPos(telemetryPos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(telemetryWidth, 0), ImGuiCond_Always);

	ImGui::Begin("##Telemetry", nullptr,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus);

	// LAN-specific data displayed first when in LAN area
	if (IsInLanArea()) {
		ImGui::AlignTextToFramePadding();

		// Player count
		int playerCount = 0;
		{
			std::lock_guard<std::mutex> lock(g_entitiesMutex);
			static const std::unordered_set<std::string> excludedNames = {
				"Beep", "AkaAka", "Knuget"
			};

			for (const auto& entity : g_entities) {
				if (!entity.isLocalPlayer && !entity.isNPC &&
					entity.id > 0 && entity.id < 999) {
					if (excludedNames.find(entity.name) == excludedNames.end()) {
						playerCount++;
					}
				}
			}
		}

		ImVec4 playerColor = (playerCount > 0) ?
			ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImVec4(0.5f, 0.8f, 0.5f, 1.0f);
		ImGui::TextColored(playerColor, "Players: %d", playerCount);

		// Separator after LAN data, before SONIC button
		ImGui::SameLine();
		ImGui::Text("|");
		ImGui::SameLine();

		// SONIC mode button - positioned after LAN data, before Bot button
		bool canEnableSonic = g_lanBotRunning;
		ImVec4 sonicColor;
		std::string sonicButtonText = "SONIC";

		if (!canEnableSonic) {
			sonicColor = ImVec4(0.4f, 0.4f, 0.4f, 0.7f);
		}
		else {
			sonicColor = g_sonicModeEnabled ?
				ImVec4(0.0f, 0.7f, 0.9f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

			// Dynamic text based on SONIC state
			if (g_sonicModeEnabled) {
				auto currentTime = std::chrono::steady_clock::now();
				int timeWithoutPlayers = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
					currentTime - g_lastPlayerDetectedTime).count());

				if (g_usingSonicSpeed) {
					sonicButtonText = "SONIC (Active)";
				}
				else {
					int timeRemaining = 50 - timeWithoutPlayers;
					if (timeRemaining > 0) {
						sonicButtonText = "SONIC (" + std::to_string(timeRemaining) + "s)";
					}
					else {
						sonicButtonText = "SONIC (Activating...)";
					}
				}
			}
		}

		ImGui::PushStyleColor(ImGuiCol_Button, sonicColor);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
			ImVec4(sonicColor.x + 0.1f, sonicColor.y + 0.1f, sonicColor.z + 0.1f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,
			ImVec4(sonicColor.x + 0.2f, sonicColor.y + 0.2f, sonicColor.z + 0.2f, 1.0f));

		if (ImGui::Button(sonicButtonText.c_str(), ImVec2(120, 0))) {
			// Shift+click timer reduction logic
			bool shiftHeld = ImGui::GetIO().KeyShift;
			bool isWaitingForTimer = g_sonicModeEnabled && !g_usingSonicSpeed &&
				(static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::steady_clock::now() - g_lastPlayerDetectedTime).count()) < 50);

			if (shiftHeld && isWaitingForTimer) {
				g_lastPlayerDetectedTime -= std::chrono::seconds(10);

				int timeWithoutPlayers = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::steady_clock::now() - g_lastPlayerDetectedTime).count());
				int timeRemaining = 50 - timeWithoutPlayers;
				if (timeRemaining < 0) timeRemaining = 0;

				AddBotDebugMessage("SONIC activation timer reduced! " +
					std::to_string(timeRemaining) + " seconds remaining", LOG_IMPORTANT);
			}
			else if (canEnableSonic) {
				g_sonicModeEnabled = !g_sonicModeEnabled;

				if (g_usingSonicSpeed) {
					g_movementSpeed = g_sonicModeEnabled ? 200.0f : 1010.0f;
					SetMovementSpeed(g_movementSpeed);

					AddBotDebugMessage("SONIC mode " +
						std::string(g_sonicModeEnabled ? "activated" : "deactivated") +
						" - Speed set to " + std::to_string(g_movementSpeed), LOG_IMPORTANT);
				}
				else {
					AddBotDebugMessage("SONIC mode " +
						std::string(g_sonicModeEnabled ? "activated" : "deactivated") +
						" - Will apply when no players detected", LOG_IMPORTANT);
				}
			}
			else {
				AddBotDebugMessage("SONIC mode requires active bot", LOG_ERROR);
			}
		}

		ImGui::PopStyleColor(3);

		// Separator after SONIC button, before Bot button
		ImGui::SameLine();
		ImGui::Text("|");
		ImGui::SameLine();
	}

	// Unified bot status button with toggle functionality
	ImGui::AlignTextToFramePadding();
	bool botActive = g_lanBotRunning;

	// Create button text based on current state
	const char* buttonText = botActive ? "Bot: Active" : "Bot: Disabled";

	// Apply color coding directly to button
	if (botActive) {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));       // Green base
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f)); // Green hover
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.8f, 0.4f, 1.0f));  // Green active
	}
	else {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));       // Red base
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f)); // Red hover
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.8f, 0.4f, 0.4f, 1.0f));  // Red active
	}

	// Single button with toggle functionality
	if (ImGui::Button(buttonText, ImVec2(110, 0))) {  // Width calculated for "Bot: Disabled"
		if (botActive) {
			StopLanBot();
		}
		else {
			StartLanBot();
		}
	}

	ImGui::PopStyleColor(3);

	// Separator after bot button
	ImGui::SameLine();
	ImGui::Text("|");

	// LAN Timer
	ImGui::SameLine();
	// LAN Timer - Corrected calculation (19:50 = remaining time format)
	auto now = std::chrono::system_clock::now();
	std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
	std::tm* localTime = std::localtime(&currentTime);

	int currentMinute = localTime->tm_min;
	int currentSecond = localTime->tm_sec;
	int currentHour = localTime->tm_hour;

	// Convert current time to total seconds since midnight
	int currentTotalSeconds = (currentHour * 3600) + (currentMinute * 60) + currentSecond;

	// Adjust reference session start time by -1 second  
	int referenceSessionStart = (15 * 3600) + (54 * 60) + 52; //

	// Calculate seconds elapsed since reference session start
	int secondsSinceReference = currentTotalSeconds - referenceSessionStart;

	// Handle day boundary crossing
	if (secondsSinceReference < 0) {
		secondsSinceReference += 24 * 3600;
	}

	// Find position in current 20-minute cycle
	int secondsIntoCurrentSession = secondsSinceReference % 1200; // 1200 = 20 minutes

	// Calculate remaining time in current session
	int secondsRemaining = 1200 - secondsIntoCurrentSession;

	int displayMinutes = secondsRemaining / 60;
	int displaySeconds = secondsRemaining % 60;

	ImVec4 timerColor;
	if (displayMinutes >= 15) {
		timerColor = ImVec4(0.0f, 0.8f, 0.2f, 1.0f);
	}
	else if (displayMinutes >= 5) {
		timerColor = ImVec4(0.9f, 0.9f, 0.0f, 1.0f);
	}
	else {
		timerColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
	}

	ImGui::Text("LAN:");
	ImGui::SameLine();
	ImGui::TextColored(timerColor, "%d:%02d", displayMinutes, displaySeconds);

	// Separator after LAN timer
	ImGui::SameLine();
	ImGui::Text("|");

	// Coordinates button with user's preferred format
	ImGui::SameLine();
	int x = 0, y = 0;
	bool hasPosition = GetPlayerPosition(x, y);

	if (hasPosition) {
		char coordText[32];
		snprintf(coordText, sizeof(coordText), "Pos: %d %d", x, y);  // User's format

		if (ImGui::Button(coordText, ImVec2(115, 0))) {
			char clipboardText[32];
			snprintf(clipboardText, sizeof(clipboardText), "(%d, %d)", x, y);  // Copy format unchanged
			ImGui::SetClipboardText(clipboardText);

			AddBotDebugMessage("Coordinates copied: " + std::string(clipboardText), LOG_IMPORTANT);
		}

		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Click to copy coordinates");
		}
	}
	else {
		ImGui::Button("Pos: N/A", ImVec2(115, 0));
	}

	// Bank button with proper separator
	ImGui::SameLine();
	ImGui::Text("|");
	ImGui::SameLine();
	if (ImGui::Button("Bank", ImVec2(50, 0))) {
		if (OpenBank()) {
			AddBotDebugMessage("Bank opened from telemetry", LOG_IMPORTANT);
		}
		else {
			AddBotDebugMessage("Failed to open bank", LOG_ERROR);
		}
	}

	ImGui::End();

	ImGui::PopStyleColor(5);
	ImGui::PopStyleVar(6);
}

//=========================================================================================
// Main Menu Rendering
//=========================================================================================

void Menu::Render() noexcept {
	// Initialize ImGui frame
	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	// Reset collapsed state
	is_window_collapsed = false;

	if (show_overlay) {
		// Get display size
		ImGuiIO& io = ImGui::GetIO();

		// Advanced styling variables optimized for compact space
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));        // Refined padding balance
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 7));           // Optimized vertical spacing
		ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(6, 4));       // Inner element spacing
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 6));          // Enhanced frame padding
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);                  // Increased rounding for premium feel
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);                // Pronounced window rounding
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);                  // Child window consistency
		ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 8.0f);              // Scrollbar refinement
		ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 8.0f);                   // Grab handle consistency
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);               // Clean borderless design
		ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);                // Subtle child borders
		ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 12.0f);                 // Compact scrollbar

		// Sophisticated color system with advanced gradients and depth
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.08f, 0.12f, 0.97f));          // Deeper primary background
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.06f, 0.10f, 0.8f));            // Layered child backgrounds
		ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.08f, 0.10f, 0.16f, 0.96f));           // Enhanced popup depth

		// Advanced header system with gradient feel
		ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f, 0.20f, 0.30f, 0.9f));             // Refined header base
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.20f, 0.28f, 0.42f, 0.95f));     // Smooth hover transition
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.25f, 0.35f, 0.52f, 1.0f));       // Strong active state

		// Premium button system with depth
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.16f, 0.24f, 1.0f));             // Rich button base
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.25f, 0.38f, 1.0f));      // Elegant hover state
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.35f, 0.52f, 1.0f));       // Premium active state

		// Refined scrollbar system
		ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.02f, 0.04f, 0.08f, 0.8f));        // Subtle background
		ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.20f, 0.25f, 0.35f, 0.9f));      // Refined grab
		ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.30f, 0.38f, 0.50f, 1.0f)); // Smooth hover
		ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.40f, 0.50f, 0.65f, 1.0f)); // Strong active

		// Sophisticated border and separator system
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.18f, 0.22f, 0.32f, 0.7f));             // Subtle borders
		ImGui::PushStyleColor(ImGuiCol_BorderShadow, ImVec4(0.00f, 0.00f, 0.00f, 0.6f));       // Enhanced shadows
		ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.22f, 0.28f, 0.38f, 0.8f));          // Refined separators
		ImGui::PushStyleColor(ImGuiCol_SeparatorHovered, ImVec4(0.32f, 0.40f, 0.55f, 1.0f));   // Interactive separators
		ImGui::PushStyleColor(ImGuiCol_SeparatorActive, ImVec4(0.42f, 0.52f, 0.70f, 1.0f));    // Active separator state

		// Advanced text system with optimal contrast
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.96f, 0.98f, 1.0f));               // High contrast primary text
		ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.50f, 0.55f, 0.65f, 1.0f));       // Refined disabled text
		ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.25f, 0.35f, 0.52f, 0.6f));     // Selection background

		// Premium interactive elements
		ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.55f, 0.70f, 0.95f, 1.0f));          // Vibrant checkmarks
		ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.40f, 0.55f, 0.80f, 1.0f));         // Refined slider grab
		ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.50f, 0.68f, 0.95f, 1.0f));   // Active slider state

		// Advanced frame backgrounds
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.08f, 0.12f, 0.18f, 0.9f));            // Input backgrounds
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.12f, 0.18f, 0.28f, 1.0f));     // Hovered inputs
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.15f, 0.22f, 0.35f, 1.0f));      // Active inputs

		// Sophisticated title bar
		ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.04f, 0.06f, 0.10f, 1.0f));            // Title background
		ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.06f, 0.08f, 0.14f, 1.0f));      // Active title
		ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, ImVec4(0.02f, 0.04f, 0.08f, 0.8f));   // Collapsed title

		// Set window size constraints
		ImGui::SetNextWindowSizeConstraints(
			ImVec2(300, 0),
			ImVec2(300, 700)
		);

		// Position window in top-right corner
		float windowWidth = 300.0f;
		float rightMargin = 5.0f;
		float topMargin = 5.0f;

		ImVec2 windowPos(io.DisplaySize.x - windowWidth - rightMargin, topMargin);
		ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always);

		// Begin main window
		ImGui::Begin("CELESTIAL 1.0 [LAN [A] EDITION]", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);

		// Check if window is collapsed
		is_window_collapsed = ImGui::IsWindowCollapsed();

		// Only render content if window is not collapsed
		if (!is_window_collapsed) {

			// LAN Normal Tab
			if (ImGui::CollapsingHeader("Lan [A]")) {
				RenderLanBotTab();
			}

			// Add Misc Tab
			if (ImGui::CollapsingHeader("Misc")) {
				RenderMiscTab();
			}

		}

		ImGui::End();
		// cleanup style
		ImGui::PopStyleColor(28);
		ImGui::PopStyleVar(12);

		// Render independent telemetry display
		RenderTelemetryDisplay();

		// Update window active state
		if (!is_window_collapsed && !is_window_inactive) {
			CallWindowProc(org_wndproc, hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
			is_window_inactive = true;
		}
		else if (is_window_collapsed && is_window_inactive) {
			CallWindowProc(org_wndproc, hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
			is_window_inactive = false;
		}
	}
	else if (is_window_inactive) {
		CallWindowProc(org_wndproc, hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
		is_window_inactive = false;
	}

	// Finish ImGui frame
	ImGui::EndFrame();
	ImGui::Render();
	ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

//=========================================================================================
// Window Procedure
//=========================================================================================

LRESULT CALLBACK WNDProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	// Process ImGui input first
	if (Menu::setup) {
		if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam))
			return 0;
	}

	// Only block input if menu is shown AND not collapsed
	if (Menu::setup && Menu::show_overlay && !Menu::is_window_collapsed) {
		ImGuiIO& io = ImGui::GetIO();

		// Block mouse input when menu is expanded
		switch (msg) {
		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
		case WM_XBUTTONDOWN:
		case WM_XBUTTONUP:
		case WM_MOUSEMOVE:
		case WM_MOUSEWHEEL:      
		case WM_MOUSEHWHEEL:     
			// Block mouse input from reaching the game
			return 0;

		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_CHAR:
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
			// Only block keyboard if ImGui specifically wants it
			if (io.WantCaptureKeyboard) {
				return 0;
			}
			break;
		}
	}

	// Pass other messages to original window procedure
	return CallWindowProc(Menu::org_wndproc, hwnd, msg, wparam, lparam);
}