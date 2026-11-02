// The WireGuard test's settings - copy to secrets.h (git ignores it: it
// holds a private key, a WiFi password and the VPN's address) and fill in.
#pragma once
// An allowed WiFi network to join - a phone's hotspot, for the car's path.
#define T_WIFI_SSID     ""
#define T_WIFI_PASS     ""
// This gauge as a WireGuard peer: `wg genkey` - its `wg pubkey` goes on the
// VPN server as the peer's key.
#define T_WG_PRIVATE    ""
#define T_WG_ADDRESS    10,0,0,0        // the peer's tunnel IP, as commas
// The VPN server
#define T_WG_ENDPOINT   ""              // its static IP (or name)
#define T_WG_PORT       51820
#define T_WG_PEER_PUB   ""              // the VPN server's public key
// Something to fetch on the server, at its LAN address, over plain HTTP
#define T_TEST_URL      "http://192.168.1.10:8080/"
