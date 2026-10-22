#pragma once

#include <DeviceNetwork.h>

const WifiCredential WIFI_CREDENTIALS[] = {
	{"first-wifi-ssid", "first-wifi-password"},
	{"second-wifi-ssid", "second-wifi-password"},
};

const char SUPABASE_URL[] = "https://your-project-ref.supabase.co";
const char SUPABASE_KEY[] = "sb_publishable_your-project-key";
const char DEVICE_ID[] = "esp32-relay-01";
const char DEVICE_REGISTRY_TOKEN[] = "replace-with-a-random-device-token";
const char DEVICE_AUTH_EMAIL[] = "esp32-relay-01@devices.local";
const char DEVICE_AUTH_PASSWORD[] = "replace-with-a-unique-device-password";