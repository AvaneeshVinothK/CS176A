#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_BUFFER 100

// --- Function Prototypes ---
char get_valid_guess();
ssize_t receive_server_packet(int fd, uint8_t *buffer);

// =================================================================
//                             HELPER FUNCTIONS
// =================================================================

/**
 * Gets valid single-letter input from the user, handles errors, and returns 0 on failure.
 */
char get_valid_guess() {
    char input[MAX_BUFFER];
    char guess = '\0';
    
    while (guess == '\0') {
        printf(">>>Letter to guess: ");
        if (fgets(input, sizeof(input), stdin) == NULL) {
            return 0; // EOF or read error
        }

        // Check if input is a single character (plus newline)
        if (strlen(input) == 2 && isalpha(input[0])) {
            guess = tolower(input[0]);
        } else {
            // Check for multi-character input or invalid characters
            if (strlen(input) > 1 && input[strlen(input) - 1] != '\n') {
                // Input line was longer than buffer, clear remaining buffer
                int c;
                while ((c = getchar()) != '\n' && c != EOF);
            }
            printf(">>>Error! Please guess one letter.\n");
        }
    }
    return guess;
}

/**
 * Receives the next complete server packet, reading byte-by-byte based on the Msg Flag.
 * Returns total bytes received, or -1 on connection failure.
 */
ssize_t receive_server_packet(int fd, uint8_t *buffer) {
    uint8_t msg_flag;
    
    // Read the Msg Flag (1 byte)
    if (recv(fd, &msg_flag, 1, MSG_WAITALL) <= 0) return -1;
    
    buffer[0] = msg_flag;
    
    if (msg_flag > 0) {
        // Message Packet: total size is 1 (flag) + msg_flag (data)
        size_t data_size = msg_flag;
        if (recv(fd, buffer + 1, data_size, MSG_WAITALL) <= 0) return -1;
        return 1 + data_size;
    } else {
        // Game Control Packet: read the next 2 bytes (Word Len, Num Incorrect)
        if (recv(fd, buffer + 1, 2, MSG_WAITALL) <= 0) return -1;
        
        uint8_t word_len = buffer[1];
        uint8_t num_incorrect = buffer[2];
        size_t data_size = word_len + num_incorrect;
        
        // Read the remaining data segment
        if (recv(fd, buffer + 3, data_size, MSG_WAITALL) <= 0) return -1;
        return 3 + data_size;
    }
}

// =================================================================
//                                MAIN
// =================================================================

int main(int argc, char *argv[]) {
    // 1. Setup
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    char *server_ip = argv[1];
    int server_port = atoi(argv[2]);

    // 2. Create Socket and Connect
    int client_fd;
    struct sockaddr_in server_addr;
    
    if ((client_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address/Address not supported");
        close(client_fd);
        exit(EXIT_FAILURE);
    }
    
    if (connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection Failed");
        close(client_fd);
        exit(EXIT_FAILURE);
    }
    
    // 3. Ready to Start Prompt
    char ready_char[10];
    printf(">>> Ready to start game? (y/n): ");
    if (fgets(ready_char, sizeof(ready_char), stdin) == NULL || tolower(ready_char[0]) != 'y') {
        close(client_fd);
        exit(EXIT_SUCCESS);
    }

    // 4. Send Start Signal (Msg length = 0)
    uint8_t start_msg_length = 0;
    if (send(client_fd, &start_msg_length, sizeof(uint8_t), 0) < 0) {
        perror("send start failed");
        close(client_fd);
        exit(EXIT_FAILURE);
    }

    // 5. Main Game Loop
    uint8_t packet_buffer[MAX_BUFFER]; 
    
    while (1) {
        ssize_t bytes_received = receive_server_packet(client_fd, packet_buffer);
        
        if (bytes_received <= 0) {
            // Connection closed or error
            printf("Server connection closed.\n");
            break;
        }

        uint8_t msg_flag = packet_buffer[0];

        if (msg_flag > 0) {
            // MESSAGE PACKET
            // Null-terminate the string data (starts at buffer[1])
            packet_buffer[bytes_received] = '\0';
            printf(">>> %s\n", (char *)(packet_buffer + 1));
            
            if (strstr((char *)(packet_buffer + 1), "Over") || strstr((char *)(packet_buffer + 1), "overloaded")) {
                break; // Game is over or connection terminated
            }
        } else {
            // GAME CONTROL PACKET
            uint8_t word_len = packet_buffer[1];
            uint8_t num_incorrect = packet_buffer[2];
            
            // Display Word State
            printf(">>>");
            for (int i = 0; i < word_len; i++) {
                printf("%c ", packet_buffer[3 + i]);
            }
            printf("\n");
            
            // Display Incorrect Guesses
            printf(">>>Incorrect Guesses:");
            for (int i = 0; i < num_incorrect; i++) {
                printf(" %c", packet_buffer[3 + word_len + i]);
            }
            printf("\n>>>\n");

            // Prompt for next guess
            char guess = get_valid_guess();
            if (guess == 0) break; // Error reading input

            // Send Guess Packet (Msg length = 1)
            uint8_t guess_pkt_data[2] = {1, guess}; 
            send(client_fd, guess_pkt_data, 2, 0); 
        }
    }

    close(client_fd);
    return 0;
}