#pragma once

// First-run setup. Brings up an open access point and serves a page that writes
// the network and admin credentials into NVS, then reboots into normal running.
bool startSetupPortal();

// Drives the captive-portal DNS responder. Call every loop pass while the portal
// is up; does nothing otherwise.
void portalLoop();

// The SSID the camera is broadcasting, for printing at boot.
String portalApSsid();
