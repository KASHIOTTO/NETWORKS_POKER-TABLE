#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>

#include "poker_client.h"
#include "client_action_handler.h"
#include "game_logic.h"

#define BASE_PORT 2201
#define NUM_PORTS 6
#define BUFFER_SIZE 1024

typedef struct {
    int socket;
    struct sockaddr_in address;
} player_t;

game_state_t game; //global variable to store our game state info (this is a huge hint for you)

int main(int argc, char **argv) {
    int server_fds[NUM_PORTS], client_socket, player_count = 0;
    int opt = 1;
    struct sockaddr_in server_address;
    player_t players[MAX_PLAYERS];
    char buffer[BUFFER_SIZE] = {0};
    socklen_t addrlen = sizeof(struct sockaddr_in);

    int rand_seed = argc == 2 ? atoi(argv[1]) : 0;
    init_game_state(&game, 100, rand_seed);

    for(int i = 0; i < NUM_PORTS; i++){
        server_fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(server_fds[i], SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        memset(&players[i].address, 0, sizeof(players[i].address));
        players[i].address.sin_family = AF_INET;
        players[i].address.sin_addr.s_addr = INADDR_ANY;
        players[i].address.sin_port = htons(BASE_PORT + i);

        if(bind(server_fds[i], (struct sockaddr *)&players[i].address, sizeof(players[i].address)) < 0){
            perror("bind");
            exit(EXIT_FAILURE);
        }
        if(listen(server_fds[i], 1) < 0){
            perror("listen");
            exit(EXIT_FAILURE);
        }
    }
    printf("[Server] Listening on ports 2201-2206. Waiting for JOIN...\n");

    while(player_count < MAX_PLAYERS){
        fd_set readset;
        FD_ZERO(&readset);
        for(int i = 0; i < NUM_PORTS; i++){
            FD_SET(server_fds[i], &readset);
        }
        select(server_fds[NUM_PORTS - 1] + 1, &readset, NULL, NULL, NULL);

        for(int i = 0; i < NUM_PORTS; i++){
            if(FD_ISSET(server_fds[i], &readset)){
                int client_sock = accept(server_fds[i], (struct sockaddr *)&players[i].address, &addrlen);
                if(client_sock < 0){
                    perror("accept");
                    continue;
                }

                client_packet_t joinPkt;
                recv(client_sock, &joinPkt, sizeof(joinPkt), 0);
                if(joinPkt.packet_type != JOIN){
                    close(client_sock);
                    continue;
                }

                game.sockets[i] = client_sock;
                game.player_status[i] = PLAYER_ACTIVE;
                printf(" [Server] Player %d joined on port %d\n", i, BASE_PORT + i);
                ++player_count;
            }
        }
    }
    printf("[Server] All 6 players joined. Begin.\n");

    game.dealer_player = 0;
    while (1) {
        reset_game_state(&game);

        //READY SEQUENCE
        for(int i = 0; i < MAX_PLAYERS; i++){
            client_packet_t pkt;
            if(recv(game.sockets[i], &pkt, sizeof(pkt), 0) <= 0){
                game.player_status[i] = PLAYER_LEFT;
                continue;
            }
            if(pkt.packet_type == LEAVE){
                game.player_status[i] = PLAYER_LEFT;
                close(game.sockets[i]);
                printf("[INFO] [SERVER] Player %d left.\n", i);
            }
        }
        if(server_ready(&game) < 2){
            server_packet_t haltPkt = { .packet_type = HALT };
            for(int i = 0; i < MAX_PLAYERS; i++){
                if(game.player_status[i] != PLAYER_LEFT){
                    send(game.sockets[i], &haltPkt, sizeof(haltPkt), 0);
                    close(game.sockets[i]);
                }
            }
            break;
        }   

        //DEAL TO PLAYERS
        server_deal(&game);
        int initial_active_count = server_ready(&game);
        int action_count = 0;
        int stage_start = 1;

        // PREFLOP BETTING // FLOP BETTING // TURN BETTING // RIVER BETTING
        while(1){
            if(stage_start){
                for(int p = 0; p < MAX_PLAYERS; p++){
                    if(game.player_status[p] != PLAYER_LEFT){
                        server_packet_t infoPkt;
                        build_info_packet(&game, p, &infoPkt);
                        send(game.sockets[p], &infoPkt, sizeof(infoPkt), 0);
                    }
                }
                stage_start = 0;
            }

            player_id_t pid = game.current_player;
            client_packet_t cpkt;
            int r = recv(game.sockets[pid], &cpkt, sizeof(cpkt), 0);
            if(r <= 0){
                game.player_status[pid] = PLAYER_FOLDED;
                int next = (pid + 1) % MAX_PLAYERS;
                while(game.player_status[next] != PLAYER_ACTIVE){
                    next = (next + 1) % MAX_PLAYERS;
                }
                game.current_player = next;
                continue;
            }

            server_packet_t reply;
            if(handle_client_action(&game, pid, &cpkt, &reply) == 0){
                send(game.sockets[pid], &reply, sizeof(reply), 0);
                action_count++;
                for(int p = 0; p < MAX_PLAYERS; p++){
                    if(game.player_status[p] != PLAYER_LEFT){
                        server_packet_t updatedInfoPkt;
                        build_info_packet(&game, p, &updatedInfoPkt);
                        send(game.sockets[p], &updatedInfoPkt, sizeof(updatedInfoPkt), 0);
                    }
                }
                if((game.highest_bet != 0 && check_betting_end(&game)) || (game.highest_bet == 0 && (server_ready(&game) < 2 || action_count >= initial_active_count))){
                    if(game.round_stage == ROUND_RIVER){
                        break;
                    }
                    else{
                        server_community(&game);
                        initial_active_count = server_ready(&game);
                        action_count = 0;
                        stage_start = 1;
                        continue;
                    }
                }
            }
            else{
                reply.packet_type = NACK;
                send(game.sockets[pid], &reply, sizeof(reply), 0);
            }
        }

        int winner = find_winner(&game);
        server_end(&game);

        for(int p = 0; p < MAX_PLAYERS; p++){
            if(game.player_status[p] != PLAYER_LEFT){
                server_packet_t endPkt;
                build_end_packet(&game, winner, &endPkt);
                send(game.sockets[p], &endPkt, sizeof(endPkt), 0);
            }
        }
    }

    printf("[Server] Shutting down.\n");

    for (int i = 0; i < MAX_PLAYERS; i++) {
        close(server_fds[i]);
        if (game.player_status[i] != PLAYER_LEFT) {
            close(game.sockets[i]);
        }
    }

    return 0;
}
