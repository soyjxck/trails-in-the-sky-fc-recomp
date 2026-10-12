/* Mods (vfs.c): loose files that stand in for entries of the game's archives. */
#ifndef FC_MODS_H
#define FC_MODS_H

/* The game's own folder: SoraVoice's scripts (voice\scena), which come before
 * any mod folder's. Before the game runs, and before mods_scan. */
void mods_scan_game(const char* game_dir);

/* Read every mod folder under mods_dir, in name order. Before the game runs. */
void mods_scan(const char* mods_dir);

/* How many mod folders were found. */
int mods_count(void);

#endif
