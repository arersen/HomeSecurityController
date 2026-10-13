//
// Created by jambyl on 10/13/26.
//
#include "etc.h"

void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}