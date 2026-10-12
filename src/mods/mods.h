/* Mods (vfs.c): loose files that stand in for entries of the game's archives. */
#ifndef FC_MODS_H
#define FC_MODS_H

/* Read every mod folder under mods_dir, in name order. Before the game runs. */
void mods_scan(const char* mods_dir);

/* How many mod folders were found. */
int mods_count(void);

#endif
