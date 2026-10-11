#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <source_location>
#include <string>

extern "C" {
std::int64_t APS5_VABI readlink_nid_postfix(const char*, char*, std::size_t);
int APS5_VABI fchdir_nid_postfix(int);
int APS5_VABI chdir_nid_postfix(const char*);
char* APS5_VABI getcwd_nid_postfix(char*, std::size_t);
int APS5_VABI sceKernelOpen(const char*, int, std::uint16_t);
int APS5_VABI sceKernelClose(int);
int* APS5_VABI __error_nid_postfix();
}

void Require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "Kernel sys check failed at line %u (guest errno %d)\n", location.line(), *__error_nid_postfix());
        std::abort();
    }
}

int Error() {
    return *__error_nid_postfix();
}

int main() {
    const auto root = std::filesystem::path("anyps5-kernel-sys-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(std::filesystem::create_directory(root));
    const auto file = (root / "file.txt").string();
    std::ofstream(file).put('x');
    char buffer[64];
    Require(readlink_nid_postfix(file.c_str(), buffer, sizeof(buffer)) == -1 && Error() == 22);
    Require(readlink_nid_postfix(root.string().c_str(), buffer, sizeof(buffer)) == -1 && Error() == 22);
    Require(readlink_nid_postfix((root / "missing").string().c_str(), buffer, sizeof(buffer)) == -1 && Error() == 2);
    Require(readlink_nid_postfix("", buffer, sizeof(buffer)) == -1 && Error() == 2);
    Require(readlink_nid_postfix(nullptr, buffer, sizeof(buffer)) == -1 && Error() == 14);
    std::error_code error;
    const auto link = (root / "link").string();
    std::filesystem::create_symlink("sub/target.txt", link, error);
    if (!error) {
        std::memset(buffer, '#', sizeof(buffer));
        Require(readlink_nid_postfix(link.c_str(), buffer, sizeof(buffer)) == 14);
        Require(std::memcmp(buffer, "sub/target.txt", 14) == 0 && buffer[14] == '#');
        std::memset(buffer, '#', sizeof(buffer));
        Require(readlink_nid_postfix(link.c_str(), buffer, 3) == 3);
        Require(std::memcmp(buffer, "sub", 3) == 0 && buffer[3] == '#');
        Require(readlink_nid_postfix(link.c_str(), nullptr, 0) == 0);
        Require(readlink_nid_postfix(link.c_str(), nullptr, sizeof(buffer)) == -1 && Error() == 14);
        Require(readlink_nid_postfix((root / "LINK").string().c_str(), buffer, sizeof(buffer)) == 14);
    } else {
        std::fprintf(stderr, "symbolic links unavailable: the link cases not tested\n");
    }
    const int directory = sceKernelOpen(root.string().c_str(), SCE_KERNEL_O_RDONLY | SCE_KERNEL_O_DIRECTORY, 0);
    Require(directory >= 0);
    const int fileDescriptor = sceKernelOpen(file.c_str(), SCE_KERNEL_O_RDONLY, 0);
    Require(fileDescriptor >= 0);
    Require(fchdir_nid_postfix(-1) == -1 && Error() == 9);
    Require(fchdir_nid_postfix(fileDescriptor) == -1 && Error() == 20);
    Require(fchdir_nid_postfix(directory) == 0);
    char directoryPath[1024];
    Require(getcwd_nid_postfix(directoryPath, sizeof(directoryPath)) == directoryPath);
    Require(std::strcmp(directoryPath, ("/" + root.filename().string()).c_str()) == 0);
    Require(sceKernelClose(fileDescriptor) == 0);
    Require(sceKernelClose(directory) == 0);
    Require(fchdir_nid_postfix(directory) == -1 && Error() == 9);
    Require(chdir_nid_postfix("/") == 0);
    std::filesystem::remove_all(root);
    return 0;
}
