/* panictest — companion di test per la schermata di kernel panic.
 *
 * Invoca la syscall SYS_PANIC_TEST, che fa un kpanic() DELIBERATO. Serve a
 * verificare che, con la GUI/BGA attiva, la schermata di panic torni
 * VISIBILE in modo testo (video_force_text_mode nel kernel) invece di
 * lasciare la GUI congelata. Lancialo da DobFiles (aprendo il .mdl) con il
 * desktop attivo: il kernel andra' in panic e la schermata rossa deve
 * comparire. Il ritorno da main non avviene: kpanic non torna. */

#include <sys/syscall.h>
#include <stdio.h>

int main(void)
{
    printf("[panictest] Invoco SYS_PANIC_TEST: il kernel andra' in panic ORA.\n");
    printf("[panictest] La schermata di panic (rossa, modo testo) deve comparire.\n");

    (void)syscall0(SYS_PANIC_TEST);

    /* Mai raggiunto: la syscall non ritorna (kpanic -> halt). */
    for (;;)
    {
    }
    return 0;
}
