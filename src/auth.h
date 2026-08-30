#pragma once

#include <Arduino.h>
#include "esp_http_server.h"

// Sessions are stored on the device and survive a reboot, because this camera
// reboots on every settings change and every firmware update, and signing
// everyone out each time was friction rather than security on a home network.
// Changing the admin password ends them all.
String authCreateSession();
bool authIsSignedIn(httpd_req_t *req);
void authEndSession(httpd_req_t *req);

// Used when the admin password changes: everything issued under the old one goes.
void authEndAllSessions();

// Sends a redirect to the login page and returns false when not signed in, so
// handlers can guard with a single line.
bool authGuardPage(httpd_req_t *req);

// Same check for endpoints a browser fetches rather than navigates to, where a
// 401 is more useful than a redirect to an HTML page.
bool authGuardResource(httpd_req_t *req);

String authSessionCookie(const String &token);
String authClearCookie();
