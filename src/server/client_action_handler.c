/********************************************************************
 *  Handle a single action packet from a client, update game state
 *  and multicast fresh INFO packets, or return NACK on error.
 *******************************************************************/
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "client_action_handler.h"

/* Packs shared state into an info_packet_t (helper for broadcast) */
void fill_info(const game_state_t *g,
                      player_id_t viewer,
                      info_packet_t *pkt)
{
    memcpy(pkt->community_cards, g->community_cards,
           sizeof pkt->community_cards);
    memcpy(pkt->player_stacks,   g->player_stacks,
           sizeof pkt->player_stacks);

    pkt->pot_size   = g->pot_size;
    pkt->dealer     = g->dealer_player;
    pkt->player_turn= g->current_player;
    pkt->bet_size   = g->highest_bet;

    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        pkt->player_bets[i] = (g->current_bets[i] < 0) ?
                                0 : g->current_bets[i];

        switch (g->player_status[i])
        {
            case PLAYER_ACTIVE: pkt->player_status[i] = 1; break;
            case PLAYER_FOLDED: pkt->player_status[i] = 0; break;
            default:            pkt->player_status[i] = 2; break;
        }
    }

    pkt->player_cards[0] = g->player_hands[viewer][0];
    pkt->player_cards[1] = g->player_hands[viewer][1];
}

static void multicast_info(const game_state_t *g)
{
    server_packet_t sp;
    sp.packet_type = INFO;

    for (int seat = 0; seat < MAX_PLAYERS; ++seat)
    {
        if (g->player_status[seat] == PLAYER_LEFT)
            continue;

        fill_info(g, seat, &sp.info);
        send(g->sockets[seat], &sp, sizeof sp, 0);
    }
}

int handle_client_action(game_state_t *g, player_id_t pid,
                         const client_packet_t *in,
                         server_packet_t *reply /* ← ACK/NACK only */)
{
    /* sanity / turn checks */
    if (pid != g->current_player ||
        g->player_status[pid] != PLAYER_ACTIVE)
        goto nack;

    int call_amt = (g->current_bets[pid] < 0)
                     ? g->highest_bet
                     : g->highest_bet - g->current_bets[pid];

    switch (in->packet_type)
    {
        case CHECK:
            if (call_amt != 0) goto nack;      /* cannot check if bet pending */
            break;

        case CALL:
            if (call_amt == 0) goto nack;      /* nothing to call            */

            if (g->player_stacks[pid] <= call_amt)   /* all-in counts as call  */
            {
                g->pot_size          += g->player_stacks[pid];
                g->current_bets[pid] += g->player_stacks[pid];
                g->player_stacks[pid] = 0;
                g->player_status[pid] = PLAYER_ALLIN;
            }
            else
            {
                g->player_stacks[pid] -= call_amt;
                g->current_bets[pid]  += call_amt;
                g->pot_size           += call_amt;
            }
            break;

        case RAISE:
        {
            int new_amt = in->params[0];
            if (new_amt <= g->highest_bet ||
                new_amt <= g->current_bets[pid]) goto nack;

            int diff = new_amt - g->current_bets[pid];
            if (g->player_stacks[pid] < diff)   goto nack;

            g->player_stacks[pid] -= diff;
            g->current_bets[pid]   = new_amt;
            g->highest_bet         = new_amt;
            g->pot_size           += diff;

            /* every other active player must act again */
            for (int i = 0; i < MAX_PLAYERS; ++i)
                if (i != pid && g->player_status[i] == PLAYER_ACTIVE)
                    g->current_bets[i] = -1;
            break;
        }

        case FOLD:
            g->player_status[pid] = PLAYER_FOLDED;
            g->current_bets[pid]  = 0;
            break;

        default: goto nack;
    }

    /* mark that this player has at least matched current bet */
    if (g->player_status[pid] == PLAYER_ACTIVE ||
        g->player_status[pid] == PLAYER_ALLIN)
        g->current_bets[pid] = g->highest_bet;

    /* advance turn */
    int next = (pid + 1) % MAX_PLAYERS;
    while (g->player_status[next] != PLAYER_ACTIVE)
    {
        next = (next + 1) % MAX_PLAYERS;
        if (next == pid) break;        /* could be only 0/1 active left */
    }
    g->current_player = next;

    /* ACK then broadcast fresh state */
    reply->packet_type = ACK;
    multicast_info(g);
    return 0;

nack:
    reply->packet_type = NACK;
    return -1;
}

/* build_end_packet – used once per hand */
void build_end_packet(game_state_t *g, player_id_t winner,
                      server_packet_t *out)
{
    out->packet_type = END;
    end_packet_t *ep = &out->end;

    memcpy(ep->community_cards, g->community_cards,
           sizeof ep->community_cards);
    memcpy(ep->player_stacks,   g->player_stacks,
           sizeof ep->player_stacks);

    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        ep->player_cards[i][0] = g->player_hands[i][0];
        ep->player_cards[i][1] = g->player_hands[i][1];

        switch (g->player_status[i])
        {
            case PLAYER_ACTIVE:
            case PLAYER_ALLIN:  ep->player_status[i] = 1; break;
            case PLAYER_FOLDED: ep->player_status[i] = 0; break;
            default:            ep->player_status[i] = 2; break;
        }
    }

    ep->pot_size = g->pot_size;
    ep->dealer   = g->dealer_player;
    ep->winner   = winner;
}
