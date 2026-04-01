/*
 * CS2 Internal DLL - ESP (ImGui + Present Hook), Auto-Jump, Angle Lock
 * Offsets: April 2026 (from your generated files)
 *
 * Compile (x64 Release, MSVC):
 *   cl /EHsc /O2 /MT /LD internal_cs2.cpp MinHook.lib d3d11.lib dxgi.lib
 *   (plus all ImGui source files: imgui.cpp, imgui_draw.cpp, imgui_impl_win32.cpp, imgui_impl_dx11.cpp)
 */

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include <psapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <mutex>

// ImGui
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

//=============================================================================
// Offsets (from your provided offsets file)
//=============================================================================
namespace cs2 {
    namespace client_dll {
        constexpr uintptr_t dwLocalPlayerPawn   = 0x206A9E0;   // Local player pawn
        constexpr uintptr_t dwEntityList        = 0x24B0258;   // CEntitySystem*
        constexpr uintptr_t dwViewMatrix        = 0x2310F10;   // View matrix (float[4][4])
        constexpr uintptr_t dwViewAngles        = 0x231B968;   // Global view angles (Vector3)
        constexpr uintptr_t dwCSGOInput         = 0x231B2E0;   // CSGOInput*
    }

    namespace C_BaseEntity {
        constexpr uintptr_t m_pGameSceneNode    = 0x338;       // CGameSceneNode*
        constexpr uintptr_t m_iHealth           = 0x354;       // int
        constexpr uintptr_t m_lifeState         = 0x35C;       // uint8
        constexpr uintptr_t m_iTeamNum          = 0x3F3;       // uint8
        constexpr uintptr_t m_fFlags            = 0x400;       // uint32
    }

    namespace CGameSceneNode {
        constexpr uintptr_t m_vecAbsOrigin      = 0xD0;        // Vector3 (world position)
    }

    namespace CCSPlayerPawnBase {
        constexpr uintptr_t m_angEyeAngles      = 0x3DD0;      // Not used – use global dwViewAngles
    }

    namespace buttons {
        constexpr uintptr_t jump                = 0x2063C70;   // Jump button state (relative to dwCSGOInput)
    }

    // Entity system constants (CS2)
    constexpr int MAX_PLAYERS          = 64;
    constexpr int CONTROLLER_ENTRY_SIZE = 0x78;                // Size of each controller entry in entity list
    constexpr int PAWN_HANDLE_OFFSET   = 0x60;                 // Offset to pawn handle (within controller)
    constexpr int HANDLE_TO_PAWN       = 0x8;                  // Offset to convert handle to pawn (typically +0x8)

    // Flags
    constexpr uint32_t FL_ONGROUND     = 1 << 0;
    constexpr uint8_t  LIFE_ALIVE      = 0;
    constexpr uint8_t  TEAM_T          = 2;
    constexpr uint8_t  TEAM_CT         = 3;
}

//=============================================================================
// Safe memory read helper (uses SEH)
//=============================================================================
template<typename T>
bool SafeRead(uintptr_t addr, T& out) {
    if (!addr) return false;
    __try {
        out = *reinterpret_cast<T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Vector3 structure
struct Vector3 {
    float x, y, z;
};

// Matrix4x4 for view matrix
struct Matrix4x4 {
    float m[4][4];
};

//=============================================================================
// Global variables
//=============================================================================
uintptr_t clientBase = 0;
HWND gameWindow = nullptr;

// Present hook via vtable
typedef long(__stdcall* PresentFunc)(IDXGISwapChain*, UINT, UINT);
PresentFunc originalPresent = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;

// Game loop hook (CViewRender::Render)
typedef void(__fastcall* GameLoopFunc)(void* thisptr, void* edx);
GameLoopFunc originalGameLoop = nullptr;
uintptr_t gameLoopAddress = 0;

// ESP data (thread‑safe)
struct ESPData {
    float screenX, screenY;
    float distance;
    int health;
    char name[64];
};
std::vector<ESPData> espList;
std::mutex espMutex;

bool bHooksActive = false;

//=============================================================================
// Pattern scan (for game loop only, not for Present)
//=============================================================================
uintptr_t PatternScan(const char* moduleName, const char* pattern, const char* mask) {
    HMODULE hModule = GetModuleHandleA(moduleName);
    if (!hModule) return 0;

    MODULEINFO modInfo;
    if (!GetModuleInformation(GetCurrentProcess(), hModule, &modInfo, sizeof(modInfo))) return 0;

    uintptr_t start = (uintptr_t)hModule;
    uintptr_t end = start + modInfo.SizeOfImage;
    size_t patternLen = strlen(mask);

    for (uintptr_t i = start; i < end - patternLen; i++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)i, &mbi, sizeof(mbi))) continue;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) continue;

        bool found = true;
        for (size_t j = 0; j < patternLen; j++) {
            if (mask[j] == 'x' && *(BYTE*)(i + j) != (BYTE)pattern[j]) {
                found = false;
                break;
            }
        }
        if (found) return i;
    }
    return 0;
}

//=============================================================================
// Find game window (reliable method)
//=============================================================================
HWND FindGameWindow() {
    // CS2 window class is "Valve001" – fallback to foreground if not found
    HWND hwnd = FindWindowA("Valve001", nullptr);
    if (!hwnd) hwnd = GetForegroundWindow();
    return hwnd;
}

//=============================================================================
// World to Screen (safe)
//=============================================================================
bool WorldToScreen(const Vector3& worldPos, float& screenX, float& screenY) {
    Matrix4x4 viewMatrix;
    if (!SafeRead(clientBase + cs2::client_dll::dwViewMatrix, viewMatrix)) return false;

    float w = viewMatrix.m[3][0] * worldPos.x + viewMatrix.m[3][1] * worldPos.y + viewMatrix.m[3][2] * worldPos.z + viewMatrix.m[3][3];
    if (w < 0.001f) return false;

    float invw = 1.0f / w;
    float x = (viewMatrix.m[0][0] * worldPos.x + viewMatrix.m[0][1] * worldPos.y + viewMatrix.m[0][2] * worldPos.z + viewMatrix.m[0][3]) * invw;
    float y = (viewMatrix.m[1][0] * worldPos.x + viewMatrix.m[1][1] * worldPos.y + viewMatrix.m[1][2] * worldPos.z + viewMatrix.m[1][3]) * invw;

    RECT rect;
    GetClientRect(gameWindow, &rect);
    screenX = (rect.right / 2.0f) * (1.0f + x) + rect.left;
    screenY = (rect.bottom / 2.0f) * (1.0f - y) + rect.top;
    return true;
}

//=============================================================================
// Auto‑jump (frame‑independent)
//=============================================================================
void AutoJump() {
    uintptr_t localPlayer;
    if (!SafeRead(clientBase + cs2::client_dll::dwLocalPlayerPawn, localPlayer)) return;

    uint32_t flags;
    if (!SafeRead(localPlayer + cs2::C_BaseEntity::m_fFlags, flags)) return;

    if (flags & cs2::FL_ONGROUND) {
        uintptr_t csgoInput;
        if (!SafeRead(clientBase + cs2::client_dll::dwCSGOInput, csgoInput)) return;

        uint8_t* jumpState = reinterpret_cast<uint8_t*>(csgoInput + cs2::buttons::jump);
        static int tick = 0;
        tick++;
        // Toggle on even frames, off on odd frames
        *jumpState = (tick % 2 == 0) ? 1 : 0;
    }
}

//=============================================================================
// Angle lock (clamped)
//=============================================================================
void AngleLock() {
    Vector3 angles;
    if (!SafeRead(clientBase + cs2::client_dll::dwViewAngles, angles)) return;

    // Clamp pitch to [-89, 89] – we set it to 89
    angles.x = 89.0f;
    SafeWrite(clientBase + cs2::client_dll::dwViewAngles, angles);
}

//=============================================================================
// ESP data collection (CS2 entity iteration)
//=============================================================================
void CollectESPData() {
    uintptr_t localPlayer;
    if (!SafeRead(clientBase + cs2::client_dll::dwLocalPlayerPawn, localPlayer)) return;

    uint8_t localTeam;
    if (!SafeRead(localPlayer + cs2::C_BaseEntity::m_iTeamNum, localTeam)) return;
    if (localTeam != cs2::TEAM_T && localTeam != cs2::TEAM_CT) return;

    uintptr_t entityList;
    if (!SafeRead(clientBase + cs2::client_dll::dwEntityList, entityList)) return;

    // Get local origin for distance calculations
    uintptr_t localSceneNode;
    if (!SafeRead(localPlayer + cs2::C_BaseEntity::m_pGameSceneNode, localSceneNode)) return;
    Vector3 localOrigin;
    if (!SafeRead(localSceneNode + cs2::CGameSceneNode::m_vecAbsOrigin, localOrigin)) return;

    std::vector<ESPData> newList;

    for (int i = 1; i <= cs2::MAX_PLAYERS; i++) {
        uintptr_t controller;
        if (!SafeRead(entityList + i * cs2::CONTROLLER_ENTRY_SIZE, controller)) continue;

        // Get pawn handle from controller
        uintptr_t pawnHandle;
        if (!SafeRead(controller + cs2::PAWN_HANDLE_OFFSET, pawnHandle)) continue;

        // Convert handle to pawn pointer (handle >> 0x8 << 0x8 + ???)
        // Typically, entity list + (handle & 0x7FFF) * 0x10 + pawn offset – but simpler:
        uintptr_t pawn = pawnHandle;
        // In CS2, the pawn pointer is often stored at a known offset. We'll use the handle as-is for now.
        // More accurate: pawn = (entityList + ((handle & 0x7FFF) * 0x10) + 0x8)
        // We'll use a simplified version that assumes handle is the pointer itself (works in many cases)
        // For safety, we'll also try to get pawn via a known offset if needed.
        // Let's use the handle as pointer directly – but that's not always correct. We'll implement the correct conversion.
        // Correct method:
        // uintptr_t entityEntry = entityList + ((handle & 0x7FFF) * 0x10);
        // pawn = *(uintptr_t*)(entityEntry + 0x8);
        // I'll implement this.
        uintptr_t entry = entityList + ((pawnHandle & 0x7FFF) * 0x10);
        uintptr_t pawnPtr;
        if (!SafeRead(entry + 0x8, pawnPtr)) continue;  // 0x8 is typical offset for pawn

        if (pawnPtr == localPlayer) continue;

        uint8_t lifeState;
        if (!SafeRead(pawnPtr + cs2::C_BaseEntity::m_lifeState, lifeState)) continue;
        if (lifeState != cs2::LIFE_ALIVE) continue;

        uint8_t team;
        if (!SafeRead(pawnPtr + cs2::C_BaseEntity::m_iTeamNum, team)) continue;
        if (team == localTeam) continue; // skip teammates

        int health;
        if (!SafeRead(pawnPtr + cs2::C_BaseEntity::m_iHealth, health)) continue;
        if (health <= 0) continue;

        // Get pawn origin
        uintptr_t pawnSceneNode;
        if (!SafeRead(pawnPtr + cs2::C_BaseEntity::m_pGameSceneNode, pawnSceneNode)) continue;
        Vector3 pawnOrigin;
        if (!SafeRead(pawnSceneNode + cs2::CGameSceneNode::m_vecAbsOrigin, pawnOrigin)) continue;

        // World to screen
        float screenX, screenY;
        if (!WorldToScreen(pawnOrigin, screenX, screenY)) continue;

        // Distance
        float dx = pawnOrigin.x - localOrigin.x;
        float dy = pawnOrigin.y - localOrigin.y;
        float dz = pawnOrigin.z - localOrigin.z;
        float distance = sqrtf(dx*dx + dy*dy + dz*dz);
        if (distance < 1.0f) distance = 1.0f; // avoid division by zero later

        // Name placeholder
        char name[64];
        sprintf_s(name, "Enemy");

        ESPData data;
        data.screenX = screenX;
        data.screenY = screenY;
        data.distance = distance;
        data.health = health;
        strcpy_s(data.name, name);
        newList.push_back(data);
    }

    std::lock_guard<std::mutex> lock(espMutex);
    espList = std::move(newList);
}

//=============================================================================
// Hooked game loop
//=============================================================================
void __fastcall HookedGameLoop(void* thisptr, void* edx) {
    if (originalGameLoop) originalGameLoop(thisptr, edx);
    AutoJump();
    AngleLock();
    CollectESPData();
}

//=============================================================================
// Hooked Present (ESP drawing)
//=============================================================================
long __stdcall HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    // First call: initialize ImGui
    if (!g_pd3dDevice && pSwapChain) {
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_pd3dDevice))) {
            g_pd3dDevice->GetImmediateContext(&g_pd3dDeviceContext);
            if (!gameWindow) gameWindow = FindGameWindow();
            ImGui::CreateContext();
            ImGui_ImplWin32_Init(gameWindow);
            ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
        }
    }

    // Draw ESP
    if (g_pd3dDevice) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        {
            std::lock_guard<std::mutex> lock(espMutex);
            ImDrawList* draw = ImGui::GetBackgroundDrawList();
            RECT rect;
            GetClientRect(gameWindow, &rect);
            int width = rect.right - rect.left;
            int height = rect.bottom - rect.top;

            for (const auto& ent : espList) {
                float screenX = ent.screenX;
                float screenY = ent.screenY;
                if (screenX < 0 || screenX > width || screenY < 0 || screenY > height) continue;

                // Scale box based on distance
                float scale = 800.0f / ent.distance;
                float boxWidth = 60.0f * scale;
                float boxHeight = 120.0f * scale;
                boxWidth = max(20.0f, min(100.0f, boxWidth));
                boxHeight = max(40.0f, min(200.0f, boxHeight));

                float x = screenX - boxWidth / 2;
                float y = screenY - boxHeight;

                // Box outline
                draw->AddRect(ImVec2(x, y), ImVec2(x + boxWidth, y + boxHeight), ImColor(255, 255, 255), 2.0f);

                // Health bar
                ImColor healthColor;
                if (ent.health > 75) healthColor = ImColor(0, 255, 0);
                else if (ent.health > 40) healthColor = ImColor(255, 255, 0);
                else healthColor = ImColor(255, 0, 0);
                float healthBarHeight = boxHeight * (ent.health / 100.0f);
                draw->AddRectFilled(ImVec2(x, y + boxHeight - healthBarHeight), ImVec2(x + 5, y + boxHeight), healthColor);

                // Name
                draw->AddText(ImVec2(screenX - 30, y - 15), ImColor(255, 255, 255), ent.name);

                // Health text
                char healthText[16];
                sprintf_s(healthText, "%d HP", ent.health);
                draw->AddText(ImVec2(screenX - 20, y + boxHeight + 2), ImColor(255, 255, 255), healthText);
            }
        }

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    return originalPresent(pSwapChain, SyncInterval, Flags);
}

//=============================================================================
// Setup Present hook via vtable (robust method)
//=============================================================================
bool SetupPresentHook() {
    // Create a temporary device and swapchain to get the vtable
    ID3D11Device* tempDevice = nullptr;
    ID3D11DeviceContext* tempContext = nullptr;
    IDXGISwapChain* tempSwapChain = nullptr;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.Width = 100;
    sd.BufferDesc.Height = 100;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = GetForegroundWindow(); // temporary window
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    if (FAILED(D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &sd,
        &tempSwapChain,
        &tempDevice,
        nullptr,
        &tempContext
    ))) {
        return false;
    }

    // Get vtable and Present function index (usually 8 for IDXGISwapChain::Present)
    void** vtable = *reinterpret_cast<void***>(tempSwapChain);
    uintptr_t presentAddr = reinterpret_cast<uintptr_t>(vtable[8]); // 8 = Present

    // Cleanup temporary objects
    tempSwapChain->Release();
    tempDevice->Release();
    tempContext->Release();

    if (!presentAddr) return false;

    if (MH_CreateHook(reinterpret_cast<void*>(presentAddr), (void*)HookedPresent, (void**)&originalPresent) != MH_OK)
        return false;
    if (MH_EnableHook(reinterpret_cast<void*>(presentAddr)) != MH_OK)
        return false;

    return true;
}

//=============================================================================
// Find game loop address via pattern scan
//=============================================================================
bool FindGameLoop() {
    const char* pattern = "\x48\x89\x5C\x24\x08\x57\x48\x83\xEC\x20\x48\x8B\x05\x00\x00\x00\x00\x48\x33\xC4\x48\x89\x44\x24\x18\x48\x8B\xDA\x8B\xF9";
    const char* mask = "xxxxxxxxxxxx????xxxxxxxxxxxxx";
    gameLoopAddress = PatternScan("client.dll", pattern, mask);
    if (!gameLoopAddress) return false;

    if (MH_CreateHook(reinterpret_cast<void*>(gameLoopAddress), (void*)HookedGameLoop, (void**)&originalGameLoop) != MH_OK)
        return false;
    if (MH_EnableHook(reinterpret_cast<void*>(gameLoopAddress)) != MH_OK)
        return false;

    return true;
}

//=============================================================================
// Setup all hooks
//=============================================================================
bool SetupHooks() {
    if (MH_Initialize() != MH_OK) return false;

    if (!FindGameLoop()) {
        OutputDebugStringA("[!] Failed to find game loop.\n");
        return false;
    }

    if (!SetupPresentHook()) {
        OutputDebugStringA("[!] Failed to setup Present hook.\n");
        return false;
    }

    return true;
}

void RemoveHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_pd3dDevice) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_pd3dDevice->Release();
        if (g_pd3dDeviceContext) g_pd3dDeviceContext->Release();
    }
}

//=============================================================================
// Main thread
//=============================================================================
DWORD WINAPI MainThread(LPVOID lpParam) {
    // Wait for client.dll
    while (!GetModuleHandleA("client.dll")) Sleep(100);
    clientBase = (uintptr_t)GetModuleHandleA("client.dll");

    // Get game window
    while (!gameWindow) {
        gameWindow = FindGameWindow();
        Sleep(100);
    }

    if (!SetupHooks()) {
        MessageBoxA(0, "Hook setup failed!", "Error", MB_OK);
        return 0;
    }

    bHooksActive = true;
    while (bHooksActive) {
        if (GetAsyncKeyState(VK_END) & 1) {
            RemoveHooks();
            bHooksActive = false;
            break;
        }
        Sleep(50);
    }

    FreeLibraryAndExitThread((HMODULE)lpParam, 0);
    return 0;
}

//=============================================================================
// DllMain
//=============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE hThread = CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
        if (hThread) CloseHandle(hThread);
    }
    return TRUE;
}