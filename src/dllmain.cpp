#include <Windows.h>
#include <iostream>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            // Log the actual module path being loaded
            char moduleName[256];
            GetModuleFileNameA(hModule, moduleName, sizeof(moduleName));
            std::cout << "[PDPerfPlugin] Module loaded from: " << moduleName << std::endl;
            break;
        case DLL_PROCESS_DETACH:
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            break;
    }
    return TRUE;
}
