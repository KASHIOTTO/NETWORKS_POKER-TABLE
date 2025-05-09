#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "client_action_handler.h"
#include "game_logic.h"

static void save_state(game_state_t *game, info_packet_t *pack){
    memcpy(pack->community_cards, game->community_cards, sizeof(pack->community_cards));
    memcpy(pack->player_stacks, game->player_stacks, sizeof(pack->player_stacks));

    for(int i = 0; i < MAX_PLAYERS; ++i){
        int shown_bet = game->current_bets[i];
        pack->player_bets[i] = (shown_bet < 0) ? 0 : shown_bet;

        switch(game->player_status[i]){
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
    pack->dealer    = game->dealer_player;
    pack->player_turn = game->current_player;
}

int handle_client_action(game_state_t *game, player_id_t pid, const client_packet_t *in, server_packet_t *out) {
    if(pid != game->current_player || game->player_status[pid] != PLAYER_ACTIVE){
        return -1;
    }

    int already  = (game->current_bets[pid] < 0) ? 0 : game->current_bets[pid];
    int callAmnt = game->highest_bet - already;

    switch(in->packet_type){
        case CHECK:
            if(callAmnt != 0){
                return -1;
            }
            game->current_bets[pid] = already;
            break;

        case CALL:
            if(callAmnt == 0){
                return -1;
            }
            if(game->player_stacks[pid] <= callAmnt){
                game->pot_size += game->player_stacks[pid];
                game->current_bets[pid] = already + game->player_stacks[pid];
                game->player_stacks[pid] = 0;
                game->player_status[pid] = PLAYER_ALLIN;
            } else {
                game->player_stacks[pid] -= callAmnt;
                game->current_bets[pid] = already + callAmnt;
                game->pot_size += callAmnt;
            }
            break;

        case RAISE:{
            int newAmt = in->params[0];
            if(newAmt <= game->highest_bet || newAmt <= already){
                return -1;
            }
            int diff = newAmt - already;
            if(game->player_stacks[pid] < diff){
                return -1;
            }
            game->player_stacks[pid] -= diff;
            game->current_bets[pid] = newAmt;
            game->highest_bet = newAmt;
            game->pot_size += diff;
            for(int i = 0; i < MAX_PLAYERS; ++i){
                if(i != pid && game->player_status[i] == PLAYER_ACTIVE){
                    game->current_bets[i] = -1;
                }
            }
            } break;

        case FOLD:
            game->player_status[pid] = PLAYER_FOLDED;
            break;

        default:
            return -1;
    }

    if(game->player_status[pid] == PLAYER_ACTIVE || game->player_status[pid] == PLAYER_ALLIN){
        if(game->current_bets[pid] < 0){
            game->current_bets[pid] = game->highest_bet;
        }
    }

    int next = (game->current_player + 1) % MAX_PLAYERS;
    while(game->player_status[next] != PLAYER_ACTIVE){
        next = (next + 1) % MAX_PLAYERS;
    }
    game->current_player = next;

    out->packet_type = ACK;

    player_id_t nxt = game->current_player;
    if(game->player_status[nxt] != PLAYER_LEFT){
        server_packet_t infoPkt;
        build_info_packet(game, nxt, &infoPkt);
        send(game->sockets[nxt], &infoPkt, sizeof(infoPkt), 0);
    }

    return 0;
}

void build_info_packet(game_state_t *game, player_id_t pid, server_packet_t *out) {
    out->packet_type = INFO;
    info_packet_t *pack = &out->info;
    save_state(game, pack);

    for(int i = 0; i < 2; ++i){
        pack->player_cards[i] = game->player_hands[pid][i];
    }
}

void build_end_packet(game_state_t *game, player_id_t winner, server_packet_t *out) {
    out->packet_type = END;
    end_packet_t *epack = &out->end;

    memcpy(epack->community_cards, game->community_cards, sizeof(epack->community_cards));
    memcpy(epack->player_stacks, game->player_stacks, sizeof(epack->player_stacks));

    for(int i = 0; i < MAX_PLAYERS; ++i){
        epack->player_cards[i][0] = game->player_hands[i][0];
        epack->player_cards[i][1] = game->player_hands[i][1];
        switch(game->player_status[i]){
            case PLAYER_ACTIVE:
                epack->player_status[i] = 1;
                break;
            case PLAYER_FOLDED:
                epack->player_status[i] = 0;
                break;
            default:
                epack->player_status[i] = 2;
                break;
        }
    }
    epack->pot_size = game->pot_size;
    epack->dealer   = game->dealer_player;
    epack->winner   = winner;
}
