/*
 * Boss practice (port menu, BOSSES tab): fight again any boss of Sora's story
 * already beaten, with no XP or rewards, then return to where Sora stood. The
 * TUTORIALS tab replays the two battle tutorials of Traverse Town the same
 * way (they bring their own deck).
 *
 * Flow:
 *   1. The menu lists the bosses this file has got past (PortBossInfo), worked
 *      out from the story progress kept in the save (see Faced below).
 *   2. Picking one sets a request; the field's main loop (MapFldMain, map.c)
 *      takes it like a normal encounter: fade out, the field is set to resume
 *      (RequestFieldResume), and MapFldStartBattle hands over to us instead of
 *      starting the room's battle.
 *   3. PortBossStart starts the battle. Once the field has shut down (it
 *      stores Sora's position and the room's enemies in the game state as it
 *      does), the battle's set-up calls PortBossBattleInit, which saves the
 *      whole game state, then sets the world, battle stage and floor of the
 *      original fight (they decide the backdrop, the music and the enemies'
 *      strength) and fills Sora's HP.
 *   4. Won or lost (ExitBattle, or the game-over fade in battle_runtime.c),
 *      PortBossEnd puts the saved game state back (only the play time
 *      carries on) and returns to the field, which resumes where it was. The
 *      post-battle story events, the Continue screen and the story flags the
 *      battle would set are skipped; the XP orbs are not counted (btl2.c).
 *
 * Enemy level (BOSSES only, not tutorials): the player may pick a level from 1
 * to Sora's own instead of the original strength. Enemies have two stats, HP
 * and attack (there is no defence: Sora's damage comes from his cards). The
 * game scales them with the castle floor, not with any level:
 *     HP     x (1 + floor * a / 256)   a = 25 up to 10F, then 51
 *     attack x (1 + floor * b / 256)   b = 102 up to 10F, then 76
 * (InitEnemyBtlObj), while the Organization's and Riku's are fixed per
 * battle. The chosen level is turned into an equivalent (fractional) floor
 * through Sora's typical level on each floor of the story (sStoryLevels),
 * and every enemy of the fight gets HP and attack times the ratio of those
 * multipliers at that floor and at the fight's own floor. So a boss at the
 * typical level of its floor is as in the story, weaker below, stronger
 * above, and the fixed-stat bosses follow the same curve.
 *
 * Where each boss is fought (offline from the ROM's event chains): world
 * bosses in a world's story step, the Organization fights in a floor's exit
 * hall, and the last ones on 13F, reached only by finishing the game.
 */
#include "types.h"
#include "fade.h"
#include "game_state.h"
#include "map_api.h"
#include "map_types.h"
#include "mode.h"
#include "mode_battle_data.h"
#include "save_types.h"
#include "system_state.h"
#include "world_types.h"
#include "port.h"

#include <string.h>

extern u8 gWorldBattleStages[];

enum {
    BOSS_WORLD, /* in world `where`'s story, at event step `step` */
    BOSS_FLOOR, /* in floor `where`'s exit hall */
    BOSS_CLEAR, /* on 13F: needs the game finished */
};

typedef struct {
    u8 tutorial; /* listed under TUTORIALS */
    u8 battleId;
    u8 kind;
    u8 where;
    u8 step;
    const char* name;
    const char* place;
} BossDef;

static const BossDef sBosses[] = {
    /* Both in Traverse Town (1F): the first on arrival, the second at its
     * first story step; listed once that step is done. */
    { 1, 178, BOSS_WORLD, WORLD_TRAVERSE_TOWN, 0, "BASICS (HOODED FIGURE)", "TRAVERSE TOWN" },
    { 1, 179, BOSS_WORLD, WORLD_TRAVERSE_TOWN, 0, "COMBOS AND SLEIGHTS (LEON)", "TRAVERSE TOWN" },
    { 0, 148, BOSS_WORLD, WORLD_TRAVERSE_TOWN, 2, "GUARD ARMOR", "TRAVERSE TOWN" },
    { 0, 162, BOSS_FLOOR, 0, 0, "AXEL", "CASTLE OBLIVION 1F" },
    { 0, 149, BOSS_WORLD, WORLD_AGRABAH, 2, "JAFAR", "AGRABAH" },
    { 0, 151, BOSS_WORLD, WORLD_ATLANTICA, 2, "URSULA", "ATLANTICA" },
    { 0, 159, BOSS_WORLD, WORLD_OLYMPUS_COLISEUM, 1, "CLOUD", "OLYMPUS COLISEUM" },
    { 0, 160, BOSS_WORLD, WORLD_OLYMPUS_COLISEUM, 2, "HADES", "OLYMPUS COLISEUM" },
    { 0, 150, BOSS_WORLD, WORLD_WONDERLAND, 2, "TRICKMASTER", "WONDERLAND" },
    { 0, 152, BOSS_WORLD, WORLD_MONSTRO, 1, "PARASITE CAGE", "MONSTRO" },
    { 0, 155, BOSS_WORLD, WORLD_HALLOWEEN_TOWN, 2, "OOGIE BOOGIE", "HALLOWEEN TOWN" },
    { 0, 158, BOSS_WORLD, WORLD_NEVER_LAND, 2, "CAPTAIN HOOK", "NEVERLAND" },
    { 0, 163, BOSS_FLOOR, 5, 0, "LARXENE", "CASTLE OBLIVION 6F" },
    { 0, 161, BOSS_FLOOR, 6, 0, "RIKU", "CASTLE OBLIVION 7F" },
    { 0, 168, BOSS_FLOOR, 7, 0, "RIKU (2)", "CASTLE OBLIVION 8F" },
    { 0, 153, BOSS_WORLD, WORLD_HOLLOW_BASTION, 2, "DRAGON MALEFICENT", "HOLLOW BASTION" },
    { 0, 164, BOSS_FLOOR, 9, 0, "VEXEN", "CASTLE OBLIVION 10F" },
    { 0, 154, BOSS_WORLD, WORLD_DESTINY_ISLANDS, 1, "DARKSIDE", "DESTINY ISLANDS" },
    { 0, 169, BOSS_FLOOR, 10, 0, "RIKU (3)", "CASTLE OBLIVION 11F" },
    { 0, 175, BOSS_WORLD, WORLD_TWILIGHT_TOWN, 0, "VEXEN (2)", "TWILIGHT TOWN" },
    { 0, 170, BOSS_FLOOR, 11, 0, "RIKU (4)", "CASTLE OBLIVION 12F" },
    { 0, 173, BOSS_WORLD, WORLD_CASTLE_OBLIVION, 2, "AXEL (2)", "CASTLE OBLIVION 13F" },
    { 0, 174, BOSS_CLEAR, 12, 0, "LARXENE (2)", "CASTLE OBLIVION 13F" },
    { 0, 165, BOSS_CLEAR, 12, 0, "MARLUXIA", "CASTLE OBLIVION 13F" },
    { 0, 156, BOSS_CLEAR, 12, 0, "MARLUXIA (FINAL)", "CASTLE OBLIVION 13F" },
};

#define BOSS_COUNT ((int)(sizeof(sBosses) / sizeof(sBosses[0])))

enum { STATE_IDLE, STATE_REQUESTED, STATE_STARTING, STATE_FIGHTING, STATE_ENDING };

/* Sora's usual level when reaching each floor in the story (estimates, to
 * tune by playing): the level at which an enemy has its floor's strength. */
static const u8 sStoryLevels[13] = { 5, 10, 14, 18, 22, 26, 30, 34, 38, 42, 46, 50, 55 };

static int sState;
static int sBoss;
static int sLevel;       /* chosen enemy level, 0 = original */
static int sScaleFloor;  /* the fight's own floor */
static s32 sLevelFloor;  /* the level's equivalent floor, 1/256 units */
static int sScaleLogged;
static GameState sSaved;     /* the game state before the practice battle */
static u32 sFieldFrame = ~0u; /* gFrameCounter when the field's main loop last ran */

/* Floor f's story state: the live one for the current floor (stored to
 * gGameState.floors only when leaving it). */
static void FloorState(int f, u16* flags, u8* world, u8* eventStep) {
    if (f == gGameState.floor) {
        *flags = gMapFloorState.flags;
        *world = gMapFloorState.world;
        *eventStep = gMapFloorState.eventStep;
    } else {
        *flags = gGameState.floors[f].flags;
        *world = gGameState.floors[f].world;
        *eventStep = gGameState.floors[f].eventStep;
    }
}

/* The floor boss b is fought on, or -1 when not known yet. */
static int BossFloor(const BossDef* b) {
    int f;

    if (b->kind != BOSS_WORLD) {
        return b->where;
    }
    for (f = 0; f < 13; f++) {
        u16 flags;
        u8 world, step;

        FloorState(f, &flags, &world, &step);
        if (world == b->where) {
            return f;
        }
    }
    return -1;
}

/* Whether this file got past boss b in the story. */
static int Faced(const BossDef* b) {
    u16 flags;
    u8 world, step;
    int f;

    if (gGameState.flags & GAME_FLAG_SORA_CLEAR) {
        return 1;
    }
    switch (b->kind) {
    case BOSS_WORLD:
        f = BossFloor(b);
        if (f < 0) {
            return 0;
        }
        FloorState(f, &flags, &world, &step);
        return step > b->step || (flags & (FLOOR_FLAG_EXIT_UNLOCKED | FLOOR_FLAG_CLEARED)) != 0;
    case BOSS_FLOOR:
        FloorState(b->where, &flags, &world, &step);
        return (flags & FLOOR_FLAG_CLEARED) != 0 || gGameState.floor > b->where;
    }
    return 0;
}

int PortBossCount(void) {
    return BOSS_COUNT;
}

int PortBossIsTutorial(int i) {
    return sBosses[i].tutorial;
}

int PortBossInfo(int i, const char** name, const char** place) {
    *name = sBosses[i].name;
    *place = sBosses[i].place;
    return Faced(&sBosses[i]);
}

int PortBossAvailable(void) {
    if (gGameState.flags & GAME_FLAG_RIKU) {
        return PORT_BOSS_RIKU;
    }
    if (sState != STATE_IDLE || gFrameCounter - sFieldFrame > 2) {
        return PORT_BOSS_NOT_FIELD;
    }
    return PORT_BOSS_OK;
}

int PortBossSoraLevel(void) {
    return gGameState.progression.level;
}

/* The floor (1/256 units) whose enemies match `level`, along sStoryLevels;
 * past either end the nearest step's slope carries on, down to floor -1. */
static s32 LevelFloor(int level) {
    s32 f;
    int i;

    if (level <= sStoryLevels[0]) {
        f = (level - sStoryLevels[0]) * 256 / (sStoryLevels[1] - sStoryLevels[0]);
        return f < -256 ? -256 : f;
    }
    for (i = 0; i < 12; i++) {
        if (level <= sStoryLevels[i + 1]) {
            return i * 256 + (level - sStoryLevels[i]) * 256 / (sStoryLevels[i + 1] - sStoryLevels[i]);
        }
    }
    return 12 * 256 + (level - sStoryLevels[12]) * 256 / (sStoryLevels[12] - sStoryLevels[11]);
}

/* The game's floor multipliers (x256) at floor f (1/256 units). */
static s32 HpScale(s32 f) {
    return 256 + f * (f <= 9 * 256 ? 25 : 51) / 256;
}

static s32 AttackScale(s32 f) {
    return 256 + f * (f <= 9 * 256 ? 102 : 76) / 256;
}

static short ScaleStat(short v, s32 to, s32 from) {
    s32 r;

    if (v <= 0) {
        return v;
    }
    r = (v * to + from / 2) / from;
    return r < 1 ? 1 : r > 32000 ? 32000 : r;
}

void PortBossScaleEnemy(short* maxHp, short* attack) {
    s32 from;

    if (sState != STATE_FIGHTING || sLevel == 0) {
        return;
    }
    from = sScaleFloor * 256;
    if (!sScaleLogged) {
        sScaleLogged = 1;
        PortLog("boss practice: level %d = floor %d.%02d (HP x%d%%, attack x%d%%)", sLevel, (int)(sLevelFloor / 256) + 1,
                (int)((sLevelFloor & 255) * 100 / 256), (int)(HpScale(sLevelFloor) * 100 / HpScale(from)),
                (int)(AttackScale(sLevelFloor) * 100 / AttackScale(from)));
    }
    *maxHp = ScaleStat(*maxHp, HpScale(sLevelFloor), HpScale(from));
    *attack = ScaleStat(*attack, AttackScale(sLevelFloor), AttackScale(from));
}

void PortBossRequest(int i, int level) {
    if (i >= 0 && i < BOSS_COUNT && PortBossAvailable() == PORT_BOSS_OK && Faced(&sBosses[i])) {
        sBoss = i;
        /* Tutorials keep their own strength. */
        sLevel = sBosses[i].tutorial || level < 0 ? 0 : level > PortBossSoraLevel() ? PortBossSoraLevel() : level;
        sState = STATE_REQUESTED;
        PortLog("boss practice: %s requested (level %s%d)", sBosses[i].name, sLevel ? "" : "original ", sLevel);
    }
}

int PortBossFieldTick(void) {
    sFieldFrame = gFrameCounter;
    return sState == STATE_REQUESTED;
}

int PortBossStart(void) {
    if (sState != STATE_REQUESTED) {
        return 0;
    }
    sState = STATE_STARTING;
    ModeRequest(&gModeBattle, sBosses[sBoss].battleId);
    return 1;
}

void PortBossBattleInit(void) {
    const BossDef* b = &sBosses[sBoss];
    int f;

    if (sState != STATE_STARTING) {
        return;
    }
    memcpy(&sSaved, &gGameState, sizeof(sSaved));
    f = BossFloor(b);
    if (b->kind == BOSS_WORLD) {
        gGameState.world = b->where;
        gGameState.battleStage = gWorldBattleStages[b->where];
    } else {
        /* The exit hall, as EnterExitHall sets it. */
        gGameState.world = 0;
        gGameState.battleStage = BATTLE_STAGE_CASTLE_OBLIVION;
    }
    if (f >= 0) {
        gGameState.floor = f;
    }
    sScaleFloor = gGameState.floor;
    sLevelFloor = LevelFloor(sLevel);
    sScaleLogged = 0;
    gGameState.roomEffect = 0;
    gGameState.flags &= ~GAME_FLAG_FIRST_STRIKE;
    gGameState.hp = gGameState.progression.maxHp;
    sState = STATE_FIGHTING;
    PortLog("boss practice: %s (battle %d, floor %d)", b->name, b->battleId, gGameState.floor + 1);
}

int PortBossActive(void) {
    return sState == STATE_FIGHTING || sState == STATE_ENDING;
}

int PortBossEnd(int won) {
    u32 playTime;

    if (sState == STATE_ENDING) {
        return 1; /* called again until the mode changes */
    }
    if (sState != STATE_FIGHTING) {
        return 0;
    }
    playTime = gGameState.playTime;
    memcpy(&gGameState, &sSaved, sizeof(gGameState));
    gGameState.playTime = playTime;
    sState = STATE_ENDING;
    PortLog("boss practice: %s %s, back to the field", sBosses[sBoss].name, won ? "beaten" : "lost");
    RequestMapMode();
    return 1;
}

void PortBossBattleExit(void) {
    if (sState == STATE_ENDING) {
        sState = STATE_IDLE;
    }
}
