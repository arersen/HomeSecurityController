//
// Created by jambyl on 10/13/26.
//
#include "board_config.h"

const int used_pins[] = { BUZZER_GPIO_OFFSET, RADAR_GPIO_OFFSET, TEST_GPIO_COLLISION_OFFSET };

void check_pins_or_die(void) {
    size_t count = sizeof(used_pins) / sizeof(used_pins[0]);

    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (used_pins[i] == used_pins[j]) {
                fprintf(stderr, "FATAL ERROR: Duplicate GPIO pin detected (Pin %d)\n", used_pins[i]);
                exit(EXIT_FAILURE);
            }
        }
    }
}

