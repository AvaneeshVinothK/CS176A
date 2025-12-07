//Concepts taken from https://github.com/rtjuandi/Hangman-Networking/blob/master/game_client.cpp
//Concepts taken from https://www.youtube.com/watch?v=KEiur5aZnIM 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_BUFFER 100

char get_valid_guess() {
    char input[MAX_BUFFER];
    char guess = '\0';
    
    while (guess == '\0') {
        printf(">>>Letter to guess: ");
        if (fgets(input, sizeof(input), stdin) == NULL) 
        {
            printf("\n");
            return 0; 
        }

        if (strlen(input) == 2 && isalpha(input[0])) 
        {
            guess = tolower(input[0]);
        } 
        else 
        {
            if (strlen(input) > 1 && input[strlen(input) - 1] != '\n') 
            {
                int c;
                while ((c = getchar()) != '\n' && c != EOF);
            }
            printf(">>>Error! Please guess one letter.\n");
        }
    }
    return guess;
}

ssize_t receive_server_packet(int fd, uint8_t *buffer) {
    uint8_t msg_flag;
    
    if (recv(fd, &msg_flag, 1, MSG_WAITALL) <= 0) 
    {
        return -1;
    }

    buffer[0] = msg_flag;
    
    if (msg_flag > 0) 
    {
        size_t data_size = msg_flag;
        if (recv(fd, buffer + 1, data_size, MSG_WAITALL) <= 0)
        {
            return -1;
        }
        return 1 + data_size;
    } 
    
    else 
    {
        if (recv(fd, buffer + 1, 2, MSG_WAITALL) <= 0)
        {
             return -1;
        }
        
        uint8_t word_len = buffer[1];
        uint8_t num_incorrect = buffer[2];
        size_t data_size = word_len + num_incorrect;
        
        if (recv(fd, buffer + 3, data_size, MSG_WAITALL) <= 0)
        { 
            return -1;
        }
        return 3 + data_size;
    }
}

int main(int argc, char *argv[]) 
{
    if (argc != 3) 
    {
        exit(1);
    }
    char *server_ip = argv[1];
    int server_port = atoi(argv[2]);

    int client_fd;
    struct sockaddr_in server_addr;
    
    if ((client_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) 
    {
        exit(1);
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) 
    {
        close(client_fd);
        exit(1);
    }
    
    if (connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) 
    {
        close(client_fd);
        exit(1);
    }
    
    char ready_char[10];
    printf(">>>Ready to start game? (y/n): ");
    if (fgets(ready_char, sizeof(ready_char), stdin) == NULL || tolower(ready_char[0]) != 'y') 
    {
        if (fgets(ready_char, sizeof(ready_char), stdin) == NULL)
        {
            printf("\n");
        }
        close(client_fd);
        exit(0);
    }

    uint8_t start_msg_length = 0;
    if (send(client_fd, &start_msg_length, sizeof(uint8_t), 0) < 0) 
    {
        close(client_fd);
        exit(1);
    }

    uint8_t packet_buffer[MAX_BUFFER]; 
    
    while (1) 
    {
        ssize_t bytes_received = receive_server_packet(client_fd, packet_buffer);
        
        if (bytes_received <= 0) 
        {
            printf("Server connection closed.\n");
            break;
        }

        uint8_t msg_flag = packet_buffer[0];

        if (msg_flag > 0) 
        {
            packet_buffer[bytes_received] = '\0';
            printf(">>>%s\n", (char *)(packet_buffer + 1));
            
            if (strstr((char *)(packet_buffer + 1), "Over") || strstr((char *)(packet_buffer + 1), "overloaded")) 
            {
                break;
            }
        } 
        else 
        {
            uint8_t word_len = packet_buffer[1];
            uint8_t num_incorrect = packet_buffer[2];
            
            printf(">>>");
            for (int i = 0; i < word_len; i++) 
            {
                printf("%c", packet_buffer[3 + i]);
                if (i < word_len - 1) 
                {
                    printf(" ");
                }
            }
            printf("\n");
            
            printf(">>>Incorrect Guesses: ");
            for (int i = 0; i < num_incorrect; i++) 
            {
                printf("%c", packet_buffer[3 + word_len + i]);
            }
            printf("\n>>>\n");

            char guess = get_valid_guess();
            if (guess == 0) 
            {
                break;
            }

            uint8_t guess_pkt_data[2] = {1, guess}; 
            send(client_fd, guess_pkt_data, 2, 0); 
        }
    }

    close(client_fd);
    return 0;
}