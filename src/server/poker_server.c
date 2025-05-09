// poker_server.c
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

game_state_t game;

int main(int argc, char **argv) {
    int server_fds[NUM_PORTS], player_count = 0;
    int opt = 1;
    player_t players[NUM_PORTS];
    socklen_t addrlen = sizeof(struct sockaddr_in);

    init_game_state(&game, 100, argc == 2 ? atoi(argv[1]) : 0);

    for(int i = 0; i < NUM_PORTS; ++i){
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
    printf("[Server] Listening on ports %d-%d. Waiting for JOIN...\n", BASE_PORT, BASE_PORT + NUM_PORTS - 1);

    while(player_count < MAX_PLAYERS){
        fd_set readset;
        FD_ZERO(&readset);
        for(int i = 0; i < NUM_PORTS; i++){
            FD_SET(server_fds[i], &readset);
        }
        select(server_fds[NUM_PORTS-1] + 1, &readset, NULL, NULL, NULL);
        for(int i = 0; i < NUM_PORTS; i++){
            if(FD_ISSET(server_fds[i], &readset)){
                int client_sock = accept(server_fds[i], (struct sockaddr *)&players[i].address, &addrlen);
                if(client_sock < 0){ 
                    perror("accept"); continue; 
                }
                client_packet_t joinPkt;
                recv(client_sock, &joinPkt, sizeof(joinPkt), 0);
                if(joinPkt.packet_type != JOIN){
                    close(client_sock);
                    continue;
                }
                game.sockets[i] = client_sock;
                game.player_status[i] = PLAYER_ACTIVE;
                printf(" [Server] Player %d joined on %d\n", i, BASE_PORT + i);
                ++player_count;
            }
        }
    }
    printf("[Server] All 6 players joined. Entering main loop.\n");

    while(1){
        reset_game_state(&game);

        int readyCount = 0;
        for(int i = 0; i < MAX_PLAYERS; i++){
            if(game.player_status[i] != PLAYER_LEFT){
                client_packet_t pkt;
                if(recv(game.sockets[i], &pkt, sizeof(pkt), 0) <= 0){
                    game.player_status[i] = PLAYER_LEFT;
                    close(game.sockets[i]);
                    continue;
                }
                if(pkt.packet_type == LEAVE){
                    game.player_status[i] = PLAYER_LEFT;
                    close(game.sockets[i]);
                } 
                else if(pkt.packet_type == READY){
                    ++readyCount;
                }
            }
        }
        if(readyCount < 2){
            server_packet_t haltPkt = { .packet_type = HALT };
            for(int i = 0; i < MAX_PLAYERS; i++){
                if(game.player_status[i] != PLAYER_LEFT){
                    send(game.sockets[i], &haltPkt, sizeof(haltPkt), 0);
                    close(game.sockets[i]);
                }
            }
            break;
        }

        server_deal(&game);
        for(int p = 0; p < MAX_PLAYERS; p++){
            if(game.player_status[p] != PLAYER_LEFT){
                server_packet_t infoPkt;
                build_info_packet(&game, p, &infoPkt);
                send(game.sockets[p], &infoPkt, sizeof(infoPkt), 0);
            }
        }

        while(1){
            if(check_betting_end(&game)){
                if(game.round_stage == ROUND_RIVER) break;
                server_community(&game);
                for(int p = 0; p < MAX_PLAYERS; p++){
                    if(game.player_status[p] != PLAYER_LEFT){
                        server_packet_t infoPkt;
                        build_info_packet(&game, p, &infoPkt);
                        send(game.sockets[p], &infoPkt, sizeof(infoPkt), 0);
                    }
                }
                continue;
            }
            player_id_t pid = game.current_player;
            client_packet_t inPkt;
            int r = recv(game.sockets[pid], &inPkt, sizeof(inPkt), 0);
            if(r <= 0){
                game.player_status[pid] = PLAYER_FOLDED;
                int nxt = (pid + 1) % MAX_PLAYERS;
                while(game.player_status[nxt] != PLAYER_ACTIVE){
                    nxt = (nxt + 1) % MAX_PLAYERS;
                }
                game.current_player = nxt;
                continue;
            }
            server_packet_t reply;
            if(handle_client_action(&game, pid, &inPkt, &reply) == 0){
                send(game.sockets[pid], &reply, sizeof(reply), 0);
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
    return 0;
}
