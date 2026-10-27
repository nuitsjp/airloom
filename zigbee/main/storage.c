#include "storage.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SEGMENTS 8
#define SEGMENT_RECORDS 512
#define RECORD_MAGIC UINT32_C(0x41495231)
#define HAS_TEMPERATURE 1U
#define HAS_HUMIDITY 2U
#define IS_TEMPERATURE 4U
#define SYNCED_EPOCH INT64_C(1735689600)

/* Fixed on-flash format, versioned independently from the firmware. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t sequence;
    uint32_t boot_id;
    uint32_t flags;
    int64_t recorded_us;
    int64_t epoch_seconds;
    int64_t temperature_received_us;
    int64_t humidity_received_us;
    char sensor_id[24];
    float temperature_c;
    float humidity_percent;
    uint32_t checksum;
    uint32_t reserved;
} record_t;
_Static_assert(sizeof(record_t) == 96, "history record layout changed");

typedef struct {
    unsigned count;
    uint64_t first;
    uint64_t last;
} segment_t;

static const char *TAG = "airloom_storage";
static segment_t segments[SEGMENTS];
static SemaphoreHandle_t file_lock;
static QueueHandle_t queue;
static airloom_snapshot_fn snapshot_fn;
static record_t disk_latest;
static record_t disk_temperature;
static record_t disk_humidity;
static unsigned history_count;
static uint32_t boot_id;
static atomic_bool healthy;
static bool mounted;

static void segment_path(char path[48], unsigned index)
{
    snprintf(path, 48, "/history/segment-%u.bin", index);
}

static uint32_t checksum(const record_t *record)
{
    const unsigned char *bytes = (const unsigned char *)record;
    uint32_t value = UINT32_C(2166136261);
    for (size_t i = 0; i < offsetof(record_t, checksum); ++i)
        value = (value ^ bytes[i]) * UINT32_C(16777619);
    return value;
}

static bool valid_record(const record_t *record)
{
    return record->magic == RECORD_MAGIC && record->version == 1 &&
        record->sequence != 0 && record->checksum == checksum(record) &&
        memchr(record->sensor_id, '\0', sizeof(record->sensor_id)) != NULL &&
        record->recorded_us >= 0 &&
        (!(record->flags & HAS_TEMPERATURE) || isfinite(record->temperature_c)) &&
        (!(record->flags & HAS_HUMIDITY) || isfinite(record->humidity_percent));
}

static bool partition_is_blank(const esp_partition_t *partition)
{
    unsigned char bytes[1024];
    for (size_t offset = 0; offset < partition->size; offset += sizeof bytes) {
        size_t count = partition->size - offset;
        if (count > sizeof bytes) count = sizeof bytes;
        if (esp_partition_read(partition, offset, bytes, count) != ESP_OK) return false;
        for (size_t i = 0; i < count; ++i) if (bytes[i] != 0xff) return false;
    }
    return true;
}

static bool scan_segments(void)
{
    for (unsigned index = 0; index < SEGMENTS; ++index) {
        char path[48];
        segment_path(path, index);
        FILE *file = fopen(path, "rb");
        if (!file) {
            if (errno == ENOENT) continue;
            ESP_LOGE(TAG, "cannot open history segment %u", index);
            return false;
        }
        record_t record;
        size_t size;
        bool valid = true;
        while ((size = fread(&record, 1, sizeof record, file)) == sizeof record) {
            segment_t *segment = &segments[index];
            if (!valid_record(&record) || segment->count >= SEGMENT_RECORDS ||
                (segment->count && record.sequence != segment->last + 1)) {
                ESP_LOGE(TAG, "corrupt history segment %u; preserving contents", index);
                valid = false;
                break;
            }
            if (!segment->count) segment->first = record.sequence;
            segment->last = record.sequence;
            ++segment->count;
            ++history_count;
            if (record.sequence > disk_latest.sequence) disk_latest = record;
            if ((record.flags & HAS_TEMPERATURE) && record.sequence > disk_temperature.sequence)
                disk_temperature = record;
            if ((record.flags & HAS_HUMIDITY) && record.sequence > disk_humidity.sequence)
                disk_humidity = record;
        }
        if (ferror(file)) valid = false;
        if (fclose(file)) valid = false;
        if (!valid) return false;
        if (size != 0) {
            ESP_LOGW(TAG, "discarding incomplete final record in segment %u", index);
            if (truncate(path, (off_t)segments[index].count * sizeof record)) return false;
        }
    }
    /* Overlapping sequences would make ordering ambiguous after a damaged rotation. */
    for (unsigned a = 0; a < SEGMENTS; ++a)
        for (unsigned b = a + 1; b < SEGMENTS; ++b)
            if (segments[a].count && segments[b].count &&
                segments[a].first <= segments[b].last && segments[b].first <= segments[a].last)
                return false;
    return true;
}

static bool write_record(FILE *file, const record_t *record)
{
    bool written = fwrite(record, sizeof *record, 1, file) == 1;
    if (written && fflush(file)) written = false;
    if (written && fsync(fileno(file))) written = false;
    if (fclose(file)) written = false;
    return written;
}

/* Called only with file_lock held. */
static bool append_record(record_t *record)
{
    unsigned current = 0;
    for (unsigned index = 1; index < SEGMENTS; ++index)
        if (segments[index].last > segments[current].last) current = index;
    bool rotate = !segments[current].count || segments[current].count == SEGMENT_RECORDS;
    if (rotate) {
        for (unsigned index = 0; index < SEGMENTS; ++index) {
            if (!segments[index].count) { current = index; break; }
            if (segments[index].first < segments[current].first) current = index;
        }
    }
    record->sequence = disk_latest.sequence + 1;
    record->checksum = checksum(record);
    char path[48];
    segment_path(path, current);
    const char *write_path = rotate ? "/history/pending.bin" : path;
    FILE *file = fopen(write_path, rotate ? "wb" : "ab");
    if (!file || !write_record(file, record)) return false;
    if (rotate) {
        if (rename(write_path, path)) return false;
        history_count -= segments[current].count;
        segments[current] = (segment_t){.first = record->sequence};
    }
    ++segments[current].count;
    segments[current].last = record->sequence;
    ++history_count;
    disk_latest = *record;
    if (record->flags & HAS_TEMPERATURE) disk_temperature = *record;
    if (record->flags & HAS_HUMIDITY) disk_humidity = *record;
    return true;
}

static void storage_task(void *arg)
{
    (void)arg;
    record_t record;
    for (;;) {
        if (xQueueReceive(queue, &record, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(file_lock, portMAX_DELAY);
        bool saved = append_record(&record);
        xSemaphoreGive(file_lock);
        if (!saved) {
            atomic_store(&healthy, false);
            ESP_LOGE(TAG, "history write failed; storage worker stopped");
            vTaskDelete(NULL);
        }
    }
}

bool storage_start(airloom_snapshot_fn snapshot)
{
    snapshot_fn = snapshot;
    boot_id = esp_random();
    file_lock = xSemaphoreCreateMutex();
    if (!snapshot || !file_lock) return false;
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY, "history");
    if (!partition) {
        ESP_LOGE(TAG, "history partition missing");
        return false;
    }
    bool blank = partition_is_blank(partition);
    const esp_vfs_littlefs_conf_t config = {
        .base_path = "/history", .partition_label = "history",
        /* Formatting is permitted only after checking every byte of this partition. */
        .format_if_mount_failed = blank,
    };
    esp_err_t error = esp_vfs_littlefs_register(&config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "history mount failed: %s; existing contents preserved", esp_err_to_name(error));
        return false;
    }
    mounted = true;
    if (!scan_segments()) {
        ESP_LOGE(TAG, "history scan failed; refusing to overwrite existing records");
        return false;
    }
    queue = xQueueCreate(16, sizeof(record_t));
    if (!queue) return false;
    atomic_store(&healthy, true);
    if (xTaskCreate(storage_task, "history", 4096, NULL, 3, NULL) != pdPASS) {
        atomic_store(&healthy, false);
        return false;
    }
    ESP_LOGI(TAG, "history ready; retained=%u capacity=%u boot=%08" PRIx32,
             history_count, SEGMENTS * SEGMENT_RECORDS, boot_id);
    return true;
}

void storage_record(const airloom_snapshot_t *snapshot, bool temperature)
{
    if (!snapshot || !queue || !atomic_load(&healthy)) return;
    time_t epoch = time(NULL);
    record_t record = {
        .magic = RECORD_MAGIC, .version = 1, .boot_id = boot_id,
        .flags = (snapshot->has_temperature ? HAS_TEMPERATURE : 0) |
                 (snapshot->has_humidity ? HAS_HUMIDITY : 0) |
                 (temperature ? IS_TEMPERATURE : 0),
        .recorded_us = esp_timer_get_time(),
        .epoch_seconds = epoch >= SYNCED_EPOCH ? (int64_t)epoch : 0,
        .temperature_received_us = snapshot->temperature_received_us,
        .humidity_received_us = snapshot->humidity_received_us,
        .temperature_c = snapshot->temperature_c,
        .humidity_percent = snapshot->humidity_percent,
    };
    memcpy(record.sensor_id, snapshot->sensor_id, sizeof record.sensor_id);
    if (xQueueSend(queue, &record, 0) != pdTRUE) {
        atomic_store(&healthy, false);
        ESP_LOGE(TAG, "history queue full; at least one sensor report was not saved");
    }
}

static bool add_nullable_number(cJSON *json, const char *name, bool present, double value)
{
    return (present ? cJSON_AddNumberToObject(json, name, value) :
                      cJSON_AddNullToObject(json, name)) != NULL;
}

static cJSON *record_json(const record_t *record)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    char boot[9];
    snprintf(boot, sizeof boot, "%08" PRIx32, record->boot_id);
    if (!cJSON_AddStringToObject(json, "sensor_id", record->sensor_id) ||
        !cJSON_AddStringToObject(json, "boot_id", boot) ||
        !cJSON_AddNumberToObject(json, "received_uptime_seconds", record->recorded_us / 1000000.0) ||
        !add_nullable_number(json, "received_at", record->epoch_seconds != 0, record->epoch_seconds) ||
        !add_nullable_number(json, "temperature_c", record->flags & HAS_TEMPERATURE, record->temperature_c) ||
        !add_nullable_number(json, "humidity_percent", record->flags & HAS_HUMIDITY, record->humidity_percent) ||
        !cJSON_AddStringToObject(json, "kind", record->flags & IS_TEMPERATURE ? "temperature" : "humidity")) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static bool saved_age(const record_t *record, int64_t received_us, int64_t uptime,
                      time_t epoch, double *age)
{
    if (record->boot_id == boot_id) {
        *age = (uptime - received_us) / 1000000.0;
        return *age >= 0;
    }
    if (record->epoch_seconds < SYNCED_EPOCH || epoch < record->epoch_seconds) return false;
    *age = difftime(epoch, (time_t)record->epoch_seconds) + (record->recorded_us - received_us) / 1000000.0;
    return *age >= 0;
}

cJSON *storage_status_json(void)
{
    record_t saved = {0};
    record_t saved_temperature = {0};
    record_t saved_humidity = {0};
    unsigned count = 0;
    if (file_lock) {
        xSemaphoreTake(file_lock, portMAX_DELAY);
        saved = disk_latest;
        saved_temperature = disk_temperature;
        saved_humidity = disk_humidity;
        count = history_count;
        xSemaphoreGive(file_lock);
    }
    airloom_snapshot_t current = {0};
    bool live = snapshot_fn && snapshot_fn(&current) && (current.has_temperature || current.has_humidity);
    time_t epoch = time(NULL);
    int64_t uptime = esp_timer_get_time();
    cJSON *status = cJSON_CreateObject();
    if (!status) return NULL;
    char boot[9];
    snprintf(boot, sizeof boot, "%08" PRIx32, boot_id);
    if (!cJSON_AddStringToObject(status, "boot_id", boot) ||
        !cJSON_AddNumberToObject(status, "uptime_seconds", uptime / 1000000.0) ||
        !cJSON_AddNumberToObject(status, "free_heap_bytes", esp_get_free_heap_size()) ||
        !cJSON_AddNumberToObject(status, "minimum_free_heap_bytes", esp_get_minimum_free_heap_size()) ||
        !cJSON_AddBoolToObject(status, "storage_ok", atomic_load(&healthy)) ||
        !cJSON_AddNumberToObject(status, "history_count", count) ||
        !cJSON_AddNumberToObject(status, "history_capacity", SEGMENTS * SEGMENT_RECORDS) ||
        !cJSON_AddBoolToObject(status, "time_synchronized", epoch >= SYNCED_EPOCH)) goto failed;
    if (!live && !saved.sequence) {
        if (!cJSON_AddNullToObject(status, "latest")) goto failed;
        return status;
    }
    cJSON *latest = cJSON_AddObjectToObject(status, "latest");
    if (!latest) goto failed;
    const char *sensor_id = live ? current.sensor_id : saved.sensor_id;
    bool live_temperature = live && current.has_temperature;
    bool live_humidity = live && current.has_humidity;
    bool saved_temperature_matches = saved_temperature.sequence && !strcmp(saved_temperature.sensor_id, sensor_id);
    bool saved_humidity_matches = saved_humidity.sequence && !strcmp(saved_humidity.sensor_id, sensor_id);
    bool has_temperature = live_temperature || saved_temperature_matches;
    bool has_humidity = live_humidity || saved_humidity_matches;
    bool previous = (!live_temperature && saved_temperature_matches && saved_temperature.boot_id != boot_id) ||
                    (!live_humidity && saved_humidity_matches && saved_humidity.boot_id != boot_id);
    double temperature_age = 0, humidity_age = 0;
    bool temperature_age_known = live_temperature;
    bool humidity_age_known = live_humidity;
    if (live_temperature) temperature_age = (uptime - current.temperature_received_us) / 1000000.0;
    else if (saved_temperature_matches)
        temperature_age_known = saved_age(&saved_temperature, saved_temperature.temperature_received_us, uptime, epoch, &temperature_age);
    if (live_humidity) humidity_age = (uptime - current.humidity_received_us) / 1000000.0;
    else if (saved_humidity_matches)
        humidity_age_known = saved_age(&saved_humidity, saved_humidity.humidity_received_us, uptime, epoch, &humidity_age);
    if (!cJSON_AddStringToObject(latest, "sensor_id", sensor_id) ||
        !add_nullable_number(latest, "temperature_c", has_temperature, live_temperature ? current.temperature_c : saved_temperature.temperature_c) ||
        !add_nullable_number(latest, "humidity_percent", has_humidity, live_humidity ? current.humidity_percent : saved_humidity.humidity_percent) ||
        !add_nullable_number(latest, "temperature_age_seconds", has_temperature && temperature_age_known, temperature_age) ||
        !add_nullable_number(latest, "humidity_age_seconds", has_humidity && humidity_age_known, humidity_age) ||
        !cJSON_AddBoolToObject(latest, "from_previous_boot", previous)) goto failed;
    return status;
failed:
    cJSON_Delete(status);
    return NULL;
}

cJSON *storage_history_json(unsigned limit, unsigned offset)
{
    if (limit == 0 || limit > 64 || offset > SEGMENTS * SEGMENT_RECORDS) return NULL;
    cJSON *array = cJSON_CreateArray();
    if (!array) return NULL;
    if (!file_lock || !mounted) return array;
    xSemaphoreTake(file_lock, portMAX_DELAY);
    unsigned order[SEGMENTS];
    for (unsigned i = 0; i < SEGMENTS; ++i) order[i] = i;
    for (unsigned i = 0; i < SEGMENTS; ++i)
        for (unsigned j = i + 1; j < SEGMENTS; ++j)
            if (segments[order[j]].first < segments[order[i]].first) {
                unsigned swap = order[i]; order[i] = order[j]; order[j] = swap;
            }
    unsigned end = history_count > offset ? history_count - offset : 0;
    unsigned start = end > limit ? end - limit : 0;
    unsigned base = 0;
    bool ok = true;
    for (unsigned i = 0; i < SEGMENTS && base < end && ok; ++i) {
        unsigned index = order[i];
        unsigned segment_end = base + segments[index].count;
        if (start < segment_end && end > base) {
            unsigned first = start > base ? start - base : 0;
            unsigned last = end < segment_end ? end - base : segments[index].count;
            char path[48];
            segment_path(path, index);
            FILE *file = fopen(path, "rb");
            if (!file) { ok = false; break; }
            if (fseek(file, (long)(first * sizeof(record_t)), SEEK_SET)) ok = false;
            for (unsigned n = first; n < last && ok; ++n) {
                record_t record;
                if (fread(&record, sizeof record, 1, file) != 1 || !valid_record(&record)) {
                    atomic_store(&healthy, false);
                    ESP_LOGE(TAG, "history read failed or checksum mismatch");
                    ok = false;
                    break;
                }
                cJSON *json = record_json(&record);
                if (!json) { ok = false; break; }
                if (!cJSON_AddItemToArray(array, json)) {
                    cJSON_Delete(json);
                    ok = false;
                }
            }
            if (fclose(file)) ok = false;
        }
        base = segment_end;
    }
    xSemaphoreGive(file_lock);
    if (!ok) { cJSON_Delete(array); return NULL; }
    return array;
}
