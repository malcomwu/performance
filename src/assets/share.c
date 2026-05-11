#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// ===== Shared function: safe read line =====
ssize_t safe_read_line(int fd, char *buffer, size_t maxlen) {
    if (!buffer || maxlen == 0) return -1;
    ssize_t n, total = 0;
    char c;
    while (total < (ssize_t)(maxlen - 1)) {
        n = read(fd, &c, 1);
        if (n > 0) {
            if (c == '\n') break;
            buffer[total++] = c;
        } else if (n == 0) {
            break; // EOF
        } else {
            perror("read");
            return -1;
        }
    }
    buffer[total] = '\0';
    return total;
}

int main() {
    int sockfd;
    struct sockaddr_in server_addr;
    fd_set readfds;
    struct timeval timeout;
    char buffer[1024];

    // Create a UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket");
        return 1;
    }

    // Configure server address (localhost:9000)
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(9000);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    printf("Type something or wait for UDP data (timeout 5s)...\n");

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds); // Monitor stdin
        FD_SET(sockfd, &readfds);       // Monitor socket

        int maxfd = (sockfd > STDIN_FILENO) ? sockfd : STDIN_FILENO;

        timeout.tv_sec = 5;
        timeout.tv_usec = 0;

        int activity = select(maxfd + 1, &readfds, NULL, NULL, &timeout);

        if (activity < 0) {
            perror("select");
            break;
        } else if (activity == 0) {
            printf("Timeout: no activity.\n");
            continue;
        }

        // Check stdin
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (safe_read_line(STDIN_FILENO, buffer, sizeof(buffer)) > 0) {
                printf("You typed: %s\n", buffer);
                // Send to server
                sendto(sockfd, buffer, strlen(buffer), 0,
                       (struct sockaddr *)&server_addr, sizeof(server_addr));
            }
        }

        // Check socket
        if (FD_ISSET(sockfd, &readfds)) {
            socklen_t addrlen = sizeof(server_addr);
            ssize_t len = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                                   (struct sockaddr *)&server_addr, &addrlen);
            if (len > 0) {
                buffer[len] = '\0';
                printf("Received UDP: %s\n", buffer);
            }
        }
    }

    close(sockfd);
    return 0;
}
