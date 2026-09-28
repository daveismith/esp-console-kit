/*
 * What web_server.c shares with the rest of the component, and nobody else.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_http_server.h"

/* The server, while it runs; NULL otherwise. */
httpd_handle_t web_server_handle(void);

/* The request's Origin, when the CORS allowlist has it. */
bool web_cors_origin(httpd_req_t *req, char *out, size_t len);

/* web_events.c: GET /api/v1/events; the server starting and stopping; a connection closing. */
esp_err_t web_events_get(httpd_req_t *req);
void web_events_init(void);
void web_events_started(void);
void web_events_stopped(void);
void web_events_closed(int fd);
