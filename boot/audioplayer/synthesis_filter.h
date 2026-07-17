/* synthesis_filter.h — Filtro di sintesi polifase MPEG-1 (32-subband)
 *
 * Modulo condiviso tra decoder MP2 e MP3: entrambi i layer producono
 * 32 campioni di subband per canale, che questo filtro trasforma in
 * 32 campioni PCM time-domain tramite matricizzazione + finestra prototipo
 * + collasso 16→1 (ISO/IEC 11172-3 Annex 3-A.2).
 *
 * Stato per-canale: buffer FIFO V[1024] con offset rotante.
 * Una chiamata a synfilt_process consuma 32 subband e produce 32 PCM.
 */

#ifndef MAINDOB_SYNTHESIS_FILTER_H
#define MAINDOB_SYNTHESIS_FILTER_H

#include <dob/types.h>

typedef struct
{
    int16_t V[1024];  /* FIFO polyphase (Q9 fixed-point), stride 64 per chiamata */
    int     offset;   /* Indice di scrittura corrente (0..1023, step -64) */
} synfilt_state_t;

/* Init globale: precomputa matrice N[64][32] e finestra prototipo D[512].
 * Idempotente, safe da chiamare più volte. */
void synfilt_init(void);

/* Azzera stato per-canale. Chiamare a inizio playback e su seek. */
void synfilt_reset(synfilt_state_t *s);

/* Processa un blocco di 32 subband samples → 32 PCM int16 stereo-interleaved.
 * Scrive solo il canale indicato (0=L, 1=R) di pcm_out, lasciando
 * invariato l'altro. pcm_out deve essere un buffer di 32 frames stereo. */
void synfilt_process(synfilt_state_t *s, const float subband[32],
                     int16_t *pcm_out, int channel);

#endif /* MAINDOB_SYNTHESIS_FILTER_H */
