#include <WinSock2.h>
#include <WS2tcpip.h>
#include <MultiSocketRUDPCore.h>

int main()
{
    // Construction verifies library linkage; listening starts only with StartServer().
    MultiSocketRUDPCore server(L"MY", L"DevServerCert");
    return 0;
}
