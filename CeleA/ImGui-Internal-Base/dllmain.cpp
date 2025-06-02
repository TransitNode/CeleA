#include <stdexcept>
#include <windows.h>
#include "Core/globals.h"
#include "hooks/hooks.h"

// Module handle for FreeLibraryAndExitThread
HMODULE g_moduleHandle = NULL;

// Main DLL thread
DWORD WINAPI injection_thread(LPVOID module)
{
	HMODULE hModule = static_cast<HMODULE>(module);

	try
	{
		// Initialize core components
		Menu::Core();
		hooks::Setup();
	}
	catch (const std::exception&)
	{
		// Silent exception handling
	}

	// Simple polling loop for hotkey detection
	while (true)
	{
		// Check for F1+F2 combination
		// Using & 0x8000 for current state and & 1 for transition state
		if ((GetAsyncKeyState(VK_F1) & 0x8000) && (GetAsyncKeyState(VK_F2) & 1))
		{
			break; // Exit the loop when hotkey detected
		}

		Sleep(50); // Reduce CPU usage
	}

	// Clean up in proper order
	hooks::Destroy();
	Menu::Destroy();

	// Small delay before unloading to ensure cleanup completes
	Sleep(150);

	// Free the library and exit thread
	FreeLibraryAndExitThread(hModule, 0);
	return 0; // Never reached, but good practice
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
	{
		// Store module handle and disable thread attach/detach notifications
		g_moduleHandle = module;
		DisableThreadLibraryCalls(module);

		// Initialize base address
		Window::base = (uintptr_t)GetModuleHandleW(nullptr);

		// Create main thread
		HANDLE threadHandle = CreateThread(
			NULL,                   // Default security
			0,                      // Default stack size
			injection_thread,       // Thread function
			module,                 // Thread parameter
			0,                      // Run immediately
			NULL                    // Don't need thread ID
		);

		// Close thread handle as we don't need it
		if (threadHandle) {
			CloseHandle(threadHandle);
		}
	}
	break;

	case DLL_PROCESS_DETACH:
		// No additional cleanup needed here as the thread handles it
		break;
	}

	return TRUE;
}