// game_logic.c
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

static inline uint64_t bit(int rank)
{
    return 1ULL << rank;
}

static uint64_t value_of_hand(const card_t cards[7])
{
    int rankCnt[13] = {0};
    uint16_t suitRanks[4] = {0};
    for (int i = 0; i < 7; i++)
    {
        if (cards[i] == NOCARD)
        {
            continue;
        }
        int r = RANK(cards[i]);
        int s = SUITE(cards[i]);
        rankCnt[r]++;
        suitRanks[s] |= 1 << r;
    }

    int flushSuit = -1;
    for (int s = 0; s < 4; s++)
    {
        if (__builtin_popcount(suitRanks[s]) >= 5)
        {
            flushSuit = s;
            break;
        }
    }

    int straightHi = -1, sfHi = -1;
    uint16_t ranksMask = 0;
    for (int r = 0; r < 13; r++)
    {
        if (rankCnt[r])
        {
            ranksMask |= 1 << r;
        }
    }
    if (ranksMask & bit(12))
    {
        ranksMask |= 1;
    }
    for (int hi = 12; hi >= 4; hi--)
    {
        uint16_t straightMask = 0x1F << (hi - 4);
        if ((ranksMask & straightMask) == straightMask)
        {
            straightHi = hi;
            break;
        }
    }
    if (flushSuit != -1)
    {
        uint16_t fm = suitRanks[flushSuit];
        if (fm & bit(12))
        {
            fm |= 1;
        }
        for (int hi = 12; hi >= 4; hi--)
        {
            uint16_t straightMask = 0x1F << (hi - 4);
            if ((fm & straightMask) == straightMask)
            {
                sfHi = hi;
                break;
            }
        }
    }

    enum
    {
        HICARD = 1,
        ONE_PAIR,
        TWO_PAIR,
        THREE_OF_A_KIND,
        STRAIGHT,
        FLUSH,
        FULL_HOUSE,
        FOUR_OF_A_KIND,
        STR_FLUSH
    };

    uint64_t best = 0;

    if (sfHi != -1)
    {
        return ((uint64_t)STR_FLUSH << 60) | sfHi;
    }

    int quad = -1, trips[3] = {-1, -1, -1}, tCnt = 0, pairs[3] = {-1, -1, -1}, pCnt = 0;
    for (int r = 12; r >= 0; r--)
    {
        if (rankCnt[r] == 4)
        {
            quad = r;
        }
        else if (rankCnt[r] == 3)
        {
            trips[tCnt++] = r;
        }
        else if (rankCnt[r] == 2)
        {
            pairs[pCnt++] = r;
        }
    }
    if (quad != -1)
    {
        int hiCard5 = -1;
        for (int r = 12; r >= 0; r--)
        {
            if (r != quad && rankCnt[r])
            {
                hiCard5 = r;
                break;
            }
        }
        return ((uint64_t)FOUR_OF_A_KIND << 60) | (quad << 4) | hiCard5;
    }
    if (trips[0] != -1 && (pairs[0] != -1 || trips[1] != -1))
    {
        int three = trips[0], two = ((trips[1] != -1) ? trips[1] : pairs[0]);
        return ((uint64_t)FULL_HOUSE << 60) | (three << 4) | two;
    }
    if (flushSuit != -1)
    {
        uint16_t fm = suitRanks[flushSuit];
        uint64_t val = 0;
        int cnt = 0;
        for (int r = 12; r >= 0 && cnt < 5; r--)
        {
            if (fm & bit(r))
            {
                val = (val << 4) | r;
                cnt++;
            }
        }
        return ((uint64_t)FLUSH << 60) | val;
    }
    if (straightHi != -1)
    {
        return ((uint64_t)STRAIGHT << 60) | straightHi;
    }
    if (trips[0] != -1)
    {
        int k1 = -1, k2 = -1;
        for (int r = 12; r >= 0; r--)
        {
            if (r != trips[0] && rankCnt[r])
            {
                if (k1 == -1)
                {
                    k1 = r;
                }
                else
                {
                    k2 = r;
                    break;
                }
            }
        }
        return ((uint64_t)THREE_OF_A_KIND << 60) | (trips[0] << 8) | (k1 << 4) | k2;
    }
    if (pCnt >= 2)
    {
        int hi = pairs[0], lo = pairs[1], k = -1;
        for (int r = 12; r >= 0; r--)
        {
            if (rankCnt[r] && r != hi && r != lo)
            {
                k = r;
                break;
            }
        }
        return ((uint64_t)TWO_PAIR << 60) | (hi << 8) | (lo << 4) | k;
    }
    if (pCnt == 1)
    {
        int k1 = -1, k2 = -1, k3 = -1;
        for (int r = 12; r >= 0; r--)
        {
            if (rankCnt[r] && r != pairs[0])
            {
                if (k1 == -1)
                {
                    k1 = r;
                }
                else if (k2 == -1)
                {
                    k2 = r;
                }
                else
                {
                    k3 = r;
                    break;
                }
            }
        }
        return ((uint64_t)ONE_PAIR << 60) | (pairs[0] << 12) | (k1 << 8) | (k2 << 4) | k3;
    }
    {
        uint64_t val = 0;
        int cnt = 0;
        for (int r = 12; r >= 0 && cnt < 5; r--)
        {
            if (rankCnt[r])
            {
                val = (val << 4) | r;
                cnt++;
            }
        }
        return ((uint64_t)HICARD << 60) | val;
    }
}

void print_game_state(game_state_t *game)
{
    (void)game;
}

void init_deck(card_t deck[DECK_SIZE], int seed)
{
    srand(seed);
    int i = 0;
    for (int r = 0; r < 13; r++)
    {
        for (int s = 0; s < 4; s++)
        {
            deck[i++] = (r << SUITE_BITS) | s;
        }
    }
}

void shuffle_deck(card_t deck[DECK_SIZE])
{
    for (int i = 0; i < DECK_SIZE; i++)
    {
        int j = rand() % DECK_SIZE;
        card_t t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
}

void init_game_state(game_state_t *game, int starting_stack, int random_seed)
{
    memset(game, 0, sizeof(*game));
    init_deck(game->deck, random_seed);
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        game->player_stacks[i] = starting_stack;
    }
    game->dealer_player = 0;
    game->current_player = 1;
    game->round_stage = ROUND_INIT;
}

void reset_game_state(game_state_t *game)
{
    shuffle_deck(game->deck);

    game->next_card = 0;
    memset(game->community_cards, NOCARD, sizeof(game->community_cards));
    memset(game->player_hands, NOCARD, sizeof(game->player_hands));
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        game->current_bets[i] = 0;
    }
    game->highest_bet = 0;
    game->pot_size = 0;
    if (game->round_stage != ROUND_INIT)
    {
        int activeCount = server_ready(game);
        if (activeCount == 0)
        {
            return;
        }
        int nd = (game->dealer_player + 1) % MAX_PLAYERS;
        while (game->player_status[nd] == PLAYER_LEFT)
        {
            nd = (nd + 1) % MAX_PLAYERS;
        }
        game->dealer_player = nd;
    }
    game->current_player = -1;
    game->round_stage = ROUND_PREFLOP;
}

int server_ready(game_state_t *game)
{
    int cnt = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (game->player_status[i] == PLAYER_ACTIVE)
        {
            cnt++;
        }
    }
    return cnt;
}

void server_deal(game_state_t *game)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        game->current_bets[i] = 0;
    }
    game->highest_bet = 0;

    int first = (game->dealer_player + 1) % MAX_PLAYERS;
    while (game->player_status[first] != PLAYER_ACTIVE)
    {
        first = (first + 1) % MAX_PLAYERS;
        if (first == game->dealer_player)
        {
            break;
        }
    }
    game->current_player = first;
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (game->player_status[i] == PLAYER_ACTIVE)
        {
            game->player_hands[i][0] = game->deck[game->next_card++];
            game->player_hands[i][1] = game->deck[game->next_card++];
        }
    }
}

int check_betting_end(game_state_t *game)
{
    int active_count = 0;
    int matching_bet = -1;
    for (int i = 0; i < MAX_PLAYERS; ++i)
    {
        if (game->player_status[i] == PLAYER_ACTIVE)
        {
            active_count++;
            if (game->current_bets[i] == -1)
            {
                // This active player has not acted yet
                return 0;
            }
            if (matching_bet == -1)
            {
                matching_bet = game->current_bets[i];
            }
            else if (game->current_bets[i] != matching_bet)
            {
                // Active players have put in different amounts -> betting still ongoing
                return 0;
            }
        }
    }
    // Round ends if only one player is active (others folded), or all active players have acted and have equal bets
    return (active_count <= 1) ? 1 : 1;
}

void server_community(game_state_t *game)
{
    if (game->round_stage == ROUND_PREFLOP)
    {
        for (int i = 0; i < 3; i++)
        {
            game->community_cards[i] = game->deck[game->next_card++];
        }
        game->round_stage = ROUND_FLOP;
    }
    else if (game->round_stage == ROUND_FLOP)
    {
        game->community_cards[3] = game->deck[game->next_card++];
        game->round_stage = ROUND_TURN;
    }
    else if (game->round_stage == ROUND_TURN)
    {
        game->community_cards[4] = game->deck[game->next_card++];
        game->round_stage = ROUND_RIVER;
    }
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (game->player_status[i] == PLAYER_ACTIVE)
        {
            game->current_bets[i] = 0;
        }
    }
    game->highest_bet = 0;
    int p = (game->dealer_player + 1) % MAX_PLAYERS;
    while (game->player_status[p] != PLAYER_ACTIVE)
    {
        p = (p + 1) % MAX_PLAYERS;
    }
    game->current_player = p;
}

void server_end(game_state_t *game)
{
    int won = find_winner(game);

    if (won >= 0 && won < MAX_PLAYERS)
    {
        game->player_stacks[won] += game->pot_size;
        game->pot_size = 0;
    }
}

int evaluate_hand(game_state_t *game, player_id_t pid)
{
    card_t c[7] = {game->player_hands[pid][0],
                   game->player_hands[pid][1],
                   game->community_cards[0],
                   game->community_cards[1],
                   game->community_cards[2],
                   game->community_cards[3],
                   game->community_cards[4]};
    return (int)value_of_hand(c);
}

int find_winner(game_state_t *game)
{
    uint64_t bestHand = 0;
    int bestPlyr = -1;
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (game->player_status[i] == PLAYER_ACTIVE || game->player_status[i] == PLAYER_ALLIN)
        {
            card_t hand[7] = {game->player_hands[i][0],
                              game->player_hands[i][1],
                              game->community_cards[0],
                              game->community_cards[1],
                              game->community_cards[2],
                              game->community_cards[3],
                              game->community_cards[4]};

            uint64_t v = value_of_hand(hand);
            if (v > bestHand)
            {
                bestHand = v;
                bestPlyr = i;
            }
        }
    }
    return bestPlyr;
}
