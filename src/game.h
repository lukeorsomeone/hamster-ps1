#ifndef GAME_H
#define GAME_H
#include <stdint.h>

void game_init(void);
void game_update(uint16_t held);
void game_render(void);

#endif
