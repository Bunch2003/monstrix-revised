// Copy this file to secrets.h (same folder) and fill in your own values.
// secrets.h is listed in .gitignore so it never gets uploaded to GitHub.

#pragma once

// Wi-Fi (must be a 2.4 GHz network - the ESP32-C3 can't use 5 GHz)
const char* WIFI_SSID     = "your-wifi-name";
const char* WIFI_PASSWORD = "your-wifi-password";

// Your location, for sunset/sunrise (west longitude is negative)
#define MY_LATITUDE  0.0
#define MY_LONGITUDE 0.0

// Password for wireless updates (Arduino IDE network port and /update page)
#define OTA_PASSWORD "pick-a-password"

// 1 = Monstrix, 2 = Nova. Only used the first time a board gets this
// firmware over USB; after that each board remembers who it is.
#define WHICH_ROBOT 1

// Set to 1 for one upload to force a board to take WHICH_ROBOT, then back to 0.
#define SET_IDENTITY_ON_UPLOAD 0
