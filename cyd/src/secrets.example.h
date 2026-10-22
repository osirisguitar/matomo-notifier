// Copy to src/secrets.h (gitignored) and fill in your values
#pragma once

#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"
// The matomo-notifier backend, no trailing slash. https:// is verified against
// the Let's Encrypt roots in letsencrypt_roots.h; plain http:// works for a
// local dev backend (http://<host running it>:3001)
#define API_BASE "https://matomo-notifier.int.bornholm.se"
