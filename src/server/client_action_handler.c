// client_action_handler.c
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "client_action_handler.h"
#include "game_logic.h"

static void save_state(game_state_t *game, info_packet_t *pack) {
    memcpy(pack->community_cards, game->community_cards, sizeof(pack->community_cards));
    memcpy(pack->player_stacks, game->player_stacks, sizeof(pack->player_stacks));
    for(int i = 0; i < MAX_PLAYERS; i++){
        int b = game->current_bets[i];
        pack->player_bets[i] = b < 0 ? 0 : b;
        switch(game->player_status[i]){
            case PLAYER_ACTIVE:  
                pack->player_status[i] = 1; 
                break;
            case PLAYER_FOLDED:  
                pack->player_status[i]=0; 
                break;
            default:             
                pack->player_status[i]=2; 
                break;
        }
    }
    pack->bet_size = game->highest_bet;
    pack->pot_size = game->pot_size;
    pack->dealer = game->dealer_player;
    pack->player_turn = game->current_player;
}

int handle_client_action(game_state_t *game, player_id_t pid, const client_packet_t *in, server_packet_t *out) {
    if(pid != game->current_player || game->player_status[pid] != PLAYER_ACTIVE){
        return -1;
    }
    int have = game->current_bets[pid];
    int callAmt = have < 0 ? game->highest_bet : game->highest_bet - have;
    switch(in->packet_type){
        case CHECK:
            if(callAmt!=0){
                return -1;
            }
            break;
        case CALL:
            if(callAmt == 0){
                return -1;
            }
            if(game->player_stacks[pid] <= callAmt){
                game->pot_size += game->player_stacks[pid];
                game->current_bets[pid] += game->player_stacks[pid];
                game->player_stacks[pid] = 0;
                game->player_status[pid] = PLAYER_ALLIN;
            } 
            else{
                game->player_stacks[pid] -= callAmt;
                game->current_bets[pid] += callAmt;
                game->pot_size += callAmt;
            }
            break;
        case RAISE:
            int newAmt = in->params[0];
            if(newAmt <= game->highest_bet || newAmt <= game->current_bets[pid]){
                return -1;
            }
            int diff=newAmt-game->current_bets[pid];
            if(game->player_stacks[pid]<diff){
                return -1;
            }
            game->player_stacks[pid] -= diff;
            game->current_bets[pid] = newAmt;
            game->highest_bet = newAmt;
            game->pot_size += diff;
            for(int i = 0; i < MAX_PLAYERS; i++){
                if(i != pid && game->player_status[i] == PLAYER_ACTIVE){
                    game->current_bets[i] = -1;
                }
            }
            break;
        case FOLD:
            game->player_status[pid] = PLAYER_FOLDED;
            break;
        default:
            return -1;
    }
    if(game->player_status[pid] == PLAYER_ACTIVE || game->player_status[pid] == PLAYER_ALLIN){
        game->current_bets[pid] = game->highest_bet;
    }

    int nxt = (game->current_player+1) % MAX_PLAYERS;
    while(nxt != game->current_player && game->player_status[nxt] != PLAYER_ACTIVE){
	nxt = (nxt + 1) % MAX_PLAYERS;
	if(nxt == game->current_player){break;}
	}
    	game->current_player=nxt;

    out->packet_type = ACK;
    for(int p = 0; p < MAX_PLAYERS; p++){
        if(game->player_status[p] != PLAYER_LEFT){
            server_packet_t infoPkt;
            build_info_packet(game, p, &infoPkt);
            send(game->sockets[p], &infoPkt, sizeof(infoPkt), 0);
        }
    }
    return 0;
}

void build_info_packet(game_state_t *game, player_id_t pid, server_packet_t *out) {
    out->packet_type = INFO;
    save_state(game, &out->info);
    out->info.player_cards[0] = game->player_hands[pid][0];
    out->info.player_cards[1] = game->player_hands[pid][1];
}

void build_end_packet(game_state_t *game, player_id_t winner, server_packet_t *out) {
    out->packet_type = END;
    memcpy(out->end.community_cards, game->community_cards, sizeof(game->community_cards));
    memcpy(out->end.player_stacks,   game->player_stacks,   sizeof(game->player_stacks));
    for(int i = 0; i < MAX_PLAYERS; i++){
        out->end.player_cards[i][0] = game->player_hands[i][0];
        out->end.player_cards[i][1] = game->player_hands[i][1];
        switch(game->player_status[i]){
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
