#include <iostream>
#include <fstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define PORT        8080
#define CHUNK_SIZE  4096
#define END_SIGNAL  "END_OF_FILE"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ./client <filename> [server_ip]\n";
        return 1;
    }

    std::string filename  = argv[1];
    // Default to localhost; optionally pass IP as second argument
    const char* server_ip = (argc >= 3) ? argv[2] : "127.0.0.1";

    std::ifstream infile(filename, std::ios::binary);
    if (!infile) {
        std::cerr << "Cannot open file: " << filename << "\n";
        return 1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Socket creation failed\n";
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(PORT);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid server IP: " << server_ip << "\n";
        return 1;
    }

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Connection failed! Is server running on " << server_ip << ":" << PORT << "?\n";
        return 1;
    }
    std::cout << "=== Connected to server " << server_ip << ":" << PORT << " ===\n";

    // Send just the base filename (not full path)
    std::string base = filename;
    size_t slash = filename.find_last_of("/\\");
    if (slash != std::string::npos) base = filename.substr(slash + 1);

    // Send filename with null terminator so server knows where it ends
    send(sock, base.c_str(), base.size() + 1, 0);
    usleep(200000); // 0.2s — give server time to set up the TCB

    // Send file in chunks
    char buffer[CHUNK_SIZE];
    int  chunk_num   = 0;
    long total_bytes = 0;

    while (true) {
        infile.read(buffer, CHUNK_SIZE);
        int bytes_read = (int)infile.gcount();
        if (bytes_read <= 0) break;

        // Send this chunk — loop until all bytes are written
        int sent = 0;
        while (sent < bytes_read) {
            int n = send(sock, buffer + sent, bytes_read - sent, 0);
            if (n <= 0) {
                std::cerr << "Send failed at chunk " << chunk_num + 1 << "\n";
                close(sock);
                return 1;
            }
            sent += n;
        }

        total_bytes += bytes_read;
        chunk_num++;
        std::cout << "Sent chunk " << chunk_num
                  << " (" << bytes_read << " bytes)\n";
    }

    // Small delay so the last data chunk is fully received before END signal
    usleep(100000);

    // Send END signal as its own separate send call
    send(sock, END_SIGNAL, strlen(END_SIGNAL), 0);

    std::cout << "\n=== File sent successfully! ===\n";
    std::cout << "Total chunks : " << chunk_num   << "\n";
    std::cout << "Total bytes  : " << total_bytes << "\n";

    infile.close();
    close(sock);
    return 0;
}
