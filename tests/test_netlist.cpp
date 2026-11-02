// ============================================================================
// test_netlist.cpp  —  the WiFi networks seen while parked (netlist.h),
// merged scan by scan.
// ============================================================================
#include "test.h"
#include "../firmware/car_gauge_v1/netlist.h"

static void a_network_seen_again_is_one_entry_with_its_best_signal() {
  NetList l = {};
  netlist_note(&l, "home", -70, false, 1000);
  netlist_note(&l, "home", -55, false, 2000);
  netlist_note(&l, "home", -80, false, 3000);
  CHECK_EQ(l.n, 1);
  CHECK_EQ(l.nets[0].rssi, -55);
  CHECK_EQ(l.nets[0].count, 3);
  CHECK_EQ(l.nets[0].lastSeen, 3000);
}

static void hidden_networks_are_not_listed() {
  NetList l = {};
  netlist_note(&l, "", -40, true, 1000);
  netlist_note(&l, nullptr, -40, true, 1000);
  CHECK_EQ(l.n, 0);
}

// With the clock unset (no GPS yet, no RTC), the last time it was seen stays.
static void no_clock_keeps_the_last_time() {
  NetList l = {};
  netlist_note(&l, "cafe", -60, true, 5000);
  netlist_note(&l, "cafe", -62, true, 0);
  CHECK_EQ(l.nets[0].lastSeen, 5000);
  CHECK_EQ(l.nets[0].open, 1);
}

static void full_the_least_seen_oldest_makes_way() {
  NetList l = {};
  for (int i = 0; i < NETLIST_MAX; i++) {
    char name[8];
    snprintf(name, sizeof(name), "n%d", i);
    netlist_note(&l, name, -70, false, 1000 + i);
  }
  netlist_note(&l, "n5", -70, false, 2000);       // seen twice: stays
  netlist_note(&l, "new", -50, false, 3000);      // replaces n0: once, oldest
  CHECK_EQ(l.n, NETLIST_MAX);
  bool haveN0 = false, haveN5 = false, haveNew = false;
  for (int i = 0; i < l.n; i++) {
    haveN0 |= strcmp(l.nets[i].ssid, "n0") == 0;
    haveN5 |= strcmp(l.nets[i].ssid, "n5") == 0;
    haveNew |= strcmp(l.nets[i].ssid, "new") == 0;
  }
  CHECK(!haveN0);
  CHECK(haveN5);
  CHECK(haveNew);
}

static void a_32_character_name_fits() {
  NetList l = {};
  const char *name = "abcdefghijklmnopqrstuvwxyz012345";   // 32, the most WiFi allows
  netlist_note(&l, name, -60, false, 1);
  CHECK_STREQ(l.nets[0].ssid, name);
}

int main() {
  printf("test_netlist\n");
  TEST(a_network_seen_again_is_one_entry_with_its_best_signal);
  TEST(hidden_networks_are_not_listed);
  TEST(no_clock_keeps_the_last_time);
  TEST(full_the_least_seen_oldest_makes_way);
  TEST(a_32_character_name_fits);
  TEST_SUMMARY();
}
