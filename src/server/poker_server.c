#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <stdbool.h>

#include "poker_client.h"
#include "client_action_handler.h"
#include "game_logic.h"

#define BASE_PORT 2201
#define NUM_PORTS 6
#define BUFFER_SIZE 1024

typedef struct
{
    int socket;
    struct sockaddr_in address;
} player_t;

game_state_t game; // global game state

int main(int argc, char **argv)
{
    int server_fds[NUM_PORTS];
    int player_count = 0;
    int opt = 1;
    struct sockaddr_in server_address;
    player_t players[MAX_PLAYERS];
    socklen_t addrlen = sizeof(server_address);

    // Initialize game state and shuffle deck
    int rand_seed = (argc == 2 ? atoi(argv[1]) : 0);
    init_game_state(&game, 100, rand_seed); // sets initial stacks, dealer=0, etc.

    // Setup listening sockets for 6 players on ports 2201-2206
    for (int i = 0; i < NUM_PORTS; ++i)
    {
        server_fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(server_fds[i], SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        memset(&players[i].address, 0, sizeof(players[i].address));
        players[i].address.sin_family = AF_INET;
        players[i].address.sin_addr.s_addr = INADDR_ANY;
        players[i].address.sin_port = htons(BASE_PORT + i);
        if (bind(server_fds[i], (struct sockaddr *)&players[i].address, sizeof(players[i].address)) < 0)
        {
            perror("bind");
            exit(EXIT_FAILURE);
        }
        if (listen(server_fds[i], 1) < 0)
        {
            perror("listen");
            exit(EXIT_FAILURE);
        }
    }
    printf("[Server] Listening on ports %d-%d. Waiting for JOIN...\n", BASE_PORT, BASE_PORT + NUM_PORTS - 1);

    // Accept exactly MAX_PLAYERS (6) connections with JOIN packets
    while (player_count < MAX_PLAYERS)
    {
        fd_set readset;
        FD_ZERO(&readset);
        for (int i = 0; i < NUM_PORTS; ++i)
        {
            FD_SET(server_fds[i], &readset);
        }
        select(server_fds[NUM_PORTS - 1] + 1, &readset, NULL, NULL, NULL);
        for (int i = 0; i < NUM_PORTS; ++i)
        {
            if (FD_ISSET(server_fds[i], &readset))
            {
                int client_sock = accept(server_fds[i], (struct sockaddr *)&players[i].address, &addrlen);
                if (client_sock < 0)
                {
                    perror("accept");
                    continue;
                }
                // Expect a JOIN packet from this connection
                client_packet_t joinPkt;
                recv(client_sock, &joinPkt, sizeof(joinPkt), 0);
                if (joinPkt.packet_type != JOIN)
                {
                    close(client_sock);
                    continue;
                }
                game.sockets[i] = client_sock;
                game.player_status[i] = PLAYER_ACTIVE;
                printf("[Server] Player %d JOINed on port %d\n", i, BASE_PORT + i);
                player_count++;
            }
        }
    }
    printf("[Server] All %d players joined. Entering main loop.\n", MAX_PLAYERS);

    // Set initial dealer and prepare for first round
    game.dealer_player = 0; // start with player 0 as dealer
    bool first_round = true;

    // Main game loop: run hands until fewer than 2 players remain
    while (1)
    {

        // Ready phase: wait for each active player to send READY or handle LEAVE
        for (int i = 0; i < MAX_PLAYERS; ++i)
        {
            if (game.player_status[i] == PLAYER_LEFT)
            {
                continue; // skip players already out
            }
            client_packet_t pkt;
            int bytes = recv(game.sockets[i], &pkt, sizeof(pkt), 0);
            if (bytes <= 0)
            {
                // Disconnected during ready phase
                game.player_status[i] = PLAYER_LEFT;
                close(game.sockets[i]);
                printf("[Server] Player %d disconnected before new round.\n", i);
                continue;
            }
            if (pkt.packet_type == LEAVE)
            {
                game.player_status[i] = PLAYER_LEFT;
                close(game.sockets[i]);
                printf("[Server] Player %d left the table.\n", i);
            }
            // (If packet_type is READY, do nothing special; any non-LEAVE means they're ready)
        }

        // Check if enough players remain to continue
        int readyPlayers = server_ready(&game);
        if (readyPlayers < 2)
        {
            // Not enough players to continue the game
            server_packet_t haltPkt = {.packet_type = HALT};
            for (int i = 0; i < MAX_PLAYERS; ++i)
            {
                if (game.player_status[i] != PLAYER_LEFT)
                {
                    send(game.sockets[i], &haltPkt, sizeof(haltPkt), 0);
                    close(game.sockets[i]);
                }
            }
            break; // exit main loop, shutting down server
        }

        // Initialize new round state
        reset_game_state(&game); // shuffle deck, reset pot/bets, rotate dealer if not first round
        first_round = false;

        // Deal hole cards to each active player
        server_deal(&game);
        // Ensure all non-left players are marked ACTIVE (reset folded status from previous hand)
        for (int i = 0; i < MAX_PLAYERS; ++i)
        {
            if (game.player_status[i] != PLAYER_LEFT)
            {
                game.player_status[i] = PLAYER_ACTIVE;
                game.current_bets[i] = -1; // no bet placed yet this round
            }
        }
        game.highest_bet = 0;
        game.pot_size = 0;
        game.round_stage = ROUND_PREFLOP;

        // === Betting Phases: Preflop, Flop, Turn, River ===
        bool endRound = false;
        while (!endRound)
        {
            // Broadcast state to all players at the start of this betting round
            for (int p = 0; p < MAX_PLAYERS; ++p)
            {
                if (game.player_status[p] != PLAYER_LEFT)
                {
                    server_packet_t infoPkt;
                    build_info_packet(&game, p, &infoPkt);
                    send(game.sockets[p], &infoPkt, sizeof(infoPkt), 0);
                }
            }

            // Betting loop for the current stage (preflop/flop/turn/river)
            while (1)
            {
                // If only one player remains active (others folded), end the round immediately
                if (server_ready(&game) < 2)
                {
                    endRound = true;
                    break;
                }

                player_id_t pid = game.current_player;
                client_packet_t actionPkt;
                int bytes = recv(game.sockets[pid], &actionPkt, sizeof(actionPkt), 0);
                if (bytes <= 0)
                {
                    // Player disconnected in the middle of a hand; treat as if they left/folded
                    printf("[Server] Player %d disconnected during play. Treating as fold.\n", pid);
                    close(game.sockets[pid]);
                    game.player_status[pid] = PLAYER_LEFT;
                    // Remove from turn rotation: find next active player
                    int nxt = (pid + 1) % MAX_PLAYERS;
                    while (game.player_status[nxt] != PLAYER_ACTIVE)
                    {
                        nxt = (nxt + 1) % MAX_PLAYERS;
                        // (We are guaranteed at least one other active by earlier check)
                    }
                    game.current_player = nxt;
                    // Continue to next action without sending ACK (player is gone)
                    continue;
                }
                if (actionPkt.packet_type == LEAVE)
                {
                    // Player voluntarily left during their turn
                    printf("[Server] Player %d left during the hand. Removing from game.\n", pid);
                    game.player_status[pid] = PLAYER_LEFT;
                    close(game.sockets[pid]);
                    // This is effectively a fold for this hand
                    if (server_ready(&game) < 2)
                    {
                        // If this leave leaves only one player, end the round
                        endRound = true;
                        break;
                    }
                    // Otherwise, set next player and continue
                    int nxt = (pid + 1) % MAX_PLAYERS;
                    while (game.player_status[nxt] != PLAYER_ACTIVE)
                    {
                        nxt = (nxt + 1) % MAX_PLAYERS;
                    }
                    game.current_player = nxt;
                    continue;
                }

                // Handle the action packet (CHECK/CALL/RAISE/FOLD) from player `pid`
                server_packet_t response;
                int result = handle_client_action(&game, pid, &actionPkt, &response);
                if (result != 0)
                {
                    // Invalid action (e.g., out-of-turn or not allowed) -> send NACK and retry
                    response.packet_type = NACK;
                    send(game.sockets[pid], &response, sizeof(response), 0);
                    continue; // prompt the same player again (did not advance turn)
                }
                // Valid action -> send ACK to that player
                send(game.sockets[pid], &response, sizeof(response), 0);

                // Update all players with the new state after this action
                for (int p = 0; p < MAX_PLAYERS; ++p)
                {
                    if (game.player_status[p] != PLAYER_LEFT)
                    {
                        server_packet_t infoPkt;
                        build_info_packet(&game, p, &infoPkt);
                        send(game.sockets[p], &infoPkt, sizeof(infoPkt), 0);
                    }
                }

                // Check if this betting round is now complete (everyone called or checked)
                if (check_betting_end(&game))
                {
                    break; // exit the betting loop for this stage
                }
                // Otherwise, continue to next player's action (game.current_player was advanced by handle_client_action)
            } // end betting loop for current stage

            if (endRound)
            {
                // Round ended early (all but one player left or other termination)
                break;
            }

            // If we reach here, betting for the current stage ended normally
            if (game.round_stage == ROUND_RIVER)
            {
                // River betting finished – time for showdown
                endRound = true;
            }
            else
            {
                // Deal next community card(s) and move to the next stage
                server_community(&game); // flop/turn dealt as appropriate, round_stage advanced
                // Continue to the next betting stage (loop again)
            }
        } // end of betting stages loop for this hand

        // **Showdown / Hand Conclusion**
        // If multiple players are still in (active or all-in) and community cards remain, reveal all remaining community cards
        int players_in = 0;
        for (int i = 0; i < MAX_PLAYERS; ++i)
        {
            if (game.player_status[i] == PLAYER_ACTIVE || game.player_status[i] == PLAYER_ALLIN)
            {
                players_in++;
            }
        }
        if (players_in > 1 && game.round_stage != ROUND_RIVER)
        {
            // Reveal all remaining community cards because players have gone all-in or no further betting will happen
            while (game.round_stage != ROUND_RIVER)
            {
                server_community(&game);
            }
        }

        // Determine the winner and distribute pot
        int winner = find_winner(&game);
        server_end(&game); // add pot to winner's stack

        // Send END packet to all players still at the table (with final hands, winner, etc.)
        for (int p = 0; p < MAX_PLAYERS; ++p)
        {
            if (game.player_status[p] != PLAYER_LEFT)
            {
                server_packet_t endPkt;
                build_end_packet(&game, winner, &endPkt);
                send(game.sockets[p], &endPkt, sizeof(endPkt), 0);
                // Mark players who were folded/all-in back to active for next round
                if (game.player_status[p] != PLAYER_ACTIVE)
                {
                    game.player_status[p] = PLAYER_ACTIVE;
                }
            }
        }
        // Loop back for the next hand
    }

    // Cleanup: close listening and any open client sockets
    printf("[Server] Shutting down.\n");
    for (int i = 0; i < NUM_PORTS; ++i)
    {
        close(server_fds[i]);
    }
    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        if (game.player_status[i] != PLAYER_LEFT)
        {
            close(game.sockets[i]);
        }
    }
    return 0;
}
