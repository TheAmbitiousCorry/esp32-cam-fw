#pragma once

// Starts two servers: the page and stills on port 80, the stream on 81.
bool startWebServers();

// Frees both servers so an OTA write is not competing with a live stream.
void stopWebServers();
