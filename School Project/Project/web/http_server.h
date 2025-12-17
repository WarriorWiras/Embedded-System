#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "sd_card.h"
#include <stdbool.h>

// HTTP server functions
bool http_server_init(void);
void http_server_set_file_list(sd_file_info_t *files, int *file_count, bool *needs_refresh);

#endif // HTTP_SERVER_H
