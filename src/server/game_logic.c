#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdint.h>

#include "poker_client.h"
#include "client_action_handler.h"
#include "game_logic.h"

static inline uint64_t bit(int rank){
    return 1ULL << rank;
}

static uint64_t value_of_hand(const card_t cards[7]){
    int rankOccurrence[13] = {0};
    uint16_t suitRanks[4] = {0};
    for(int i = 0; i < 7; ++i){
        int r = RANK(cards[i]);
        int s = SUITE(cards[i]);
        rankOccurrence[r]++;
        suitRanks[s] |= 1 << r;
    }

    int flushSuit = -1;
    for(int s = 0; s < 4; ++s){
        if(__builtin_popcount(suitRanks[s]) >= 5){
            flushSuit = s;
            break;
        }
    }

    int straightHi = -1, sfHi = -1;
    uint16_t ranksMask = 0;
    for(int r = 0; r < 13; ++r){
        if(rankOccurrence[r]){
            ranksMask |= 1 << r;
        }
    }

    if(ranksMask & bit(12)){
        ranksMask |= 1;
    }

    for(int hi = 12; hi >= 4; --hi){
        if((ranksMask >> (hi - 4)) & 0x1F){
            straightHi = hi;
            break;
        }
    }

    if(flushSuit != -1){
        uint16_t fm = suitRanks[flushSuit];
        if(fm & bit(12)){
            fm |= 1;
        }
        for(int hi = 12; hi >= 4; --hi){
            if((fm >> (hi - 4)) & 0x1F){
                sfHi = hi;
                break;
            }
        }
    }

    enum{
        HICARD = 1,
        PAIR,
        TWOPAIR,
        TRIPS,
        STRAIGHT,
        FLUSH,
        FULL,
        QUADS,
        STRFLUSH
    };
    uint64_t best = 0;

    if(sfHi != -1){
        best = ((uint64_t)STRFLUSH << 60) | sfHi;
        return best;
    }

    int quad = -1, trips[3] = {-1, -1, -1}, tCount = 0, pairs[3] = {-1, -1, -1}, pCount = 0;
    for(int r = 12; r >= 0; --r){
        if(rankOccurrence[r] == 4){
            quad = r;
        }
        else if(rankOccurrence[r] == 3){
            trips[tCount++] = r;
        }
        else if(rankOccurrence[r] == 2){
            pairs[pCount++] = r;
        }
    }
    if(quad != -1){
        int fifthHi = -1;
        for(int r = 12; r >= 0; --r){
            if(r != quad && rankOccurrence[r]){
                fifthHi = r;
                break;
            }
        }
        best = ((uint64_t)QUADS << 60) | (quad << 4) | fifthHi;
        return best;
    }
    if(trips[0] != -1 && (pairs[0] != -1 || trips[1] != -1)){
        int three = trips[0];
        int two = (trips[1] != -1) ? trips[1] : pairs[0];
        best = ((uint64_t)FULL << 60) | (three << 4) | two;
        return best;
    }

    if(flushSuit != -1){
        uint16_t fm = suitRanks[flushSuit];
        uint64_t val = 0;
        int filled = 0;
        for(int r = 12; r >= 0 && filled < 5; --r){
            if(fm & bit(r)){
                val = (val << 4) | r;
                ++filled;
            }
        }
        best = ((uint64_t)FLUSH << 60) | val;
        return best;
    }
    if(straightHi != -1){
        best = ((uint64_t)STRAIGHT << 60) | straightHi;
        return best;
    }
    if(trips[0] != -1){
        int fifthHi1 = -1, fifthHi2 = -1;
        for(int r = 12; r >= 0; ++r){
            if(r != trips[0] && rankOccurrence[r]){
                if(fifthHi1 == -1){
                    fifthHi1 = r;
                }
                else{
                    fifthHi2 = r;
                    break;
                }
            }
        }
        best = ((uint64_t)TRIPS << 60) | (trips[0] << 8) | (fifthHi1 << 4) | fifthHi2;
        return best;
    }
    if(pCount >= 2){
        int hi = pairs[0], lo = pairs[1], fifthHi = -1;
        for(int r = 12; r >=0; --r){
            if(rankOccurrence[r] && r != hi && r != lo){
                fifthHi = r;
                break;
            }
        }
        best = ((uint64_t)TWOPAIR << 60) | (hi << 8) | (lo << 4) | fifthHi;
        return best;
    }
    if(pCount == 1){
        int fifthHi1 = -1, fifthHi2 = -1, fifthHi3 = -1;
        for(int r = 12; r >= 0; --r){
            if(rankOccurrence[r] && r != pairs[0]){
                if(fifthHi1 == -1){
                    fifthHi1 = r;
                }
                else if(fifthHi2 == -1){
                    fifthHi2 = r;
                }
                else{
                    fifthHi3 = r;
                    break;
                }
            }
        }
        best = ((uint64_t)PAIR << 60) | (pairs[0] << 12) | (fifthHi1 << 8) | (fifthHi2 << 4) | fifthHi3;
        return best;
    }
    {
        uint64_t val = 0;
        int filled = 0;
        for(int r = 12; r >= 0 && filled < 5; --r){
            if(rankOccurrence[r]){
                val = (val << 4) | r;
                ++filled;
            }
        }
        best = ((uint64_t)HICARD << 60) | val;
    }
    return best;
}

void print_game_state( game_state_t *game){
    (void) game;
}

void init_deck(card_t deck[DECK_SIZE], int seed){
    srand(seed);
    int i = 0;
    for(int r = 0; r<13; r++){
        for(int s = 0; s<4; s++){
            deck[i++] = (r << SUITE_BITS) | s;
        }
    }
}

void shuffle_deck(card_t deck[DECK_SIZE]){
    for(int i = 0; i<DECK_SIZE; i++){
        int j = rand() % DECK_SIZE;
        card_t temp = deck[i];
        deck[i] = deck[j];
        deck[j] = temp;
    }
}

void init_game_state(game_state_t *game, int starting_stack, int random_seed){
    memset(game, 0, sizeof(game_state_t));
    init_deck(game->deck, random_seed);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        game->player_stacks[i] = starting_stack;
    }

    game->dealer_player = 0;
    game->current_player = 1;
    game->round_stage = ROUND_INIT;
}

void reset_game_state(game_state_t *game) {
    shuffle_deck(game->deck);
    game->next_card = 0;
    memset(game->community_cards, NOCARD, sizeof(game->community_cards));
    memset(game->player_hands, NOCARD, sizeof(game->player_hands));
    memset(game->current_bets, 0, sizeof(game->current_bets));
    game->highest_bet = 0;
    game->pot_size = 0;

    if(game->round_stage != ROUND_INIT){
        int next_dealer = (game->dealer_player + 1) % MAX_PLAYERS;
        while(game->player_status[next_dealer] == PLAYER_LEFT){
            next_dealer = (next_dealer + 1) % MAX_PLAYERS;
        }
        game->dealer_player = next_dealer;
    }

    game->current_player = -1;
    game->round_stage = ROUND_PREFLOP;
}

void server_join(game_state_t *game) {
    (void) game;
}

int server_ready(game_state_t *game) {
    int ready = 0;
    for(int i = 0; i < MAX_PLAYERS; i++){
        if(game->player_status[i] == PLAYER_ACTIVE){
            ++ready;
        }
    }
    return ready;
}

void server_deal(game_state_t *game) {
    int first = (game->dealer_player + 1) % MAX_PLAYERS;
    while(game->player_status[first] != PLAYER_ACTIVE){
        first = (first + 1) % MAX_PLAYERS;
    }
    game->current_player = first;

    for(int i = 0; i < MAX_PLAYERS; ++i){
        if(game->player_status[i] == PLAYER_ACTIVE){
            game->current_bets[i] = -1;
        }
    }
    for(int i = game->dealer_player, dealt = 0; dealt < MAX_PLAYERS; i = (i + 1) % MAX_PLAYERS, ++dealt){
        if (game->player_status[i] != PLAYER_ACTIVE) {
            continue;
        }
        game->player_hands[i][0] = game->deck[game->next_card++];
        game->player_hands[i][1] = game->deck[game->next_card++];
    }
    game->highest_bet = 0;
}

int server_bet(game_state_t *game) {
    (void) game;
    return 0;
}

int check_betting_end(game_state_t *game){
    int activeCount = 0;
    int match = 1;
    for(int i = 0; i < MAX_PLAYERS; ++i){
        if(game->player_status[i] != PLAYER_ACTIVE && game->player_status[i] != PLAYER_ALLIN){
            continue;
        }
        ++activeCount;
        if(game->player_status[i] == PLAYER_ACTIVE && game->current_bets[i] == -1){
            match = 0;
        }
        if(game->player_status[i] == PLAYER_ACTIVE && game->current_bets[i] != game->highest_bet){
            match = 0;
        }
    }
    return match;
}

void server_community(game_state_t *game) {
    if(game->round_stage == ROUND_PREFLOP){
        for(int i = 0; i < 3; ++i){
            game->community_cards[i] = game->deck[game->next_card++];
        }
        game->round_stage = ROUND_FLOP;
    }
    else if(game->round_stage == ROUND_FLOP){
        game->community_cards[3] = game->deck[game->next_card++];
        game->round_stage = ROUND_TURN;
    }
    else if(game->round_stage == ROUND_TURN){
        game->community_cards[4] = game->deck[game->next_card++];
        game->round_stage = ROUND_RIVER;
    }

    for(int i = 0; i < MAX_PLAYERS; ++i){
        game->current_bets[i] = (game->player_status[i] == PLAYER_ACTIVE) ? -1 : 0;
    }
    game->highest_bet = 0;

    int p = (game->dealer_player + 1) % MAX_PLAYERS;
    while(game->player_status[p] != PLAYER_ACTIVE){
        p = (p + 1) % MAX_PLAYERS;
    }
    game->current_player = p;
}

void server_end(game_state_t *game) {
    int won = find_winner(game);
    game->player_stacks[won] += game->pot_size;
}

int evaluate_hand(game_state_t *game, player_id_t pid) {
    card_t cards[7] = {
        game->player_hands[pid][0],
        game->player_hands[pid][1],
        game->community_cards[0],
        game->community_cards[1],
        game->community_cards[2],
        game->community_cards[3],
        game->community_cards[4]
    };
    return (int)value_of_hand(cards);
}

int find_winner(game_state_t *game) {
    uint64_t bestHand = 0;
    int bestSeat = -1;
    for(int i = 0; i < MAX_PLAYERS; ++i){
        if(game->player_status[i] == PLAYER_FOLDED || game->player_status[i] == PLAYER_LEFT){
            continue;
        }
        uint64_t value = (uint64_t)evaluate_hand(game, i);
        if(value > bestHand){
            bestHand = value;
            bestSeat = i;
        }
    }
    return bestSeat;
}
