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

// --- Global Constants ---
#define MAX_CLIENTS 3
#define MAX_WORD_LEN 8
#define MIN_WORD_LEN 3
#define MAX_INCORRECT 6
#define MAX_WORDS 15
#define MAX_PACKET_SIZE 100 

// --- Global Word Pool ---
char word_pool[MAX_WORDS][MAX_WORD_LEN + 1];
int word_pool_count = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t word_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Protocol Structures (Simplified for conceptual clarity, actual network transfer uses byte arrays) ---
typedef struct {
    uint8_t msg_length; // 1 byte
    char data[1];       // 1 byte
} ClientPacket;

// --- Game State Structure (Per Client) ---
typedef struct {
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

// --- Function Prototypes ---
void load_words();
void send_message_packet(int fd, const char *message);
void send_game_control_packet(int fd, GameSession *session);
int process_guess(GameSession *session, char guess);
void *handle_client_game(void *arg);

// =================================================================
//                 PROTOCOL AND WORD MANAGEMENT HELPERS
// =================================================================

/**
 * Loads words from hangman_words.txt into the global word_pool.
 */
void load_words() {
    pthread_mutex_lock(&word_pool_mutex);
    FILE *file = fopen("hangman_words.txt", "r");
    if (file == NULL) {
        perror("Error opening hangman_words.txt");
        pthread_mutex_unlock(&word_pool_mutex);
        return;
    }

    char line[MAX_WORD_LEN + 2]; // +1 for newline, +1 for null terminator
    word_pool_count = 0;

    while (fgets(line, sizeof(line), file) && word_pool_count < MAX_WORDS) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }
        
        // Basic validation
        if (len >= MIN_WORD_LEN && len <= MAX_WORD_LEN) {
            strncpy(word_pool[word_pool_count], line, MAX_WORD_LEN);
            word_pool[word_pool_count][MAX_WORD_LEN] = '\0';
            word_pool_count++;
        } else if (len > 0) {
            fprintf(stderr, "Warning: Word '%s' ignored (length %zu).\n", line, len);
        }
    }
    
    fclose(file);
    printf("Loaded %d words from hangman_words.txt.\n", word_pool_count);
    pthread_mutex_unlock(&word_pool_mutex);
}

/**
 * Sends a string message using the Server Message Header (Msg Flag > 0).
 */
void send_message_packet(int fd, const char *message) {
    uint8_t msg_len = (uint8_t)strlen(message);
    
    // Total size = 1 byte (Msg Flag) + msg_len bytes (Data)
    size_t packet_size = sizeof(uint8_t) + msg_len;
    
    uint8_t buffer[packet_size];
    
    buffer[0] = msg_len; // Msg Flag
    memcpy(buffer + 1, message, msg_len); // Data
    
    send(fd, buffer, packet_size, 0);
}

/**
 * Sends the current game state using the Game Control Header (Msg Flag = 0).
 */
void send_game_control_packet(int fd, GameSession *session) {
    uint8_t word_len = (uint8_t)strlen(session->secret_word);
    uint8_t num_incorrect = (uint8_t)session->incorrect_count;
    
    // Data size = Word Length bytes + Num Incorrect bytes
    size_t data_size = word_len + num_incorrect;
    
    // Total packet size = 3 bytes (Headers) + data_size
    size_t packet_size = 3 + data_size;
    
    uint8_t buffer[packet_size];
    
    // Header
    buffer[0] = 0; // Msg Flag
    buffer[1] = word_len; // Word Length
    buffer[2] = num_incorrect; // Num Incorrect
    
    // Data: Display Word
    memcpy(buffer + 3, session->display_word, word_len);
    
    // Data: Incorrect Guesses
    memcpy(buffer + 3 + word_len, session->incorrect_guesses, num_incorrect);
    
    send(fd, buffer, packet_size, 0);
}

/**
 * Processes a guess for a specific session.
 * Returns 1 if the game is over (win/loss), 0 otherwise.
 */
int process_guess(GameSession *session, char guess) {
    int is_correct = 0;
    int len = strlen(session->secret_word);
    
    // 1. Check if the guess is correct (and update the display word)
    for (int i = 0; i < len; i++) {
        // Check if the secret letter matches the guess AND it hasn't been revealed yet
        if (session->secret_word[i] == guess && session->display_word[i] == '_') {
            session->display_word[i] = guess;
            is_correct = 1;
        }
    }

    if (!is_correct) {
        // 2. The guess was incorrect. Check if it's a NEW incorrect guess.
        
        // If the letter is NOT already in the incorrect_guesses string:
        if (strchr(session->incorrect_guesses, guess) == NULL) {
            // New incorrect guess
            session->incorrect_guesses[session->incorrect_count] = guess;
            session->incorrect_count++;
            session->incorrect_guesses[session->incorrect_count] = '\0';
        }
        // If it was already guessed incorrectly, we do nothing (no penalty, no update).
    }

    // 3. Check Win/Loss conditions
    int won = (strchr(session->display_word, '_') == NULL); // No more underscores left
    int lost = (session->incorrect_count >= MAX_INCORRECT); // 6 or more incorrect guesses

    return won || lost;
}

// =================================================================
//                         CLIENT HANDLER THREAD
// =================================================================

void *handle_client_game(void *arg) {
    GameSession *session = (GameSession *)arg;
    int client_fd = session->client_socket;
    
    printf("[Slot %d] New game handler started.\n", session->slot_index);
    
    // 1. Setup New Game
    if (word_pool_count == 0) {
        send_message_packet(client_fd, "Server error: No words loaded. Closing connection.");
        goto cleanup;
    }
    
    // Choose word randomly
    pthread_mutex_lock(&word_pool_mutex);
    int random_index = rand() % word_pool_count;
    strcpy(session->secret_word, word_pool[random_index]);
    pthread_mutex_unlock(&word_pool_mutex);

    int word_len = strlen(session->secret_word);
    
    // Initialize state
    memset(session->display_word, '_', word_len);
    session->display_word[word_len] = '\0';
    session->incorrect_guesses[0] = '\0';
    session->incorrect_count = 0;

    // 2. Wait for Client's Start Signal (ClientPacket with msg_length = 0)
    ClientPacket client_start_pkt;
    ssize_t bytes_recvd = recv(client_fd, &client_start_pkt, sizeof(uint8_t), MSG_WAITALL);
    
    if (bytes_recvd <= 0 || client_start_pkt.msg_length != 0) {
        printf("[Slot %d] Client disconnected or failed to send start signal.\n", session->slot_index);
        goto cleanup;
    }

    // 3. Send Initial Game State
    send_game_control_packet(client_fd, session);
    
    // 4. Main Game Loop
    int game_over = 0;
    while (!game_over) {
        ClientPacket guess_pkt;
        
        // Read the 1-byte header (msg_length)
        bytes_recvd = recv(client_fd, &guess_pkt, sizeof(uint8_t), MSG_WAITALL); 
        if (bytes_recvd <= 0) break; // Client disconnected
        
        // Read the 1 byte of data (the guess)
        bytes_recvd = recv(client_fd, guess_pkt.data, guess_pkt.msg_length, MSG_WAITALL);
        if (bytes_recvd <= 0) break;
        
        char guess = tolower(guess_pkt.data[0]);

        // 5. Process Guess and check game over
        game_over = process_guess(session, guess);

        // 6. Send Response
        if (game_over) {
            int won = (strchr(session->display_word, '_') == NULL);
            
            // Print the final word state before announcing win/loss
            char final_word_msg[30];
            snprintf(final_word_msg, sizeof(final_word_msg), "The word was %s", session->secret_word);
            send_message_packet(client_fd, final_word_msg);
            
            send_message_packet(client_fd, won ? "You Win!" : "You Lose :(");
            send_message_packet(client_fd, "Game Over!");
        } else {
            send_game_control_packet(client_fd, session);
        }
    }

cleanup:
    // 7. Cleanup and Free Slot
    close(client_fd);
    pthread_mutex_lock(&clients_mutex);
    session->is_active = 0;
    pthread_mutex_unlock(&clients_mutex);
    printf("[Slot %d] Connection closed. Slot freed.\n", session->slot_index);
    pthread_exit(NULL);
}


// =================================================================
//                                MAIN
// =================================================================

int main(int argc, char *argv[]) {
    // 1. Setup
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    int port = atoi(argv[1]);
    srand(time(NULL)); 
    load_words();
    if (word_pool_count == 0) {
         fprintf(stderr, "Fatal Error: No valid words loaded from hangman_words.txt. Exiting.\n");
         exit(EXIT_FAILURE);
    }

    // Initialize session slots
    for (int i = 0; i < MAX_CLIENTS; i++) {
        game_sessions[i].is_active = 0;
        game_sessions[i].slot_index = i;
    }

    // 2. Create and Bind Socket
    int listen_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // Allow reuse of port immediately after closing
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    
    if (listen(listen_fd, 5) < 0) {
        perror("listen failed");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    
    printf("Hangman Server listening on port %d...\n", port);

    // 3. Main Server Loop (Accept Connections)
    while (1) {
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept failed");
            continue;
        }
        
        printf("Connection received from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        // Find an available slot
        int slot_index = -1;
        pthread_mutex_lock(&clients_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (game_sessions[i].is_active == 0) {
                slot_index = i;
                break;
            }
        }
        pthread_mutex_unlock(&clients_mutex);

        if (slot_index == -1) {
            // Server Overloaded
            send_message_packet(client_fd, "server-overloaded");
            close(client_fd);
            printf("Connection rejected: Server overloaded.\n");
        } else {
            // Start a new game thread
            GameSession *session = &game_sessions[slot_index];
            session->client_socket = client_fd;
            session->is_active = 1;
            
            if (pthread_create(&session->thread_id, NULL, handle_client_game, session) != 0) {
                perror("pthread_create failed");
                close(client_fd);
                session->is_active = 0;
            }
            pthread_detach(session->thread_id); // Detach thread to clean resources automatically
        }
    }
    
    close(listen_fd);
    return 0;
}