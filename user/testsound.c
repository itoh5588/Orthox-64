#include <stdio.h>
#include <stdint.h>
#include "syscall.h"

int main(void) {
    printf("sound test start\n");

    uint32_t notes[] = {440, 660, 880, 660, 440};
    for (unsigned i = 0; i < sizeof(notes) / sizeof(notes[0]); i++) {
        sound_on(notes[i]);
        sleep_ms(120);
        sound_off();
        sleep_ms(60);
    }

    printf("sound test end\n");
    return 0;
}
