#pragma once

#include <Arduino.h>
#include "esp_http_server.h"

// Session tokens live in RAM only, so a reboot signs everyone out. That is the
// right default for a device that reboots on firmware update: a stolen cookie
// cannot outlive the box it was taken from.
String authCreateSession();
bool authIsSignedIn(httpd_req_t *req);
void authEndSession(httpd_req_t *req);

// Sends a redirect to the login page and returns false when not signed in, so
// handlers can guard with a single line.
bool authGuardPage(httpd_req_t *req);

// Same check for endpoints a browser fetches rather than navigates to, where a
// 401 is more useful than a redirect to an HTML page.
bool authGuardResource(httpd_req_t *req);

String authSessionCookie(const String &token);
String authClearCookie();
