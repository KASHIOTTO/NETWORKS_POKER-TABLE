// client_action_handler.c
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "client_action_handler.h"
#include "game_logic.h"

static void save_state(game_state_t *game, info_packet_t *pack)
{
    memcpy(pack->community_cards, game->community_cards, sizeof(pack->community_cards));
    memcpy(pack->player_stacks, game->player_stacks, sizeof(pack->player_stacks));
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        int b = game->current_bets[i];
        pack->player_bets[i] = b < 0 ? 0 : b;
        switch (game->player_status[i])
        {
        case PLAYER_ACTIVE:
            pack->player_status[i] = 1;
            break;
        case PLAYER_FOLDED:
            pack->player_status[i] = 0;
            break;
        default:
            pack->player_status[i] = 2;
            break;
        }
    }
    pack->bet_size = game->highest_bet;
    pack->pot_size = game->pot_size;
    pack->dealer = game->dealer_player;
    pack->player_turn = game->current_player;
}

int handle_client_action(game_state_t *game, player_id_t pid,
                         const client_packet_t *in, server_packet_t *out)
{
    // Only process if it's this player's turn and they are active in the hand
    if (pid != game->current_player || game->player_status[pid] != PLAYER_ACTIVE)
    {
        return -1; // out-of-turn or inactive player
    }

    // Compute the call amount (difference between current highest bet and this player's bet)
    int playerBet = (game->current_bets[pid] < 0 ? 0 : game->current_bets[pid]); // treat -1 as 0
    int callAmnt = game->highest_bet - playerBet;

    switch (in->packet_type)
    {
    case CHECK:
        if (callAmnt != 0)
        {
            // Cannot check if there's an outstanding bet to call
            return -1;
        }
        // Player checks: no chips added. Mark their bet as 0 (if it was -1).
        if (game->current_bets[pid] < 0)
        {
            game->current_bets[pid] = 0;
        }
        break;

    case CALL:
        if (callAmnt == 0)
        {
            // Nothing to call (should have checked instead)
            return -1;
        }
        if (game->player_stacks[pid] <= callAmnt)
        {
            // Player goes all-in (not enough chips to fully call)
            game->pot_size += game->player_stacks[pid];
            game->current_bets[pid] += game->player_stacks[pid];
            game->player_stacks[pid] = 0;
            game->player_status[pid] = PLAYER_ALLIN;
        }
        else
        {
            // Pay the call amount
            game->player_stacks[pid] -= callAmnt;
            game->current_bets[pid] += callAmnt;
            game->pot_size += callAmnt;
        }
        break;

    case RAISE:
    {
        int newBet = in->params[0]; // the total amount this player wants to have bet
        if (newBet <= game->highest_bet || newBet <= game->current_bets[pid])
        {
            return -1; // raise must increase the bet beyond current highest
        }
        int addAmount = newBet - game->current_bets[pid]; // additional chips put in by this raise
        if (game->player_stacks[pid] < addAmount)
        {
            return -1; // not enough chips to raise this amount
        }
        // Commit the raise
        game->player_stacks[pid] -= addAmount;
        game->current_bets[pid] = newBet;
        game->highest_bet = newBet;
        game->pot_size += addAmount;
    }
    break;

    case FOLD:
        game->player_status[pid] = PLAYER_FOLDED;
        break;

    default:
        return -1; // unknown action
    }

    // Advance turn to the next player who is still active (skip players who folded or are all-in)
    int next = (game->current_player + 1) % MAX_PLAYERS;
    while (game->player_status[next] != PLAYER_ACTIVE)
    {
        next = (next + 1) % MAX_PLAYERS;
        if (next == game->current_player)
        {
            // We have looped around, meaning no other active player (handled by check_betting_end)
            break;
        }
    }
    game->current_player = next;

    // Prepare ACK response (the server loop will send updated INFO to everyone)
    out->packet_type = ACK;
    return 0;
}

void build_info_packet(game_state_t *game, player_id_t pid, server_packet_t *out)
{
    out->packet_type = INFO;
    save_state(game, &out->info);
    out->info.player_cards[0] = game->player_hands[pid][0];
    out->info.player_cards[1] = game->player_hands[pid][1];
}

void build_end_packet(game_state_t *game, player_id_t winner, server_packet_t *out)
{
    out->packet_type = END;
    memcpy(out->end.community_cards, game->community_cards, sizeof(game->community_cards));
    memcpy(out->end.player_stacks, game->player_stacks, sizeof(game->player_stacks));
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        out->end.player_cards[i][0] = game->player_hands[i][0];
        out->end.player_cards[i][1] = game->player_hands[i][1];
        switch (game->player_status[i])
        {
        case PLAYER_ACTIVE:
            out->end.player_status[i] = 1;
            break;
        case PLAYER_FOLDED:
            out->end.player_status[i] = 0;
            break;
        default:
            out->end.player_status[i] = 2;
            break;
        }
    }
    out->end.pot_size = game->pot_size;
    out->end.dealer = game->dealer_player;
    out->end.winner = winner;
}
