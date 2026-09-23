#include <spatialsnapshot/spatialsnapshot.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3)
        return 2;
    std::ifstream file(argv[1], std::ios::binary);
    if (!file)
        return 2;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    ss_document_t *document = nullptr;
    if (argc == 3) {
        ss_container_t *container = nullptr;
        auto kind = !std::strcmp(argv[2], "heif") ? SS_CONTAINER_HEIF : SS_CONTAINER_QUICKTIME;
        if (ss_container_open_memory(kind, bytes.data(), bytes.size(), nullptr, &container) !=
            SS_OK)
            return 1;
        auto status = ss_container_document(container, &document);
        ss_container_release(container);
        if (status != SS_OK)
            return 1;
    } else if (ss_document_open_memory(bytes.data(), bytes.size(), nullptr, &document, nullptr) !=
               SS_OK)
        return 1;
    ss_document_info_t info{};
    info.struct_size = sizeof(info);
    info.abi_version = SS_ABI_VERSION;
    auto status = ss_document_get_info(document, &info);
    ss_document_release(document);
    if (status != SS_OK || info.camera_count < 1 || ss_version() != SS_VERSION ||
        (argc == 2 && (info.kind != SS_STILL || info.camera_count != 1)))
        return 1;
    std::cout << "Installed C ABI consumed from C++17\n";
    return 0;
}
