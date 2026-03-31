// ============================================================
// client.cpp — Weeks 1-5: TCP + AES-128-CBC encrypt + SHA-256 integrity
// ============================================================
#include <iostream>
#include <fstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "../include/crypto.h"

#define PORT       8080
#define CHUNK_SIZE 4096

static bool send_all(int sock, const void* buf, size_t len) {
    const char* p = (const char*)buf;
    while (len > 0) {
        int n = send(sock, p, len, 0);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ./client <filename> [server_ip]\n";
        return 1;
    }

    std::string filename  = argv[1];
    const char* server_ip = (argc >= 3) ? argv[2] : "127.0.0.1";

    std::ifstream infile(filename, std::ios::binary);
    if (!infile) { std::cerr << "Cannot open file: " << filename << "\n"; return 1; }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { std::cerr << "Socket creation failed\n"; return 1; }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(PORT);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid IP\n"; return 1;
    }
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Connection failed!\n"; return 1;
    }
    std::cout << "=== Connected to " << server_ip << ":" << PORT << " ===\n";

    std::string base = filename;
    size_t slash = filename.find_last_of("/\\");
    if (slash != std::string::npos) base = filename.substr(slash + 1);
    send(sock, base.c_str(), base.size() + 1, 0);
    usleep(200000);

    uint8_t plain_buf[CHUNK_SIZE];
    uint8_t out_buf[CRYPTO_HDR + CHUNK_SIZE + AES_BLOCK + 16];

    int  chunk_num   = 0;
    long total_plain = 0;
    long total_enc   = 0;

    while (true) {
        infile.read((char*)plain_buf, CHUNK_SIZE);
        int bytes_read = (int)infile.gcount();
        if (bytes_read <= 0) break;

        uint8_t hash[HASH_SIZE];
        SHA256(plain_buf, bytes_read, hash);
        std::string hex = hash_to_hex(hash);

        int enc_len = crypto_encrypt(plain_buf, bytes_read,
                                     out_buf, sizeof(out_buf));
        if (enc_len < 0) {
            std::cerr << "Encryption failed at chunk " << chunk_num+1 << "\n";
            close(sock); return 1;
        }

        uint32_t frame_len_net = htonl((uint32_t)enc_len);
        if (!send_all(sock, &frame_len_net, 4) ||
            !send_all(sock, out_buf, enc_len)) {
            std::cerr << "Send failed at chunk " << chunk_num+1 << "\n";
            close(sock); return 1;
        }

        total_plain += bytes_read;
        total_enc   += enc_len;
        chunk_num++;

        std::cout << "Chunk " << chunk_num
                  << " sent (plain=" << bytes_read << "B"
                  << " enc=" << enc_len << "B)"
                  << " sha256=" << hex.substr(0,16) << "...\n";
    }

    usleep(100000);
    uint32_t end_marker = htonl(0xFFFFFFFF);
    send_all(sock, &end_marker, 4);

    std::cout << "\n=== Transfer complete ===\n"
              << "Chunks      : " << chunk_num   << "\n"
              << "Plain bytes : " << total_plain << "\n"
              << "Enc bytes   : " << total_enc   << "\n";

    infile.close();
    close(sock);
    return 0;
}
