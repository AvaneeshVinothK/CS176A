//Concepts Taken from https://github.com/rtjuandi/Hangman-Networking/blob/master/game_server.cpp
//Concepts Taken from https://www.youtube.com/watch?v=KEiur5aZnIM
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define MAX_CLIENTS 3
#define MAX_WORD_LEN 8
#define MIN_WORD_LEN 3
#define MAX_INCORRECT 6
#define MAX_WORDS 15
#define MAX_PACKET_SIZE 100 

char word_pool[MAX_WORDS][MAX_WORD_LEN + 1];
int word_pool_count = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t word_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct 
{
    uint8_t msg_length;
    char data[1];
} ClientPacket;

typedef struct 
{
    int client_socket;
    pthread_t thread_id;
    char secret_word[MAX_WORD_LEN + 1];
    char display_word[MAX_WORD_LEN + 1];
    char incorrect_guesses[MAX_INCORRECT + 1];
    int incorrect_count;
    int slot_index;
    int is_active; 
} GameSession;

GameSession game_sessions[MAX_CLIENTS];

void load_words() {
    pthread_mutex_lock(&word_pool_mutex);
    FILE *file = fopen("hangman_words.txt", "r");
    if (file == NULL) 
    {
        pthread_mutex_unlock(&word_pool_mutex);
        return;
    }

    char line[MAX_WORD_LEN + 2];
    word_pool_count = 0;

    while (fgets(line, sizeof(line), file) && word_pool_count < MAX_WORDS) 
    {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') 
        {
            line[len - 1] = '\0';
            len--;
        }
        
        if (len >= MIN_WORD_LEN && len <= MAX_WORD_LEN) 
        {
            strcpy(word_pool[word_pool_count], line);
            word_pool[word_pool_count][MAX_WORD_LEN] = '\0';
            word_pool_count++;
        } 
    }
    
    fclose(file);
    pthread_mutex_unlock(&word_pool_mutex);
}

void send_message_packet(int fd, const char *message) 
{
    uint8_t msg_len = (uint8_t)strlen(message);
    size_t packet_size = sizeof(uint8_t) + msg_len;
    uint8_t buffer[packet_size];
    
    buffer[0] = msg_len;
    memcpy(buffer + 1, message, msg_len);
    
    send(fd, buffer, packet_size, 0);
}

void send_game_control_packet(int fd, GameSession *session) 
{
    uint8_t word_len = (uint8_t)strlen(session->secret_word);
    uint8_t num_incorrect = (uint8_t)session->incorrect_count;
    
    size_t data_size = word_len + num_incorrect;
    size_t packet_size = 3 + data_size;
    uint8_t buffer[packet_size];
    
    buffer[0] = 0;
    buffer[1] = word_len;
    buffer[2] = num_incorrect;
    
    memcpy(buffer + 3, session->display_word, word_len);
    memcpy(buffer + 3 + word_len, session->incorrect_guesses, num_incorrect);

    send(fd, buffer, packet_size, 0);
}

int process_guess(GameSession *session, char guess) 
{
    int is_correct = 0;
    int len = strlen(session->secret_word);
    
    for (int i = 0; i < len; i++) 
    {
        if (session->secret_word[i] == guess && session->display_word[i] == '_') 
        {
            session->display_word[i] = guess;
            is_correct = 1;
        }
    }

    if (!is_correct) 
    {
        if (strchr(session->incorrect_guesses, guess) == NULL) 
        {
            session->incorrect_guesses[session->incorrect_count] = guess;
            session->incorrect_count++;
            session->incorrect_guesses[session->incorrect_count] = '\0';
        }
    }

    int won  = (strchr(session->display_word, '_') == NULL);
    int lost = (session->incorrect_count >= MAX_INCORRECT);

    if (won || lost)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

void cleanup_and_exit(int client_fd, GameSession *session) {
    close(client_fd);
    pthread_mutex_lock(&clients_mutex);
    session->is_active = 0;
    pthread_mutex_unlock(&clients_mutex);
    pthread_exit(NULL);
}


void *handle_client_game(void *arg) {
    GameSession *session = (GameSession *)arg;
    int client_fd = session->client_socket;
    
    printf("[Slot %d] New game handler started.\n", session->slot_index);
    
    if (word_pool_count == 0) 
    {
        send_message_packet(client_fd, "Server error: No words loaded. Closing connection.");
        cleanup_and_exit(client_fd, session);
    }
    
    pthread_mutex_lock(&word_pool_mutex);
    int random_index = rand() % word_pool_count;
    strcpy(session->secret_word, word_pool[random_index]);
    pthread_mutex_unlock(&word_pool_mutex);

    int word_len = strlen(session->secret_word);
    
    memset(session->display_word, '_', word_len);
    session->display_word[word_len] = '\0';
    session->incorrect_guesses[0] = '\0';
    session->incorrect_count = 0;

    ClientPacket client_start_pkt;
    ssize_t bytes_recvd = recv(client_fd, &client_start_pkt, sizeof(uint8_t), MSG_WAITALL);
    
    if (bytes_recvd <= 0 || client_start_pkt.msg_length != 0) 
    {
        cleanup_and_exit(client_fd, session);
    }

    send_game_control_packet(client_fd, session);
    
    int game_over = 0;
    while (!game_over) 
    {
        ClientPacket guess_pkt;
        
        bytes_recvd = recv(client_fd, &guess_pkt, sizeof(uint8_t), MSG_WAITALL); 
        if (bytes_recvd <= 0) break;
        
        bytes_recvd = recv(client_fd, guess_pkt.data, guess_pkt.msg_length, MSG_WAITALL);
        if (bytes_recvd <= 0) break;
        
        char guess = tolower(guess_pkt.data[0]);
        game_over = process_guess(session, guess);
        if (game_over) 
        {
            int won = (strchr(session->display_word, '_') == NULL);
            char final_word_msg[30];
            snprintf(final_word_msg, sizeof(final_word_msg), "The word was %s", session->secret_word);
            send_message_packet(client_fd, final_word_msg);
            send_message_packet(client_fd, won ? "You Win!" : "You Lose!");
            send_message_packet(client_fd, "Game Over!");
        } 
        else 
        {
            send_game_control_packet(client_fd, session);
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) 
{
    if (argc != 2) 
    {
        exit(1);
    }
    int port = atoi(argv[1]);
    srand(time(NULL)); 
    load_words();
    if (word_pool_count == 0) 
    {
        exit(1);
    }

    for (int i = 0; i < MAX_CLIENTS; i++) 
    {
        game_sessions[i].is_active = 0;
        game_sessions[i].slot_index = i;
    }

    int listen_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) 
    {
        exit(1);
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) 
    {
        close(listen_fd);
        exit(1);
    }
    
    if (listen(listen_fd, 5) < 0) 
    {
        close(listen_fd);
        exit(1);
    }
    
    printf("Hangman Server listening on port %d\n", port);

    while (1) 
    {
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) 
        {
            continue;
        }

        pthread_mutex_lock(&clients_mutex);
        int slot_index = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) 
        {
            if (game_sessions[i].is_active == 0)
            {
                slot_index = i;
                break;
            }
        }
        pthread_mutex_unlock(&clients_mutex);

        if (slot_index == -1) 
        {
            send_message_packet(client_fd, "server-overloaded");
            close(client_fd);
        } 
        else 
        {
            GameSession *session = &game_sessions[slot_index];
            session->client_socket = client_fd;
            session->is_active = 1;
            load_words();
            
            if (pthread_create(&session->thread_id, NULL, handle_client_game, session) != 0) 
            {
                close(client_fd);
                session->is_active = 0;
            }
            pthread_detach(session->thread_id);
        }
    }
    
    close(listen_fd);
    return 0;
}