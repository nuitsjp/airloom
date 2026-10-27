#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "snapshot.h"

bool storage_start(airloom_snapshot_fn snapshot);
/* Copies to a nonblocking queue; never writes flash on the Zigbee thread. */
void storage_record(const airloom_snapshot_t *snapshot, bool temperature);
cJSON *storage_status_json(void);
/* Offset counts backwards from the newest record; results are oldest first. */
cJSON *storage_history_json(unsigned limit, unsigned offset);
