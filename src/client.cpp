#include <iostream>
#include <fstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define PORT 8080
#define CHUNK_SIZE 4096
#define SERVER_IP "192.168.137.1"
#define END_SIGNAL "END_OF_FILE"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ./client <filename>\n";
        return 1;
    }

    std::string filename = argv[1];
    std::ifstream infile(filename, std::ios::binary);
    if (!infile) {
        std::cerr << "Cannot open file: " << filename << "\n";
        return 1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Connection failed! Is server running?\n";
        return 1;
    }
    std::cout << "=== Connected to server! ===\n";

    // Send just the base filename (not full path)
    std::string base = filename;
    size_t slash = filename.find_last_of("/\\");
    if (slash != std::string::npos) base = filename.substr(slash + 1);

    send(sock, base.c_str(), base.size() + 1, 0);
    usleep(200000); // 0.2 second delay

    // Send file in chunks
    char buffer[CHUNK_SIZE];
    int chunk_num = 0;
    long total_bytes = 0;

    while (!infile.eof()) {
        infile.read(buffer, CHUNK_SIZE);
        int bytes_read = infile.gcount();
        if (bytes_read > 0) {
            send(sock, buffer, bytes_read, 0);
            total_bytes += bytes_read;
            chunk_num++;
            std::cout << "Sent chunk " << chunk_num << " (" << bytes_read << " bytes)\n";
        }
    }

    send(sock, END_SIGNAL, strlen(END_SIGNAL), 0);
    std::cout << "\n=== File sent successfully! ===\n";
    std::cout << "Total chunks: " << chunk_num << "\n";
    std::cout << "Total bytes:  " << total_bytes << "\n";

    infile.close();
    close(sock);
    return 0;
}
