/* Executes the production file/rotation/recovery implementation on host files. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

static const char *local_path(const char *path)
{
    assert(strncmp(path, "/history/", 9) == 0);
    return path + 9;
}

static FILE *test_fopen(const char *path, const char *mode)
{
    return fopen(local_path(path), mode);
}

static int test_rename(const char *old_path, const char *new_path)
{
#if defined(_WIN32)
    /* The host CRT cannot replace an existing target; LittleFS/POSIX can. */
    remove(local_path(new_path));
#endif
    return rename(local_path(old_path), local_path(new_path));
}

static int test_truncate(const char *path, off_t size)
{
    FILE *file = fopen(local_path(path), "r+b");
    if (!file) return -1;
#if defined(_WIN32)
    int result = (int)_chsize_s(_fileno(file), (long long)size);
#else
    int result = ftruncate(fileno(file), (off_t)size);
#endif
    return fclose(file) ? -1 : result;
}

#define fopen test_fopen
#define rename test_rename
#define truncate test_truncate
#include "../main/storage.c"
#undef fopen
#undef rename
#undef truncate

static record_t sample(void)
{
    record_t record = {
        .magic = RECORD_MAGIC, .version = 1, .boot_id = 0x1234,
        .flags = HAS_TEMPERATURE | HAS_HUMIDITY | IS_TEMPERATURE,
        .recorded_us = 5000000, .epoch_seconds = 1800000000,
        .temperature_received_us = 5000000, .humidity_received_us = 4000000,
        .sensor_id = "a4c138e6ca181aea", .temperature_c = 22.5f, .humidity_percent = 62.5f,
    };
    return record;
}

static void reset_state(void)
{
    memset(segments, 0, sizeof segments);
    memset(&disk_latest, 0, sizeof disk_latest);
    memset(&disk_temperature, 0, sizeof disk_temperature);
    memset(&disk_humidity, 0, sizeof disk_humidity);
    history_count = 0;
}

static unsigned newest_segment(void)
{
    unsigned newest = 0;
    for (unsigned i = 1; i < SEGMENTS; ++i)
        if (segments[i].last > segments[newest].last) newest = i;
    return newest;
}

int main(void)
{
    record_t record = sample();
    for (unsigned i = 1; i <= SEGMENTS * SEGMENT_RECORDS; ++i) {
        record.recorded_us += 1000000;
        assert(append_record(&record));
        assert(history_count == i);
        assert(disk_latest.sequence == i);
    }
    assert(append_record(&record));
    assert(history_count == 3585);
    assert(disk_latest.sequence == 4097);

    /* A pending replacement that was never renamed must not replace committed data. */
    FILE *pending = fopen("pending.bin", "wb");
    assert(pending);
    record.sequence = 99999;
    record.checksum = checksum(&record);
    assert(fwrite(&record, sizeof record, 1, pending) == 1);
    assert(!fclose(pending));
    reset_state();
    assert(scan_segments());
    assert(history_count == 3585);
    assert(disk_latest.sequence == 4097);
    assert(disk_latest.temperature_c == 22.5f);
    assert(disk_latest.humidity_percent == 62.5f);
    unsigned oldest = 0;
    for (unsigned i = 0; i < SEGMENTS; ++i)
        if (!oldest || segments[i].first < oldest) oldest = (unsigned)segments[i].first;
    assert(oldest == 513);

    /* A power interruption halfway through the final record preserves complete records. */
    char path[48];
    segment_path(path, newest_segment());
    FILE *file = test_fopen(path, "ab");
    assert(file);
    assert(fwrite(&record, 1, sizeof record / 2, file) == sizeof record / 2);
    assert(!fclose(file));
    reset_state();
    assert(scan_segments());
    assert(history_count == 3585 && disk_latest.sequence == 4097);
    struct stat info;
    assert(!stat(local_path(path), &info));
    assert(info.st_size == (long long)sizeof(record_t));
    assert(append_record(&record));
    assert(disk_latest.sequence == 4098 && history_count == 3586);

    /* A complete but damaged record is reported, never formatted or overwritten. */
    file = test_fopen(path, "r+b");
    assert(file);
    assert(!fseek(file, (long)offsetof(record_t, temperature_c), SEEK_SET));
    float damaged = 99.0f;
    assert(fwrite(&damaged, sizeof damaged, 1, file) == 1);
    assert(!fclose(file));
    assert(!stat(local_path(path), &info));
    long long original_size = info.st_size;
    reset_state();
    assert(!scan_segments());
    assert(!stat(local_path(path), &info) && info.st_size == original_size);

    /* A new-boot humidity report must not erase the previous boot's temperature. */
    for (unsigned i = 0; i < SEGMENTS; ++i) {
        segment_path(path, i);
        remove(local_path(path));
    }
    reset_state();
    record = sample();
    record.flags = HAS_TEMPERATURE | IS_TEMPERATURE;
    assert(append_record(&record));
    reset_state();
    assert(scan_segments());
    assert(disk_temperature.sequence == 1);
    record.flags = HAS_HUMIDITY;
    record.boot_id = 0x5678;
    record.temperature_c = 0;
    record.temperature_received_us = 0;
    assert(append_record(&record));
    assert(disk_temperature.sequence == 1 && disk_humidity.sequence == 2);
    reset_state();
    assert(scan_segments());
    assert(disk_latest.sequence == 2);
    assert(disk_temperature.sequence == 1 && disk_temperature.temperature_c == 22.5f);
    assert(disk_humidity.sequence == 2 && disk_humidity.humidity_percent == 62.5f);
    boot_id = 0x9999;
    double age;
    assert(saved_age(&disk_temperature, disk_temperature.temperature_received_us,
                     0, (time_t)1800000010, &age) && age == 10);
    assert(!saved_age(&disk_temperature, disk_temperature.temperature_received_us,
                      0, (time_t)0, &age));
    puts("storage rotation, reboot restore, pending rotation, partial tail, corruption, sparse temperature tests passed");
    return 0;
}
