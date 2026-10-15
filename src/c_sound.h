/* ============================================================
 * c_sound.h — PSG 8 ビート BGM と効果音
 * ============================================================ */
#ifndef C_SOUND_H
#define C_SOUND_H

void sound_init(void);
void sound_stop(void);
void sound_tick(void);
void sfx_shot(void);
void sfx_destroy(void);

#endif
