#include "radar.h"
#include <stdio.h>
#include <time.h>

e_gpio_status radar_init(gpio_t* radar, unsigned int offset) {
    printf("[RADAR] Init on GPIO offset %d\n", offset);
    e_gpio_status status = gpio_init(radar, "/dev/gpiochip0", offset, GPIO_DIR_INPUT, "hw456_radar");
    if (status != GPIO_OK) {
        fprintf(stderr, "[RADAR] Error: gpio_init failed\n");
    }
    return status;
}

void radar_deinit(gpio_t* radar) {
    gpio_deinit(radar);
    printf("[RADAR] Deinitialized\n");
}

int radar_read(gpio_t* radar) {
    return gpio_read(radar);
}

void radar_run_diagnostic(gpio_t* radar, int timeout_sec) {
    printf("\n=== Radar diagnostic started (%d sec) ===\n", timeout_sec);
    printf("Wave...\n");

    int last_state = -1;
    time_t start_time = time(NULL);

    while (time(NULL) - start_time < timeout_sec) {
        int current_state = radar_read(radar);

        if (current_state != last_state) {
            if (current_state == 1) {
                printf("[!] %ld: DETECTED MOVEMENT (HIGH)\n", time(NULL));
            } else if (current_state == 0) {
                printf("[*] %ld: CLEAR (LOW)\n", time(NULL));
            } else {
                fprintf(stderr, "[X] ERROR REEADING SENSOR!\n");
            }
            last_state = current_state;
        }

        sleep_ms(50);
    }
    printf("=== DIAGNOSTIC FINISHED ===\n\n");
}