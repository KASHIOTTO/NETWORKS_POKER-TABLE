/********************************************************************
 *  Simple select-loop based poker server (6 seats, one port each)
 *******************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

#include "poker_client.h"
#include "game_logic.h"
#include "client_action_handler.h"

#define BASE_PORT   2201
#define SEATS       6

void fill_info(const game_state_t *g,
               player_id_t viewer,
               info_packet_t *pkt);
               
static int active_ready_seats(const game_state_t *g)
{
    int r = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (g->player_status[i] == PLAYER_ACTIVE)
            ++r;
    return r;
}

int main(int argc, char **argv)
{
    int   seed = (argc == 2) ? atoi(argv[1]) : 0;
    game_state_t G;
    init_game_state(&G, /*stack*/100, seed);

    /* ─────── listen sockets – one per seat ─────── */
    int listen_fd[SEATS];
    for (int seat = 0; seat < SEATS; ++seat)
    {
        listen_fd[seat] = socket(AF_INET, SOCK_STREAM, 0);

        int opt = 1;
        setsockopt(listen_fd[seat], SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

        struct sockaddr_in addr = {
            .sin_family      = AF_INET,
            .sin_addr.s_addr = INADDR_ANY,
            .sin_port        = htons(BASE_PORT + seat)
        };
        bind   (listen_fd[seat], (struct sockaddr*)&addr, sizeof addr);
        listen (listen_fd[seat], 1);
    }
    printf("Server listening on ports %d-%d…\n",
           BASE_PORT, BASE_PORT + SEATS - 1);

    /* ─────── wait for JOIN from all six seats ─────── */
    int joined = 0;
    while (joined < SEATS)
    {
        fd_set rs; FD_ZERO(&rs);
        int maxfd = 0;
        for (int s = 0; s < SEATS; ++s)
        {
            FD_SET(listen_fd[s], &rs);
            if (listen_fd[s] > maxfd) maxfd = listen_fd[s];
        }
        select(maxfd + 1, &rs, NULL, NULL, NULL);

        for (int seat = 0; seat < SEATS; ++seat)
        {
            if (!FD_ISSET(listen_fd[seat], &rs)) continue;

            int csock = accept(listen_fd[seat], NULL, NULL);
            client_packet_t jpkt;
            recv(csock, &jpkt, sizeof jpkt, 0);

            if (jpkt.packet_type != JOIN)
            {
                close(csock);
                continue;
            }
            G.sockets[seat]       = csock;
            G.player_status[seat] = PLAYER_FOLDED; /* waiting for READY */
            ++joined;
            printf("Seat %d joined.\n", seat);
        }
    }
    puts("All players joined – starting hands.");

    /* ─────────────────────────  MAIN GAME LOOP  ───────────────────────── */

    while (1)
    {
        /* reset & wait for READY/LEAVE */
        reset_game_state(&G);

        int ready = 0;
        while (ready < 2)                   /* need at least 2 to keep playing */
        {
            fd_set rs; FD_ZERO(&rs);
            int maxfd = 0;
            for (int s = 0; s < SEATS; ++s)
            {
                if (G.player_status[s] == PLAYER_LEFT) continue;
                FD_SET(G.sockets[s], &rs);
                if (G.sockets[s] > maxfd) maxfd = G.sockets[s];
            }
            select(maxfd + 1, &rs, NULL, NULL, NULL);

            for (int s = 0; s < SEATS; ++s)
            {
                if (G.player_status[s] == PLAYER_LEFT) continue;
                if (!FD_ISSET(G.sockets[s], &rs)) continue;

                client_packet_t pkt;
                if (recv(G.sockets[s], &pkt, sizeof pkt, 0) <= 0)
                {   /* hard disconnect */
                    close(G.sockets[s]);
                    G.player_status[s] = PLAYER_LEFT;
                    continue;
                }

                if (pkt.packet_type == READY)
                {
                    G.player_status[s] = PLAYER_ACTIVE;
                }
                else if (pkt.packet_type == LEAVE)
                {
                    G.player_status[s] = PLAYER_LEFT;
                    close(G.sockets[s]);
                }
            }
            ready = active_ready_seats(&G);

            /* halt if fewer than two seats still playing */
            if (ready < 2)
            {
                server_packet_t hp = { .packet_type = HALT };
                for (int s = 0; s < SEATS; ++s)
                    if (G.player_status[s] != PLAYER_LEFT)
                    {
                        send(G.sockets[s], &hp, sizeof hp, 0);
                        close(G.sockets[s]);
                        G.player_status[s] = PLAYER_LEFT;
                    }
                puts("Not enough players – shutting down.");
                goto shutdown;
            }
        }

        /* ───────  start the hand  ─────── */
        server_deal(&G);

        /* send first INFO to everyone */
        {
            server_packet_t sp;
            sp.packet_type = INFO;
            for (int seat = 0; seat < SEATS; ++seat)
            {
                if (G.player_status[seat] == PLAYER_LEFT) continue;
                fill_info(&G, seat, &sp.info);        /* helper from CAH */
                send(G.sockets[seat], &sp, sizeof sp, 0);
            }
        }

        /* ───────  betting/streets loop  ─────── */
        for (;;)
        {
            int state = check_betting_end(&G);
            if (state == 2)               /* only one player still active */
                break;
            if (state == 1)               /* street finished – deal next */
            {
                if (G.round_stage == ROUND_RIVER)
                    break;                /* river done → showdown */
                server_community(&G);
                continue;                 /* broadcast handled inside */
            }

            /* wait for current player action */
            player_id_t p = G.current_player;
            client_packet_t cp;
            if (recv(G.sockets[p], &cp, sizeof cp, 0) <= 0)
            {   /* drop = auto-fold */
                G.player_status[p] = PLAYER_FOLDED;
                continue;
            }
            server_packet_t reply;
            handle_client_action(&G, p, &cp, &reply);
            send(G.sockets[p], &reply, sizeof reply, 0);
        }

        /* ───────  SHOWDOWN / AWARD POT  ─────── */
        int winner = find_winner(&G);
        server_end(&G);

        server_packet_t ep;
        build_end_packet(&G, winner, &ep);
        for (int s = 0; s < SEATS; ++s)
            if (G.player_status[s] != PLAYER_LEFT)
                send(G.sockets[s], &ep, sizeof ep, 0);
    }

shutdown:
    for (int s = 0; s < SEATS; ++s)
    {
        close(listen_fd[s]);
        if (G.player_status[s] != PLAYER_LEFT)
            close(G.sockets[s]);
    }
    return 0;
}
