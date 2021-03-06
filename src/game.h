#ifndef __GAME_H__
#define __GAME_H__

#include "main.h"

#define mp(x, y) std::make_pair(x, y)
#define min(x, y) std::min(x, y)
#define max(x, y) std::max(x, y)

const int offset[][2] = {
    {2,0},{-2,0},{0,2},{0,-2},{1,1},{1,-1},{-1,1},{-1,-1}
};

const int offset_20[][2] = {
    {2,0},{-2,0},{0,2},{0,-2},{1,1},{1,-1},{-1,1},{-1,-1},
    {1,-3},{2,-2},{3,-1},{3,1},{2,2},{1,3},
    {-1,3},{-2,2},{-3,1},{-3,-1},{-2,-2},{-1,-3},
};

char* prod_name(int prod);
int mineral_cost(int fac, int prod);
bool knows_tech(int fac, int tech);
bool has_ability(int fac, int abl);
bool has_chassis(int fac, int chs);
bool has_weapon(int fac, int wpn);
bool has_terra(int fac, int act);
bool has_project(int fac, int id);
bool has_facility(int base_id, int id);
int unit_triad(int id);
int unit_speed(int id);
int best_armor(int fac);
int best_weapon(int fac);
int best_reactor(int fac);
int offense_value(UNIT* u);
int defense_value(UNIT* u);
int random(int n);
int wrap(int a);
int map_range(int x1, int y1, int x2, int y2);
MAP* mapsq(int x, int y);
int unit_in_tile(MAP* sq);
int set_move_to(int id, int x, int y);
int set_road_to(int id, int x, int y);
int set_action(int id, int act, char icon);
int set_convoy(int id, int res);
int at_target(VEH* veh);
bool water_base(int id);
bool workable_tile(int x, int y, int fac);
int nearby_items(int x, int y, int item);
int bases_in_range(int x, int y, int range);
int coast_tiles(int x, int y);

class TileSearch {
    int type;
    int head;
    int tail;
    int items;
    MAP* tile;
    std::pair<int, int> newtiles[QSIZE];
    public:
    int cur_x, cur_y;
    std::set<std::pair <int, int>> oldtiles;
    void init(int, int, int);
    int visited();
    MAP* get_next();
};


#endif // __GAME_H__
