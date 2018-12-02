
#include "main.h"
#include "patch.h"
#include "game.h"
#include "move.h"
#include "lib/ini.h"

FILE* debug_log = NULL;
Config conf;
AIPlans plans[8];

std::set<std::pair<int,int>> convoys;
std::set<std::pair<int,int>> boreholes;
std::set<std::pair<int,int>> needferry;

static int handler(void* user, const char* section, const char* name, const char* value) {
    Config* pconfig = (Config*)user;
    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    if (MATCH("thinker", "free_formers")) {
        pconfig->free_formers = atoi(value);
    } else if (MATCH("thinker", "satellites_nutrient")) {
        pconfig->satellites_nutrient = max(0, atoi(value));
    } else if (MATCH("thinker", "satellites_mineral")) {
        pconfig->satellites_mineral = max(0, atoi(value));
    } else if (MATCH("thinker", "satellites_energy")) {
        pconfig->satellites_energy = max(0, atoi(value));
    } else if (MATCH("thinker", "design_units")) {
        pconfig->design_units = atoi(value);
    } else if (MATCH("thinker", "factions_enabled")) {
        pconfig->factions_enabled = atoi(value);
    } else if (MATCH("thinker", "social_ai")) {
        pconfig->social_ai = atoi(value);
    } else if (MATCH("thinker", "movement_ai")) {
        pconfig->movement_ai = atoi(value);
    } else if (MATCH("thinker", "production_ai")) {
        pconfig->production_ai = atoi(value);
    } else if (MATCH("thinker", "tech_balance")) {
        pconfig->tech_balance = atoi(value);
    } else if (MATCH("thinker", "load_expansion")) {
        pconfig->load_expansion = atoi(value);
    } else if (MATCH("thinker", "faction_placement")) {
        pconfig->faction_placement = atoi(value);
    } else {
        for (int i=0; i<16; i++) {
            if (MATCH("thinker", lm_params[i])) {
                pconfig->landmarks &= ~((atoi(value) ? 0 : 1) << i);
                return 1;
            }
        }
        return 0;  /* unknown section/name, error */
    }
    return 1;
}

DLL_EXPORT BOOL APIENTRY DllMain(HINSTANCE UNUSED(hinstDLL), DWORD fdwReason, LPVOID UNUSED(lpvReserved)) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            // Open a debug log if appropriate; read the config file; patch SMAC's memory to call our functions

            // Default config
            conf.free_formers = 0;
            conf.satellites_nutrient = 0;
            conf.satellites_mineral = 0;
            conf.satellites_energy = 0;
            conf.design_units = 1;
            conf.factions_enabled = 7;
            conf.social_ai = 1;
            conf.movement_ai = 1;
            conf.production_ai = 1;
            conf.tech_balance = 1;
            conf.load_expansion = 1;
            conf.faction_placement = 1;
            conf.landmarks = 0xffff;

            // Defensive programming:
            //   This should already be zero filled: DLL_PROCESS_ATTACH is called exactly once and plans is a global var.
            memset(plans, 0, sizeof(AIPlans)*8);

            if (DEBUG && !(debug_log = fopen("debug.txt", "w")))
                return FALSE; // Fail if debug is set and debug.txt cannot be opened for writing
            if (ini_parse("thinker.ini", handler, &conf) < 0)
                return FALSE; // Fail if config file cannot be parsed
            if (!patch_setup(&conf))
                return FALSE;

            // Initialise global AI defaults
            for (int i=1; i<8; i++) {
                plans[i].proj_limit = 5;
                plans[i].enemy_range = 20;
            }

            // Put thinker's name in SMAC's version field :)
            *tx_version = VERSION;
            *tx_date = __DATE__;
            break;

        case DLL_PROCESS_DETACH:
            fclose(debug_log);
            break;

        case DLL_THREAD_ATTACH:
            break;

        case DLL_THREAD_DETACH:
            break;
    }
    return TRUE;
}

DLL_EXPORT int ThinkerDecide() {
    // Unused, I think
    return 0;
}

bool ai_enabled(int fac) {
    // Faction is one of the seven, is in the first n enabled, and is not a human player
    return fac > 0 && fac <= conf.factions_enabled && !(1 << fac & *tx_human_players);
}

HOOK_API int base_production(int id, int v1, int v2, int v3) {
    // Replace base_production decisions if thinker enabled for this faction.
    assert(id >= 0 && id < BASES);
    BASE* base = &tx_bases[id];
    int prod = base->queue_production_id[0];
    int fac = base->faction_id;
    int choice = 0;

    if (DEBUG) {
        debuglog("[ turn: %d faction: %d base: %2d x: %2d y: %2d "\
        "pop: %d tal: %d dro: %d mins: %2d acc: %2d prod: %3d | %s | %s ]\n",
        *tx_current_turn, fac, id, base->x, base->y,
        base->pop_size, base->talent_total, base->drone_total,
        base->mineral_intake, base->minerals_accumulated,
        prod, prod_name(prod), (char*)&(base->name));
    }

    if (1 << fac & *tx_human_players) {
        debuglog("skipping human base\n");
        choice = base->queue_production_id[0];
    } else if (!conf.production_ai || !ai_enabled(fac)) {
        debuglog("skipping computer base\n");
        choice = tx_base_prod_choices(id, v1, v2, v3);
    } else {
        tx_set_base(id);
        tx_base_compute(1);
        if ((choice = need_psych(id)) != 0) {
            debuglog("BUILD PSYCH\n");
        } else if (prod < 0 && !can_build(id, abs(prod))) {
            debuglog("BUILD CHANGE\n");
            if (base->minerals_accumulated > tx_basic->retool_exemption)
                choice = find_facility(id);
            else
                choice = select_prod(id);
        } else if (base->status_flags & BASE_PRODUCTION_DONE) {
            choice = select_prod(id);
            if (choice < 0)
                base->status_flags &= ~BASE_PRODUCTION_DONE;
        } else {
            debuglog("BUILD OLD\n");
            choice = prod;
        }
        debuglog("choice: %d %s\n", choice, prod_name(choice));
    }
    fflush(debug_log);
    return choice;
}

HOOK_API int enemy_move(int id) {
    assert(id >= 0 && id < UNITS);
    VEH* veh = &tx_vehicles[id];
    int fac = veh->faction_id;
    if (conf.movement_ai && ai_enabled(fac)) {
        int w = tx_units[veh->proto_id].weapon_type;
        if (w == WPN_COLONY_MODULE) {
            return colony_move(id);
        } else if (w == WPN_SUPPLY_TRANSPORT) {
            return crawler_move(id);
        } else if (w == WPN_TERRAFORMING_UNIT && veh_triad(id) != TRIAD_SEA) {
            return former_move(id);
        } else if (w == WPN_TROOP_TRANSPORT && veh_triad(id) == TRIAD_SEA) {
            return trans_move(id);
        } else if (w <= WPN_PSI_ATTACK && veh_triad(id) == TRIAD_LAND) {
            return combat_move(id);
        }
    }
    return tx_enemy_move(id);
}

HOOK_API int turn_upkeep() {
    tx_turn_upkeep();

    for (int i=1; i<8 && conf.design_units; i++) {
        if (1 << i & *tx_human_players || !tx_factions[i].current_num_bases)
            continue;
        int rec = best_reactor(i);
        int arm = best_armor(i, false);
        int arm2 = best_armor(i, true);
        int abl = has_ability(i, ABL_ID_TRANCE) ? ABL_TRANCE : 0;
        int chs = has_chassis(i, CHS_HOVERTANK) ? CHS_HOVERTANK : CHS_SPEEDER;
        bool twoabl = knows_tech(i, tx_basic->tech_preq_allow_2_spec_abil);
        char* arm_n = tx_defense[arm].name_short;
        char* arm2_n = tx_defense[arm2].name_short;
        char nm[100];

        if (has_weapon(i, WPN_PROBE_TEAM)) {
            int algo = has_ability(i, ABL_ID_ALGO_ENHANCEMENT) ? ABL_ALGO_ENHANCEMENT : 0;
            if (has_chassis(i, CHS_FOIL)) {
                int ship = (rec >= REC_FUSION && has_chassis(i, CHS_CRUISER) ? CHS_CRUISER : CHS_FOIL);
                tx_propose_proto(i, ship, WPN_PROBE_TEAM, (rec >= REC_FUSION ? arm : 0),
                (rec >= REC_FUSION ? abl | (twoabl ? algo : 0) : 0),
                rec, PLAN_INFO_WARFARE, (ship == CHS_FOIL ? "Foil Probe Team" : "Cruiser Probe Team"));
            }
            if (arm != ARM_NO_ARMOR && rec >= REC_FUSION) {
                tx_propose_proto(i, chs, WPN_PROBE_TEAM, arm, abl | (twoabl ? algo : 0),
                rec, PLAN_INFO_WARFARE,
                (strlen(arm_n) < 20 ? strcat(strcpy(nm, arm_n), " Probe Team") : NULL));
            }
        }
        if (has_ability(i, ABL_ID_AAA) && arm != ARM_NO_ARMOR) {
            int abls = ABL_AAA | (twoabl && has_ability(i, ABL_ID_COMM_JAMMER) ?
                ABL_COMM_JAMMER : 0);
            tx_propose_proto(i, CHS_INFANTRY, WPN_LASER, arm, abls, rec, PLAN_DEFENSIVE, NULL);
        }
        if (has_weapon(i, WPN_TERRAFORMING_UNIT) && rec >= REC_FUSION) {
            int abls = has_ability(i, ABL_ID_SUPER_TERRAFORMER ? ABL_SUPER_TERRAFORMER : 0) |
                (twoabl && has_ability(i, ABL_ID_FUNGICIDAL) ? ABL_FUNGICIDAL : 0);
            tx_propose_proto(i, chs, WPN_TERRAFORMING_UNIT, ARM_NO_ARMOR, abls,
            REC_FUSION, PLAN_TERRAFORMING, NULL);
        }
        if (has_weapon(i, WPN_SUPPLY_TRANSPORT) && rec >= REC_FUSION && arm2 != ARM_NO_ARMOR) {
            tx_propose_proto(i, CHS_INFANTRY, WPN_SUPPLY_TRANSPORT, arm2, 0,
            REC_FUSION, PLAN_DEFENSIVE,
            (strlen(arm2_n) < 20 ? strcat(strcpy(nm, arm2_n), " Supply") : NULL));
        }
    }
    if (*tx_current_turn == 1) {
        int bonus = ~(*tx_human_players) & 0xfe;
        for (int i=0; i<*tx_total_num_vehicles; i++) {
            VEH* veh = &tx_vehicles[i];
            if (1 << veh->faction_id & bonus) {
                bonus &= ~(1 << veh->faction_id);
                MAP* sq = mapsq(veh->x, veh->y);
                int unit = (is_ocean(sq) ? BSC_SEA_FORMERS : BSC_FORMERS);
                for (int j=0; j<conf.free_formers; j++) {
                    int k = tx_veh_init(unit, veh->faction_id, veh->x, veh->y);
                    if (k >= 0) {
                        tx_vehicles[k].home_base_id = -1;
                    }
                }
            }
        }
        for (int i=1; i<8; i++) {
            if (1 << i & ~*tx_human_players) {
                tx_factions[i].satellites_nutrient = conf.satellites_nutrient;
                tx_factions[i].satellites_mineral = conf.satellites_mineral;
                tx_factions[i].satellites_energy = conf.satellites_energy;
            }
        }
    }
    if (DEBUG) {
        *tx_game_rules |= RULES_DEBUG_MODE;
    }
    int minerals[BASES];
    for (int i=1; i<8; i++) {
        Faction* f = &tx_factions[i];
        R_Resource* r = tx_resource;
        if (1 << i & *tx_human_players || !f->current_num_bases)
            continue;
        plans[i].diplo_flags = 0;
        for (int j=1; j<8; j++) {
            int st = f->diplo_status[j];
            if (i != j && st & DIPLO_COMMLINK) {
                plans[i].diplo_flags |= st;
            }
        }
        int n = 0;
        for (int j=0; j<*tx_total_num_bases; j++) {
            BASE* base = &tx_bases[j];
            if (base->faction_id == i) {
                minerals[n++] = base->mineral_surplus;
            }
        }
        std::sort(minerals, minerals+n);
        plans[i].proj_limit = max(5, minerals[n*2/3]);

        int psi = max(-4, 2 - f->best_weapon_value);
        if (has_project(i, FAC_NEURAL_AMPLIFIER))
            psi += 2;
        if (has_project(i, FAC_DREAM_TWISTER))
            psi += 2;
        if (has_project(i, FAC_PHOLUS_MUTAGEN))
            psi++;
        if (has_project(i, FAC_XENOEMPATHY_DOME))
            psi++;
        psi += tx_factions_meta[i].rule_psi/10 + f->SE_planet;
        plans[i].psi_score = psi;

        const int manifold[][3] = {{0,0,0}, {0,1,0}, {1,1,0}, {1,1,1}, {1,2,1}};
        int p = (has_project(i, FAC_MANIFOLD_HARMONICS) ? min(4, max(0, f->SE_planet + 1)) : 0);
        int dn = manifold[p][0] + f->tech_fungus_nutrient - r->forest_sq_nutrient;
        int dm = manifold[p][1] + f->tech_fungus_mineral - r->forest_sq_mineral;
        int de = manifold[p][2] + f->tech_fungus_energy - r->forest_sq_energy;
        bool terra_fungus = has_terra(i, FORMER_PLANT_FUNGUS)
            && (knows_tech(i, tx_basic->tech_preq_ease_fungus_mov) || has_project(i, FAC_XENOEMPATHY_DOME));

        plans[i].keep_fungus = (dn+dm+de > 0);
        if (dn+dm+de > 0 && terra_fungus) {
            plans[i].plant_fungus = dn+dm+de;
        } else {
            plans[i].plant_fungus = 0;
        }
        debuglog("turn_upkeep %d %2d %2d %2d %2d %2d\n", i, plans[i].proj_limit, psi, dn, dm, de);
    }

    return 0;
}

HOOK_API int faction_upkeep(int fac) {
    tx_faction_upkeep(fac);
    if (ai_enabled(fac))
        move_upkeep(fac);
    fflush(debug_log);
    return 0;
}

HOOK_API int tech_value(int tech, int fac, int flag) {
    int value = tx_tech_val(tech, fac, flag);
    if (conf.tech_balance && ai_enabled(fac)) {
        if (tech == tx_weapon[WPN_TERRAFORMING_UNIT].preq_tech
        || tech == tx_weapon[WPN_SUPPLY_TRANSPORT].preq_tech
        || tech == tx_basic->tech_preq_allow_3_energy_sq
        || tech == tx_basic->tech_preq_allow_3_minerals_sq
        || tech == tx_basic->tech_preq_allow_3_nutrients_sq) {
            value *= 2;
        }
    }
    debuglog("tech_value %d %d %d %s\n", tech, fac, value, tx_techs[tech].name);
    return value;
}

int project_score(int fac, int proj) {
    R_Facility* p = &tx_facility[proj];
    Faction* f = &tx_factions[fac];
    return random(3) + f->AI_fight * p->AI_fight
        + (f->AI_growth+1) * p->AI_growth + (f->AI_power+1) * p->AI_power
        + (f->AI_tech+1) * p->AI_tech + (f->AI_wealth+1) * p->AI_wealth;
}

int find_project(int base_id) {
    BASE* base = &tx_bases[base_id];
    int fac = base->faction_id;
    Faction* fact = &tx_factions[fac];
    int bases = fact->current_num_bases;
    int nuke_limit = (fact->planet_busters < fact->AI_fight + 2 ? 1 : 0);
    int projs = 0;
    int nukes = 0;

    bool repeal = *tx_un_charter_repeals > *tx_un_charter_reinstates;
    bool build_nukes = has_weapon(fac, WPN_PLANET_BUSTER) &&
        (repeal || plans[fac].diplo_flags & DIPLO_ATROCITY_VICTIM ||
        (fact->AI_fight > 0 && fact->AI_power > 0));

    for (int i=0; i<*tx_total_num_bases; i++) {
        if (tx_bases[i].faction_id == fac) {
            int prod = tx_bases[i].queue_production_id[0];
            if (prod <= -70 || prod == FAC_SUBSPACE_GENERATOR) {
                projs++;
            } else if (prod >= 0 && tx_units[prod].weapon_type == WPN_PLANET_BUSTER) {
                nukes++;
            }
        }
    }
    if (build_nukes && nukes < nuke_limit && nukes < bases/8) {
        int best = 0;
        for(int i=0; i<64; i++) {
            int id = fac*64 + i;
            UNIT* u = &tx_units[id];
            if (u->weapon_type == WPN_PLANET_BUSTER && strlen(u->name) > 0
            && offense_value(u) > offense_value(&tx_units[best])) {
                debuglog("find_project %d %d %s\n", fac, id, (char*)tx_units[id].name);
                best = id;
            }
        }
        if (best)
            return best;
    }
    if (projs+nukes < (build_nukes ? 4 : 3) && projs+nukes < bases/4) {
        bool alien = tx_factions_meta[fac].rule_flags & FACT_ALIEN;
        if (alien && can_build(base_id, FAC_SUBSPACE_GENERATOR)) {
            return -FAC_SUBSPACE_GENERATOR;
        }
        int score = INT_MIN;
        int choice = 0;
        for (int i=70; i<107; i++) {
            if (alien && (i == FAC_ASCENT_TO_TRANSCENDENCE || i == FAC_VOICE_OF_PLANET))
                continue;
            if (can_build(base_id, i)) {
                int sc = project_score(fac, i);
                choice = (sc > score ? i : choice);
                score = max(score, sc);
                debuglog("find_project %d %d %d %s\n", fac, i, sc, (char*)tx_facility[i].name);
            }
        }
        return (projs > 0 || score > 2 ? -choice : 0);
    }
    return 0;
}

bool can_build_ships(int id) {
    BASE* b = &tx_bases[id];
    int k = *tx_map_area_sq_root + 20;
    return has_chassis(b->faction_id, CHS_FOIL) && nearby_tiles(b->x, b->y, WATER_ONLY, k) >= k;
}

int unit_score(int id, int fac, bool def) {
    const int abls[][2] = {
        {ABL_AAA, 5},
        {ABL_AIR_SUPERIORITY, 3},
        {ABL_ALGO_ENHANCEMENT, 5},
        {ABL_AMPHIBIOUS, -4},
        {ABL_ARTILLERY, -2},
        {ABL_DROP_POD, 5},
        {ABL_EMPATH, 2},
        {ABL_TRANCE, 2},
        {ABL_TRAINED, 1},
        {ABL_COMM_JAMMER, 3},
        {ABL_BLINK_DISPLACER, 3},
        {ABL_DEEP_PRESSURE_HULL, 4},
        {ABL_SUPER_TERRAFORMER, 8},
    };
    int v = 2 * (def ? defense_value(&tx_units[id]) : offense_value(&tx_units[id]));
    if (v < 0) {
        v = (def ? tx_factions[fac].best_armor_value : tx_factions[fac].best_weapon_value)
            * best_reactor(fac) + plans[fac].psi_score * 2;
    }
    if (unit_triad(id) == TRIAD_LAND)
        v += 3 * unit_speed(id);
    for (const int* i : abls) {
        if (tx_units[id].ability_flags & i[0]) {
            v += i[1];
        }
    }
    return v;
}

int find_proto(int fac, int triad, int mode, bool defend) {
    int basic = BSC_SCOUT_PATROL;
    debuglog("find_proto fac: %d triad: %d mode: %d def: %d\n", fac, triad, mode, defend);
    if (mode == WMODE_COLONIST)
        basic = (triad == TRIAD_SEA ? BSC_SEA_ESCAPE_POD : BSC_COLONY_POD);
    else if (mode == WMODE_TERRAFORMER)
        basic = (triad == TRIAD_SEA ? BSC_SEA_FORMERS : BSC_FORMERS);
    else if (mode == WMODE_CONVOY)
        basic = BSC_SUPPLY_CRAWLER;
    else if (mode == WMODE_TRANSPORT)
        basic = BSC_TRANSPORT_FOIL;
    else if (mode == WMODE_INFOWAR)
        basic = BSC_PROBE_TEAM;
    int best = basic;
    for (int i=0; i < 128; i++) {
        int id = (i < 64 ? i : (fac-1)*64 + i);
        UNIT* u = &tx_units[id];
        if (strlen(u->name) > 0 && unit_triad(id) == triad) {
            if (id < 64 && !knows_tech(fac, u->tech_preq))
                continue;
            if ((mode && u->weapon_mode != mode)
            || (!mode && tx_weapon[u->weapon_type].offense_value == 0)
            || (!mode && defend && u->chassis_type != CHS_INFANTRY)
            || (u->weapon_type == WPN_PSI_ATTACK && plans[fac].psi_score < 1)
            || u->weapon_type == WPN_PLANET_BUSTER)
                continue;
            bool valid = (mode || best == basic || (defend == (offense_value(u) < defense_value(u))));
            if (valid && (random(16) > 8 + unit_score(best, fac, defend) - unit_score(id, fac, defend))) {
                best = id;
                debuglog("===> %s\n", tx_units[best].name);
            }
        }
    }
    return best;
}

int need_psych(int id) {
    BASE* b = &tx_bases[id];
    int fac = b->faction_id;
    int unit = unit_in_tile(mapsq(b->x, b->y));
    if (unit != fac || has_project(fac, FAC_TELEPATHIC_MATRIX))
        return 0;
    // I think this says if (drone_riots), which would be a bit late for building facilities.
    if (b->drone_total > b->talent_total) {
        if (can_build(id, FAC_RECREATION_COMMONS))
            return -FAC_RECREATION_COMMONS;
        if (has_project(fac, FAC_VIRTUAL_WORLD) && can_build(id, FAC_NETWORK_NODE))
            return -FAC_NETWORK_NODE;
        if (!b->assimilation_turns_left && b->pop_size >= 6 && b->energy_surplus >= 12
        && can_build(id, FAC_HOLOGRAM_THEATRE))
            return -FAC_HOLOGRAM_THEATRE;
    }
    return 0;
}

int find_facility(int base_id) {
    const int build_order[] = {
        FAC_RECREATION_COMMONS,
        FAC_CHILDREN_CRECHE,
        FAC_PERIMETER_DEFENSE,
        FAC_GENEJACK_FACTORY,
        FAC_NETWORK_NODE,
        FAC_AEROSPACE_COMPLEX,
        FAC_TREE_FARM,
        FAC_HAB_COMPLEX,
        FAC_COMMAND_CENTER,
        FAC_FUSION_LAB,
        FAC_ENERGY_BANK,
        FAC_RESEARCH_HOSPITAL,
        FAC_HABITATION_DOME,
    };
    BASE* base = &tx_bases[base_id];
    int fac = base->faction_id;
    int proj;
    int minerals = base->mineral_surplus;
    int extra = base->minerals_accumulated/10;
    int pop_rule = tx_factions_meta[fac].rule_population;
    int hab_complex_limit = tx_basic->pop_limit_wo_hab_complex - pop_rule;
    int hab_dome_limit = tx_basic->pop_limit_wo_hab_dome - pop_rule;
    Faction* fact = &tx_factions[fac];

    if (*tx_climate_future_change > 0) {
        MAP* sq = mapsq(base->x, base->y);
        if (sq && (sq->level >> 5) == LEVEL_SHORE_LINE && can_build(base_id, FAC_PRESSURE_DOME))
            return -FAC_PRESSURE_DOME;
    }
    if (base->drone_total > 0 && can_build(base_id, FAC_RECREATION_COMMONS))
        return -FAC_RECREATION_COMMONS;
    if (can_build(base_id, FAC_RECYCLING_TANKS))
        return -FAC_RECYCLING_TANKS;
    if (base->pop_size >= hab_complex_limit && can_build(base_id, FAC_HAB_COMPLEX))
        return -FAC_HAB_COMPLEX;
    if (minerals+extra >= plans[fac].proj_limit && (proj = find_project(base_id)) != 0) {
        return proj;
    }
    if (minerals+extra >= plans[fac].proj_limit && has_facility(base_id, FAC_AEROSPACE_COMPLEX)) {
        if (can_build(base_id, FAC_ORBITAL_DEFENSE_POD))
            return -FAC_ORBITAL_DEFENSE_POD;
        if (can_build(base_id, FAC_NESSUS_MINING_STATION))
            return -FAC_NESSUS_MINING_STATION;
        if (can_build(base_id, FAC_ORBITAL_POWER_TRANS))
            return -FAC_ORBITAL_POWER_TRANS;
        if (can_build(base_id, FAC_SKY_HYDRO_LAB))
            return -FAC_SKY_HYDRO_LAB;
    }
    if (minerals >= plans[fac].proj_limit && find_hq(fac) < 0
    && bases_in_range(base->x, base->y, 4) >= 5) {
        return -FAC_HEADQUARTERS;
    }
    for (int f : build_order) {
        if (base->energy_surplus < 6 && (f == FAC_NETWORK_NODE || f == FAC_ENERGY_BANK))
            continue;
        if (base->energy_surplus < 16 && (f == FAC_FUSION_LAB || f == FAC_RESEARCH_HOSPITAL))
            continue;
        if (f == FAC_COMMAND_CENTER && (water_base(base_id) || fact->AI_fight + fact->AI_power < 1))
            continue;
        if (f == FAC_GENEJACK_FACTORY && base->mineral_intake < 16)
            continue;
        if (f == FAC_HAB_COMPLEX && base->pop_size+1 < hab_complex_limit)
            continue;
        if (f == FAC_HABITATION_DOME && base->pop_size < hab_dome_limit)
            continue;
        if (can_build(base_id, f)) {
            return -1*f;
        }
    }
    debuglog("******\n");
    bool build_ships = can_build_ships(base_id) && (water_base(base_id) || random(3) == 0);
    if (has_weapon(fac, WPN_PROBE_TEAM) && random(minerals) < base->mineral_intake/2) {
        return find_proto(fac, (build_ships ? TRIAD_SEA : TRIAD_LAND), WMODE_INFOWAR, DEF);
    }
    if (has_chassis(fac, CHS_NEEDLEJET) && fact->SE_police >= -3 && random(3) == 0) {
        return find_proto(fac, TRIAD_AIR, COMBAT, ATT);
    }
    if (build_ships) {
        return find_proto(fac, TRIAD_SEA, (random(3) == 0 ? WMODE_TRANSPORT : COMBAT), ATT);
    }
    return find_proto(fac, TRIAD_LAND, COMBAT, ATT);
}

int select_colony(int id, int pods, bool build_ships) {
    BASE* base = &tx_bases[id];
    int fac = base->faction_id;
    bool ls = has_base_sites(base->x, base->y, fac, LAND_ONLY);
    bool ws = has_base_sites(base->x, base->y, fac, WATER_ONLY);
    int limit = (*tx_current_turn < 60 || (!random(3) && (ls || (build_ships && ws))) ? 2 : 1);

    if (pods >= limit) {
        return -1;
    }
    if (water_base(id)) {
        for (const int* t : offset) {
            int x2 = wrap(base->x + t[0]);
            int y2 = base->y + t[1];
            if (ls && non_combat_move(x2, y2, fac, TRIAD_LAND) && !random(6)) {
                return TRIAD_LAND;
            }
        }
        if (ws) {
            return TRIAD_SEA;
        }
    } else {
        if (build_ships && ws && (!ls || (*tx_current_turn > 50 && !random(3)))) {
            return TRIAD_SEA;
        } else if (ls) {
            return TRIAD_LAND;
        }
    }
    return -1;
}

int faction_might(int fac) {
    Faction* f = &tx_factions[fac];
    return max(1, f->mil_strength_1 + f->mil_strength_2 + f->pop_total);
}

int select_prod(int id) {
    BASE* base = &tx_bases[id];
    int fac = base->faction_id;
    int minerals = base->mineral_surplus;
    Faction* fact = &tx_factions[fac];

    int transports = 0;
    int defenders = 0;
    int crawlers = 0;
    int formers = 0;
    int probes = 0;
    int pods = 0;
    int enemymask = 1;
    int enemyrange = 40;
    double enemymil = 0;

    for (int i=1; i<8; i++) {
        if (i==fac || ~fact->diplo_status[i] & DIPLO_COMMLINK)
            continue;
        double mil = (1.0 * faction_might(i)) / faction_might(fac);
        if (fact->diplo_status[i] & DIPLO_VENDETTA) {
            enemymask |= (1 << i);
            enemymil = max(enemymil, 1.0 * mil);
        } else if (~fact->diplo_status[i] & DIPLO_PACT) {
            enemymil = max(enemymil, 0.3 * mil);
        }
    }
    for (int i=0; i<*tx_total_num_bases; i++) {
        BASE* b = &tx_bases[i];
        if ((1 << b->faction_id) & enemymask) {
            enemyrange = min(enemyrange, map_range(base->x, base->y, b->x, b->y));
        }
    }

    for (int i=0; i<*tx_total_num_vehicles; i++) {
        VEH* veh = &tx_vehicles[i];
        UNIT* unit = &tx_units[veh->proto_id];
        if (veh->faction_id != fac)
            continue;
        if (veh->home_base_id == id) {
            if (unit->weapon_type == WPN_TERRAFORMING_UNIT)
                formers++;
            else if (unit->weapon_type == WPN_COLONY_MODULE)
                pods++;
            else if (unit->weapon_type == WPN_PROBE_TEAM)
                probes++;
            else if (unit->weapon_type == WPN_SUPPLY_TRANSPORT)
                crawlers += (veh->move_status == STATUS_CONVOY ? 1 : 5);
            else if (unit->weapon_type == WPN_TROOP_TRANSPORT)
                transports++;
        }
        int range = map_range(base->x, base->y, veh->x, veh->y);
        if (range <= 1) {
            if (unit_triad(veh->proto_id) == TRIAD_LAND && unit->weapon_type <= WPN_PSI_ATTACK) {
                defenders++;
            }
        }
    }
    int reserve = max(2, base->mineral_intake / 2);
    double base_ratio = 2.0 * fact->current_num_bases / min(80, *tx_map_area_sq_root);
    bool has_formers = has_weapon(fac, WPN_TERRAFORMING_UNIT);
    bool has_supply = has_weapon(fac, WPN_SUPPLY_TRANSPORT);
    bool build_ships = can_build_ships(id);
    bool build_pods = (base->pop_size > 1 || base->nutrient_surplus > 1) && pods < 2 && base_ratio < 1.0;
    bool sea_base = water_base(id);
    plans[fac].enemy_range = (enemyrange + 7 * plans[fac].enemy_range)/8;

    double w1 = min(1.0, 1.0 * minerals / plans[fac].proj_limit);
    double w2 = enemymil / (enemyrange * 0.1 + 0.1) - max(0, defenders-1) * 0.3
        + min(1.0, (fact->AI_fight * 0.2 + 0.8) * base_ratio);
    double threat = 1 - (1 / (1 + max(0.0, w1 * w2)));
    int def_target = (*tx_current_turn < 50 && !sea_base && !random(3) ? 2 : 1);

    debuglog("select_prod "\
    "%d %d %2d %2d | %d %d %d %d %d %d | %2d %2d %2d | %2d %.4f %.4f %.4f %.4f\n",
    *tx_current_turn, fac, base->x, base->y,
    defenders, formers, pods, probes, crawlers, build_pods,
    minerals, reserve, plans[fac].proj_limit, enemyrange, enemymil, w1, w2, threat);

    if (defenders < def_target && minerals > 2) {
        if (def_target < 2 && (*tx_current_turn > 30 || enemyrange < 15))
            return find_proto(fac, TRIAD_LAND, COMBAT, DEF);
        else
            return BSC_SCOUT_PATROL;
    } else if (minerals > reserve && random(100) < (int)(100.0 * threat)) {
        if (defenders > 2 && enemyrange < 12 && can_build(id, FAC_PERIMETER_DEFENSE))
            return -FAC_PERIMETER_DEFENSE;
        if (has_chassis(fac, CHS_NEEDLEJET) && fact->SE_police >= -3 && random(3) == 0)
            return find_proto(fac, TRIAD_AIR, COMBAT, ATT);
        else if (has_weapon(fac, WPN_PROBE_TEAM) && probes < 1 && random(3) == 0)
            if (build_ships)
                return find_proto(fac, TRIAD_SEA, WMODE_INFOWAR, DEF);
            else
                return find_proto(fac, TRIAD_LAND, WMODE_INFOWAR, DEF);
        else if (build_ships && (sea_base || ((defenders > 1 || enemyrange >= 12) && random(3) == 0)))
            if (random(3) == 0)
                return find_proto(fac, TRIAD_SEA, WMODE_TRANSPORT, DEF);
            else
                return find_proto(fac, TRIAD_SEA, COMBAT, ATT);
        else
            return find_proto(fac, TRIAD_LAND, COMBAT, ATT);
    } else if (has_formers && formers < (base->pop_size < (sea_base ? 4 : 3) ? 1 : 2)) {
        if (sea_base || (build_ships && formers == 1 && id % 3 == 0))
            return find_proto(fac, TRIAD_SEA, WMODE_TERRAFORMER, DEF);
        else
            return find_proto(fac, TRIAD_LAND, WMODE_TERRAFORMER, DEF);
    } else {
        int crawl_target = 1 + min(base->pop_size/3,
            (base->mineral_surplus >= plans[fac].proj_limit ? 3 : 1));
        if (has_supply && !sea_base && crawlers < crawl_target) {
            return find_proto(fac, TRIAD_LAND, WMODE_CONVOY, DEF);
        } else if (build_ships && !transports && needferry.count(mp(base->x, base->y))) {
            return find_proto(fac, TRIAD_SEA, WMODE_TRANSPORT, DEF);
        } else if (build_pods && !can_build(id, FAC_RECYCLING_TANKS)) {
            int tr = select_colony(id, pods, build_ships);
            if (tr == TRIAD_LAND || tr == TRIAD_SEA)
                return find_proto(fac, tr, WMODE_COLONIST, DEF);
        }
        return find_facility(id);
    }
}

int soc_effect(int val, bool immune, bool penalty) {
    if (immune)
        return max(0, val);
    if (penalty)
        return (val < 0 ? val*2 : val);
    return val;
}

int social_score(int fac, int sf, int sm2, int pop_boom, int range, int immunity, int impunity, int penalty) {
    enum {ECO, EFF, SUP, TAL, MOR, POL, GRW, PLA, PRO, IND, RES};
    Faction* f = &tx_factions[fac];
    FactMeta* m = &tx_factions_meta[fac];
    R_Social* s = &tx_social[sf];
    int morale = has_project(fac, FAC_COMMAND_NEXUS ? 2 : 0) + (has_project(fac, FAC_CYBORG_FACTORY) ? 2 : 0);
    int sm1 = (&f->SE_Politics)[sf];
    int sc = 0;
    int vals[11];
    memcpy(vals, &f->SE_economy, sizeof(vals));

    if (sm1 != sm2) {
        for (int i=0; i<11; i++) {
            bool im1 = (1 << i) & immunity || (1 << (sf*4 + sm1)) & impunity;
            bool im2 = (1 << i) & immunity || (1 << (sf*4 + sm2)) & impunity;
            bool pn1 = (1 << (sf*4 + sm1)) & penalty;
            bool pn2 = (1 << (sf*4 + sm2)) & penalty;
            vals[i] -= soc_effect(s->effects[sm1][i], im1, pn1);
            vals[i] += soc_effect(s->effects[sm2][i], im2, pn2);
        }
    }
    sc += (3 + f->AI_wealth + f->AI_tech) * (vals[ECO] + (vals[ECO] >= 2 ? 3 : 0));
    sc += (1 + f->AI_wealth + f->AI_tech + min(4, *tx_current_turn/25))
        * (min(6, vals[EFF]) + (vals[EFF] >= 4 ? 3 : 0) + (vals[EFF] < -2 ? -5 : 0));
    sc += (f->AI_power + f->AI_fight + max(3, 7 - *tx_current_turn/25))
        * (min(3, max(-4, vals[SUP])) + (vals[SUP] <= -4 ? -5 : 0));
    sc += (((vals[MOR] >= 1 && vals[MOR] + morale >= 4) ? 5 : 0) + max(1, 4 - range/8)) * min(4, max(-4, vals[MOR]));
    if (!has_project(fac, FAC_TELEPATHIC_MATRIX))
        sc += (vals[POL] < -2 && range < 20 ? 8 : 1) * (vals[POL] < 1 ? 1 : 3) * min(3, max(-5, vals[POL]));
    if (!has_project(fac, FAC_CLONING_VATS))
        sc += (pop_boom ? 4 : 2) * (vals[GRW] + (pop_boom && vals[GRW] >= 4 ? 4 : 0) + (vals[GRW] < -2 ? -4 : 0));
    sc += ((has_project(fac, FAC_MANIFOLD_HARMONICS) ? 6 : 1) + m->rule_psi/20) * min(3, max(-3, vals[PLA]));
    sc += max(1, 4 - range/8 + f->AI_power + (vals[PRO] >= 3 ? 3 : 0)) * min(3, max(-2, vals[PRO]));
    sc += 7 * min(8 - *tx_diff_level, vals[IND]);
    sc += (3 + f->AI_wealth + f->AI_tech - f->AI_fight) * min(5, max(-5, vals[RES]));

    debuglog("social_score %d %d %d %d %s\n", fac, sf, sm2, sc, s->soc_name[sm2]);
    return sc;
}

HOOK_API int social_ai(int fac, int v1, int v2, int v3, int v4, int v5) {
    Faction* f = &tx_factions[fac];
    FactMeta* m = &tx_factions_meta[fac];
    R_Social* s = tx_social;
    int range = (int)plans[fac].enemy_range;
    int pop_boom = 0;
    int want_pop = 0;
    int pop_total = 0;
    int immunity = 0;
    int impunity = 0;
    int penalty = 0;

    if (!conf.social_ai || !ai_enabled(fac)) {
        return tx_social_ai(fac, v1, v2, v3, v4, v5);
    }
    if (f->SE_upheaval_cost_paid > 0) {
        tx_social_set(fac);
        return 0;
    }
    assert(!memcmp(&f->SE_Politics, &f->SE_Politics_pending, 16));
    for (int i=0; i<m->faction_bonus_count; i++) {
        if (m->faction_bonus_id[i] == FCB_IMMUNITY)
            immunity |= (1 << m->faction_bonus_val1[i]);
        if (m->faction_bonus_id[i] == FCB_IMPUNITY)
            impunity |= (1 << (m->faction_bonus_val1[i] * 4 + m->faction_bonus_val2[i]));
        if (m->faction_bonus_id[i] == FCB_PENALTY)
            penalty |= (1 << (m->faction_bonus_val1[i] * 4 + m->faction_bonus_val2[i]));
    }
    if (has_project(fac, FAC_NETWORK_BACKBONE)) {
        impunity |= (1 << (4*3 + 1));
    }
    if (has_project(fac, FAC_CLONING_VATS)) {
        impunity |= (1 << (4*3 + 3)) | (1 << (4*2 + 1));
    } else if (knows_tech(fac, tx_facility[FAC_CHILDREN_CRECHE].preq_tech)) {
        for (int i=0; i<*tx_total_num_bases; i++) {
            BASE* b = &tx_bases[i];
            if (b->faction_id == fac) {
                want_pop += (tx_pop_goal(i) - b->pop_size)
                    * (b->nutrient_surplus > 1 && has_facility(i, FAC_CHILDREN_CRECHE) ? 4 : 1);
                pop_total += 4 * b->pop_size;
            }
        }
        if (pop_total > 0) {
            pop_boom = ((f->SE_growth < 4 ? 4 : 8) * want_pop) >= pop_total;
        }
    }
    debuglog("social_params %d %d | %d %3d %3d | %2d %4x %4x %4x\n",
        *tx_current_turn, fac, pop_boom, want_pop, pop_total, range, immunity, impunity, penalty);
    int score_diff = 1;
    int sf = -1;
    int sm2 = -1;

    for (int i=0; i<4; i++) {
        int sm1 = (&f->SE_Politics)[i];
        int sc1 = social_score(fac, i, sm1, pop_boom, range, immunity, impunity, penalty);

        for (int j=0; j<4; j++) {
            if (j == sm1 || !knows_tech(fac, s[i].soc_preq_tech[j]) ||
            (i == m->soc_opposition_category && j == m->soc_opposition_model))
                continue;
            int sc2 = social_score(fac, i, j, pop_boom, range, immunity, impunity, penalty);
            if (sc2 - sc1 > score_diff) {
                sf = i;
                sm2 = j;
                score_diff = sc2 - sc1;
            }
        }
    }
    int cost = (m->rule_flags & FACT_ALIEN ? 36 : 24);
    if (sf >= 0 && f->energy_credits > cost) {
        int sm1 = (&f->SE_Politics)[sf];
        debuglog("social_change %d %d | %d %d %2d %2d | %s -> %s\n", *tx_current_turn, fac,
            sf, sm2, cost, score_diff, s[sf].soc_name[sm1], s[sf].soc_name[sm2]);
        (&f->SE_Politics_pending)[sf] = sm2;
        f->energy_credits -= cost;
        f->SE_upheaval_cost_paid += cost;
    }
    tx_social_set(fac);
    tx_consider_designs(fac);
    return 0;
}




