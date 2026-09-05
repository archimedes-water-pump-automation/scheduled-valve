#pragma once

#include <stdbool.h>

/* Configures the relay drive and leaves the valve closed. */
void valve_init(void);

void valve_open(void);
void valve_close(void);
bool valve_is_open(void);
