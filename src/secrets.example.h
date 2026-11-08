#pragma once

// Copy to src/secrets.h (git-ignored) and fill in.

// Wi-Fi is joined by WPS on first boot (or hold BOOT 3 s), then remembered.

// The Docker VM's LAN address and the host port published for Magpie's 3425.
#define MAGPIE_URL "http://192.168.1.20:3425"

// A read-only gateway key: reads quotas and usage, can't call any model.
// Don't add a budget, model or account limit (those keys get 403 on usage).
//   docker exec <container> magpie gateway-key add "Magpie Display" --read-only
#define MAGPIE_GATEWAY_KEY "sk-magpie-..."

// POSIX TZ for the on-screen clock: "UTC0", "CST-8" (UTC+8),
// "CET-1CEST,M3.5.0,M10.5.0/3", "PST8PDT,M3.2.0,M11.1.0".
#define TIMEZONE "EST5EDT,M3.2.0,M11.1.0"  // US Eastern, with DST
