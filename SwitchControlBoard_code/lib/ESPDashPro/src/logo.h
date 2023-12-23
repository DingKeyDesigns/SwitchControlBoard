#ifndef _DASH_LOGO_H
#define _DASH_LOGO_H

#include <Arduino.h>

#define DASH_MINI_LOGO_MIME         "image/png"
#define DASH_MINI_LOGO_GZIPPED      1
#define DASH_MINI_LOGO_WIDTH        27
#define DASH_MINI_LOGO_HEIGHT       25

extern const uint8_t DASH_MINI_LOGO[16092];


#define DASH_LARGE_LOGO_MIME        "image/png"
#define DASH_LARGE_LOGO_GZIPPED     1
#define DASH_LARGE_LOGO_WIDTH       108
#define DASH_LARGE_LOGO_HEIGHT      100

extern const uint8_t DASH_LARGE_LOGO[16092];

#endif