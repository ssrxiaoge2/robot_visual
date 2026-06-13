/**
 * Minimal network barcode verification based on the vendor demo.
 *
 * This intentionally uses dlopen/dlsym like N-ScanHubDemo.cpp so it can
 * verify the vendor library independently from the main Qt application.
 */

#include "N-ScanHub.h"

#include <dlfcn.h>

#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
void *g_handle = nullptr;

template<typename Function>
Function loadFunction(const char *name)
{
    dlerror();
    void *symbol = dlsym(g_handle, name);
    const char *error = dlerror();
    if (error) {
        std::fprintf(stderr, "dlsym(%s) failed: %s\n", name, error);
        return nullptr;
    }
    return reinterpret_cast<Function>(symbol);
}

const char *lastError()
{
    using Function = char *(*)();
    const Function function = loadFunction<Function>("nl_GetLastError");
    if (!function)
        return "nl_GetLastError unavailable";
    const char *message = function();
    return message && message[0] != '\0' ? message : "<empty>";
}

int closeSocket(int *socket)
{
    using Function = int (*)(int *);
    const Function function = loadFunction<Function>("nl_CloseClientSocket");
    return function ? function(socket) : -1;
}

bool parseHexBytes(const char *input, std::vector<char> &bytes)
{
    std::string compact;
    for (const char *cursor = input; *cursor != '\0'; ++cursor) {
        const unsigned char ch = static_cast<unsigned char>(*cursor);
        if (std::isspace(ch) || ch == ':' || ch == '-' || ch == ',')
            continue;
        compact.push_back(*cursor);
    }

    if (compact.size() >= 2 && compact[0] == '0'
        && (compact[1] == 'x' || compact[1] == 'X')) {
        compact.erase(0, 2);
    }
    if (compact.empty() || compact.size() % 2 != 0)
        return false;

    bytes.clear();
    bytes.reserve(compact.size() / 2);
    for (std::size_t index = 0; index < compact.size(); index += 2) {
        const std::string byteText = compact.substr(index, 2);
        char *end = nullptr;
        const long value = std::strtol(byteText.c_str(), &end, 16);
        if (!end || *end != '\0' || value < 0 || value > 0xff)
            return false;
        bytes.push_back(static_cast<char>(value));
    }
    return true;
}
}

int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 5) {
        std::fprintf(stderr,
                     "Usage: %s <scanner-ip> [port=30000] [timeout-ms=5000] "
                     "[trigger-hex=41]\n",
                     argv[0]);
        return 64;
    }

    const char *ip = argv[1];
    const int port = argc >= 3 ? std::atoi(argv[2]) : 30000;
    const int timeoutMs = argc >= 4 ? std::atoi(argv[3]) : 5000;
    const char *triggerText = argc >= 5 ? argv[4] : "41";
    if (port <= 0 || port > 65535 || timeoutMs <= 0) {
        std::fprintf(stderr, "Invalid port or timeout\n");
        return 64;
    }

    std::vector<char> trigger;
    if (!parseHexBytes(triggerText, trigger)) {
        std::fprintf(stderr, "Invalid trigger hex: %s\n", triggerText);
        return 64;
    }

    g_handle = dlopen("./N-ScanHub.so", RTLD_NOW);
    if (!g_handle) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 65;
    }

    using ConnectFunction = int (*)(char *, int, int *);
    using SendFunction = int (*)(int, char *, int);
    using ReadFunction = int (*)(int, int, char *, int *);
    const ConnectFunction connectToService =
        loadFunction<ConnectFunction>("nl_connectToService");
    const SendFunction sendData =
        loadFunction<SendFunction>("nl_sendDataToSocket");
    const ReadFunction readData =
        loadFunction<ReadFunction>("nl_readFromSocket");
    if (!connectToService || !sendData || !readData) {
        dlclose(g_handle);
        return 65;
    }

    int socket = -1;
    char ipBuffer[64] = {};
    std::snprintf(ipBuffer, sizeof(ipBuffer), "%s", ip);

    std::printf("[1/3] Connecting to %s:%d ...\n", ipBuffer, port);
    int result = connectToService(ipBuffer, port, &socket);
    std::printf("      ret=%d socket=%d sdkError=%s\n",
                result, socket, lastError());
    if (result != 0 || socket < 0) {
        if (socket >= 0)
            closeSocket(&socket);
        dlclose(g_handle);
        return 1;
    }

    std::printf("[2/3] Sending trigger:");
    for (const char byte : trigger)
        std::printf(" %02X", static_cast<unsigned char>(byte));
    std::printf(" ...\n");
    result = sendData(socket, trigger.data(), static_cast<int>(trigger.size()));
    std::printf("      ret=%d sdkError=%s\n", result, lastError());
    if (result != 0) {
        closeSocket(&socket);
        dlclose(g_handle);
        return 2;
    }

    char buffer[4096] = {};
    int receivedLength = 0;
    std::printf("[3/3] Waiting up to %d ms for barcode ...\n", timeoutMs);
    result = readData(socket, timeoutMs, buffer, &receivedLength);
    std::printf("      ret=%d length=%d sdkError=%s\n",
                result, receivedLength, lastError());

    if (result == 0 && receivedLength > 0) {
        std::printf("      barcode=%.*s\n", receivedLength, buffer);
        std::printf("      hex=");
        for (int i = 0; i < receivedLength; ++i)
            std::printf("%02X ", static_cast<unsigned char>(buffer[i]));
        std::printf("\n");
    }

    const int closeResult = closeSocket(&socket);
    std::printf("[done] closeRet=%d socket=%d\n", closeResult, socket);
    dlclose(g_handle);

    return result == 0 && receivedLength > 0 ? 0 : 3;
}
