#include "parking_protocol.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} json_builder_t;

static size_t bounded_string_length(const char *text, size_t maximum)
{
    size_t length = 0;

    if (text == NULL)
        return maximum + 1U;

    while (length <= maximum && text[length] != '\0')
        ++length;

    return length;
}

static int valid_utf8(const char *text, size_t length)
{
    const unsigned char *data = (const unsigned char *)text;
    size_t i = 0;

    while (i < length) {
        unsigned char first = data[i];

        if (first <= 0x7FU) {
            ++i;
            continue;
        }

        if (first >= 0xC2U && first <= 0xDFU) {
            if (i + 1U >= length ||
                (data[i + 1U] & 0xC0U) != 0x80U)
                return 0;
            i += 2U;
            continue;
        }

        if (first >= 0xE0U && first <= 0xEFU) {
            unsigned char second;

            if (i + 2U >= length)
                return 0;
            second = data[i + 1U];
            if ((second & 0xC0U) != 0x80U ||
                (data[i + 2U] & 0xC0U) != 0x80U ||
                (first == 0xE0U && second < 0xA0U) ||
                (first == 0xEDU && second > 0x9FU))
                return 0;
            i += 3U;
            continue;
        }

        if (first >= 0xF0U && first <= 0xF4U) {
            unsigned char second;

            if (i + 3U >= length)
                return 0;
            second = data[i + 1U];
            if ((second & 0xC0U) != 0x80U ||
                (data[i + 2U] & 0xC0U) != 0x80U ||
                (data[i + 3U] & 0xC0U) != 0x80U ||
                (first == 0xF0U && second < 0x90U) ||
                (first == 0xF4U && second > 0x8FU))
                return 0;
            i += 4U;
            continue;
        }

        return 0;
    }

    return 1;
}

static int valid_text(const char *text, size_t maximum, int allow_empty)
{
    size_t length = bounded_string_length(text, maximum);

    if (length > maximum || (!allow_empty && length == 0U))
        return 0;

    return valid_utf8(text, length);
}

static int valid_topic_level(const char *text)
{
    size_t length;
    size_t i;

    length = bounded_string_length(
        text, PARKING_PROTOCOL_TOPIC_LEVEL_MAX_LENGTH);
    if (length == 0U ||
        length > PARKING_PROTOCOL_TOPIC_LEVEL_MAX_LENGTH)
        return 0;

    for (i = 0U; i < length; ++i) {
        unsigned char character = (unsigned char)text[i];

        if ((character >= (unsigned char)'a' &&
             character <= (unsigned char)'z') ||
            (character >= (unsigned char)'A' &&
             character <= (unsigned char)'Z') ||
            (character >= (unsigned char)'0' &&
             character <= (unsigned char)'9') ||
            character == (unsigned char)'.' ||
            character == (unsigned char)'_' ||
            character == (unsigned char)'-')
            continue;

        return 0;
    }

    return 1;
}

static void builder_terminate(json_builder_t *builder)
{
    size_t index;

    if (builder->buffer == NULL || builder->capacity == 0U)
        return;

    index = builder->length < builder->capacity
        ? builder->length : builder->capacity - 1U;
    builder->buffer[index] = '\0';
}

static void builder_append_memory(json_builder_t *builder,
                                  const char *text,
                                  size_t length)
{
    size_t available = 0U;
    size_t copy_length;

    if (builder->buffer != NULL && builder->capacity > 0U &&
        builder->length < builder->capacity - 1U)
        available = builder->capacity - 1U - builder->length;

    copy_length = length < available ? length : available;
    if (copy_length > 0U)
        memcpy(builder->buffer + builder->length, text, copy_length);

    builder->length += length;
    builder_terminate(builder);
}

static void builder_append_text(json_builder_t *builder, const char *text)
{
    builder_append_memory(builder, text, strlen(text));
}

static void builder_append_character(json_builder_t *builder, char character)
{
    builder_append_memory(builder, &character, 1U);
}

static void builder_append_uint64(json_builder_t *builder, uint64_t value)
{
    char number[32];
    int length;

    length = snprintf(number, sizeof(number), "%llu",
                      (unsigned long long)value);
    if (length > 0)
        builder_append_memory(builder, number, (size_t)length);
}

static void builder_append_float(json_builder_t *builder, float value)
{
    char number[32];
    size_t length;
    int result;

    result = snprintf(number, sizeof(number), "%.6f", (double)value);
    if (result <= 0)
        return;

    length = (size_t)result;
    while (length > 1U && number[length - 1U] == '0')
        --length;
    if (length > 1U && number[length - 1U] == '.')
        --length;

    builder_append_memory(builder, number, length);
}

static void builder_append_json_string(json_builder_t *builder,
                                       const char *text)
{
    const unsigned char *data = (const unsigned char *)text;
    size_t length = strlen(text);
    size_t start = 0U;
    size_t i;

    builder_append_character(builder, '"');
    for (i = 0U; i < length; ++i) {
        const char *escape = NULL;
        char unicode_escape[7];

        switch (data[i]) {
            case '"':
                escape = "\\\"";
                break;
            case '\\':
                escape = "\\\\";
                break;
            case '\b':
                escape = "\\b";
                break;
            case '\f':
                escape = "\\f";
                break;
            case '\n':
                escape = "\\n";
                break;
            case '\r':
                escape = "\\r";
                break;
            case '\t':
                escape = "\\t";
                break;
            default:
                break;
        }

        if (escape == NULL && data[i] < 0x20U) {
            snprintf(unicode_escape, sizeof(unicode_escape),
                     "\\u%04x", (unsigned int)data[i]);
            escape = unicode_escape;
        }

        if (escape != NULL) {
            if (i > start)
                builder_append_memory(builder, text + start, i - start);
            builder_append_text(builder, escape);
            start = i + 1U;
        }
    }

    if (length > start)
        builder_append_memory(builder, text + start, length - start);
    builder_append_character(builder, '"');
}

static parking_protocol_status_t finish_builder(
    json_builder_t *builder,
    size_t *required_size)
{
    if (required_size != NULL)
        *required_size = builder->length + 1U;

    builder_terminate(builder);
    if (builder->capacity <= builder->length)
        return PARKING_PROTOCOL_BUFFER_TOO_SMALL;

    return PARKING_PROTOCOL_OK;
}

static int valid_output(char *buffer, size_t capacity)
{
    return buffer != NULL || capacity == 0U;
}

static int valid_metadata(const parking_protocol_metadata_t *metadata)
{
    if (metadata == NULL || metadata->timestamp_unix_us == 0U ||
        !valid_text(metadata->message_id,
                    PARKING_PROTOCOL_ID_MAX_LENGTH, 0) ||
        !valid_topic_level(metadata->site_id) ||
        !valid_topic_level(metadata->device_id) ||
        !valid_text(metadata->session_id,
                    PARKING_PROTOCOL_ID_MAX_LENGTH, 0))
        return 0;

    return 1;
}

static int valid_spaces(const parking_protocol_spaces_t *spaces)
{
    uint64_t sum;

    if (spaces == NULL)
        return 0;

    sum = (uint64_t)spaces->occupied + spaces->pending_entries +
          spaces->available;
    return sum == spaces->total;
}

static const char *topic_suffix(parking_protocol_topic_t topic)
{
    switch (topic) {
        case PARKING_PROTOCOL_TOPIC_AVAILABILITY:
            return "availability";
        case PARKING_PROTOCOL_TOPIC_HEARTBEAT:
            return "heartbeat";
        case PARKING_PROTOCOL_TOPIC_STATUS:
            return "status";
        case PARKING_PROTOCOL_TOPIC_EVENT_ENTRY:
            return "events/entry";
        case PARKING_PROTOCOL_TOPIC_EVENT_EXIT:
            return "events/exit";
        case PARKING_PROTOCOL_TOPIC_COMMAND_REQUEST:
            return "commands/request";
        case PARKING_PROTOCOL_TOPIC_COMMAND_REPLY:
            return "commands/reply";
        default:
            return NULL;
    }
}

static const char *availability_name(parking_protocol_availability_t state)
{
    switch (state) {
        case PARKING_PROTOCOL_OFFLINE:
            return "offline";
        case PARKING_PROTOCOL_ONLINE:
            return "online";
        default:
            return NULL;
    }
}

static const char *lane_mode_name(parking_protocol_lane_mode_t mode)
{
    switch (mode) {
        case PARKING_PROTOCOL_LANE_ENTRY:
            return "entry";
        case PARKING_PROTOCOL_LANE_EXIT:
            return "exit";
        default:
            return NULL;
    }
}

static const char *business_state_name(
    parking_protocol_business_state_t state)
{
    switch (state) {
        case PARKING_PROTOCOL_BUSINESS_INITIALIZING:
            return "initializing";
        case PARKING_PROTOCOL_BUSINESS_IDLE:
            return "idle";
        case PARKING_PROTOCOL_BUSINESS_CHECKING:
            return "checking";
        case PARKING_PROTOCOL_BUSINESS_WAIT_PAYMENT:
            return "wait_payment";
        case PARKING_PROTOCOL_BUSINESS_OPENING:
            return "opening";
        case PARKING_PROTOCOL_BUSINESS_WAIT_PASSAGE:
            return "wait_passage";
        case PARKING_PROTOCOL_BUSINESS_CLOSING:
            return "closing";
        case PARKING_PROTOCOL_BUSINESS_COMPLETING:
            return "completing";
        case PARKING_PROTOCOL_BUSINESS_REJECT_COOLDOWN:
            return "reject_cooldown";
        case PARKING_PROTOCOL_BUSINESS_ERROR:
            return "error";
        default:
            return NULL;
    }
}

static const char *gate_mode_name(parking_protocol_gate_mode_t mode)
{
    switch (mode) {
        case PARKING_PROTOCOL_GATE_DRY_RUN:
            return "dry_run";
        case PARKING_PROTOCOL_GATE_REAL:
            return "real";
        default:
            return NULL;
    }
}

static const char *gate_state_name(parking_protocol_gate_state_t state)
{
    switch (state) {
        case PARKING_PROTOCOL_GATE_CLOSED:
            return "closed";
        case PARKING_PROTOCOL_GATE_OPENING:
            return "opening";
        case PARKING_PROTOCOL_GATE_OPEN:
            return "open";
        case PARKING_PROTOCOL_GATE_CLOSING:
            return "closing";
        case PARKING_PROTOCOL_GATE_ERROR:
            return "error";
        default:
            return NULL;
    }
}

static const char *component_state_name(
    parking_protocol_component_state_t state)
{
    switch (state) {
        case PARKING_PROTOCOL_COMPONENT_OK:
            return "ok";
        case PARKING_PROTOCOL_COMPONENT_UNAVAILABLE:
            return "unavailable";
        case PARKING_PROTOCOL_COMPONENT_ERROR:
            return "error";
        default:
            return NULL;
    }
}

static const char *passage_source_name(
    parking_protocol_passage_source_t source)
{
    switch (source) {
        case PARKING_PROTOCOL_PASSAGE_SENSOR:
            return "sensor";
        case PARKING_PROTOCOL_PASSAGE_MANUAL:
            return "manual";
        case PARKING_PROTOCOL_PASSAGE_VISION_TIMEOUT:
            return "vision_timeout";
        default:
            return NULL;
    }
}

static const char *vehicle_event_name(
    parking_protocol_vehicle_event_type_t type)
{
    switch (type) {
        case PARKING_PROTOCOL_EVENT_ENTRY:
            return "vehicle_entry";
        case PARKING_PROTOCOL_EVENT_EXIT:
            return "vehicle_exit";
        default:
            return NULL;
    }
}

static void append_metadata(json_builder_t *builder,
                            const parking_protocol_metadata_t *metadata,
                            const char *type)
{
    builder_append_text(builder, "{\"schema_version\":");
    builder_append_uint64(builder, PARKING_PROTOCOL_SCHEMA_VERSION);
    builder_append_text(builder, ",\"type\":");
    builder_append_json_string(builder, type);
    builder_append_text(builder, ",\"message_id\":");
    builder_append_json_string(builder, metadata->message_id);
    builder_append_text(builder, ",\"timestamp_unix_us\":");
    builder_append_uint64(builder, metadata->timestamp_unix_us);
    builder_append_text(builder, ",\"site_id\":");
    builder_append_json_string(builder, metadata->site_id);
    builder_append_text(builder, ",\"device_id\":");
    builder_append_json_string(builder, metadata->device_id);
    builder_append_text(builder, ",\"session_id\":");
    builder_append_json_string(builder, metadata->session_id);
}

static void append_spaces(json_builder_t *builder,
                          const parking_protocol_spaces_t *spaces)
{
    builder_append_text(builder, "\"total\":");
    builder_append_uint64(builder, spaces->total);
    builder_append_text(builder, ",\"occupied\":");
    builder_append_uint64(builder, spaces->occupied);
    builder_append_text(builder, ",\"pending_entries\":");
    builder_append_uint64(builder, spaces->pending_entries);
    builder_append_text(builder, ",\"available\":");
    builder_append_uint64(builder, spaces->available);
}

parking_protocol_status_t parking_protocol_build_topic(
    const char *site_id,
    const char *device_id,
    parking_protocol_topic_t topic,
    char *buffer,
    size_t capacity,
    size_t *required_size)
{
    const char *suffix = topic_suffix(topic);
    json_builder_t builder;

    if (!valid_output(buffer, capacity))
        return PARKING_PROTOCOL_BAD_ARG;
    if (!valid_topic_level(site_id) || !valid_topic_level(device_id) ||
        suffix == NULL)
        return PARKING_PROTOCOL_INVALID_VALUE;

    builder.buffer = buffer;
    builder.capacity = capacity;
    builder.length = 0U;
    builder_terminate(&builder);
    builder_append_text(&builder, PARKING_PROTOCOL_TOPIC_PREFIX "/");
    builder_append_text(&builder, site_id);
    builder_append_character(&builder, '/');
    builder_append_text(&builder, device_id);
    builder_append_character(&builder, '/');
    builder_append_text(&builder, suffix);
    return finish_builder(&builder, required_size);
}

parking_protocol_status_t parking_protocol_encode_availability(
    const parking_protocol_availability_message_t *message,
    char *buffer,
    size_t capacity,
    size_t *required_size)
{
    const char *state;
    json_builder_t builder;

    if (message == NULL || !valid_output(buffer, capacity))
        return PARKING_PROTOCOL_BAD_ARG;

    state = availability_name(message->state);
    if (!valid_metadata(&message->metadata) || state == NULL)
        return PARKING_PROTOCOL_INVALID_VALUE;

    builder.buffer = buffer;
    builder.capacity = capacity;
    builder.length = 0U;
    builder_terminate(&builder);
    append_metadata(&builder, &message->metadata, "availability");
    builder_append_text(&builder, ",\"state\":");
    builder_append_json_string(&builder, state);
    builder_append_character(&builder, '}');
    return finish_builder(&builder, required_size);
}

parking_protocol_status_t parking_protocol_encode_heartbeat(
    const parking_protocol_heartbeat_message_t *message,
    char *buffer,
    size_t capacity,
    size_t *required_size)
{
    json_builder_t builder;

    if (message == NULL || !valid_output(buffer, capacity))
        return PARKING_PROTOCOL_BAD_ARG;
    if (!valid_metadata(&message->metadata))
        return PARKING_PROTOCOL_INVALID_VALUE;

    builder.buffer = buffer;
    builder.capacity = capacity;
    builder.length = 0U;
    builder_terminate(&builder);
    append_metadata(&builder, &message->metadata, "heartbeat");
    builder_append_text(&builder, ",\"sequence\":");
    builder_append_uint64(&builder, message->sequence);
    builder_append_text(&builder, ",\"uptime_us\":");
    builder_append_uint64(&builder, message->uptime_us);
    builder_append_character(&builder, '}');
    return finish_builder(&builder, required_size);
}

parking_protocol_status_t parking_protocol_encode_status(
    const parking_protocol_status_message_t *message,
    char *buffer,
    size_t capacity,
    size_t *required_size)
{
    const char *lane_mode;
    const char *business_state;
    const char *gate_mode;
    const char *gate_state;
    const char *camera_state;
    const char *recognizer_state;
    const char *database_state;
    json_builder_t builder;

    if (message == NULL || !valid_output(buffer, capacity))
        return PARKING_PROTOCOL_BAD_ARG;

    lane_mode = lane_mode_name(message->lane_mode);
    business_state = business_state_name(message->business_state);
    gate_mode = gate_mode_name(message->gate_mode);
    gate_state = gate_state_name(message->gate_state);
    camera_state = component_state_name(message->camera_state);
    recognizer_state = component_state_name(message->recognizer_state);
    database_state = component_state_name(message->database_state);
    if (!valid_metadata(&message->metadata) ||
        !valid_text(message->software_version,
                    PARKING_PROTOCOL_VERSION_MAX_LENGTH, 0) ||
        !valid_spaces(&message->spaces) || lane_mode == NULL ||
        business_state == NULL || gate_mode == NULL || gate_state == NULL ||
        camera_state == NULL || recognizer_state == NULL ||
        database_state == NULL)
        return PARKING_PROTOCOL_INVALID_VALUE;

    builder.buffer = buffer;
    builder.capacity = capacity;
    builder.length = 0U;
    builder_terminate(&builder);
    append_metadata(&builder, &message->metadata, "device_status");
    builder_append_text(&builder, ",\"software_version\":");
    builder_append_json_string(&builder, message->software_version);
    builder_append_text(&builder, ",\"lane_mode\":");
    builder_append_json_string(&builder, lane_mode);
    builder_append_text(&builder, ",\"business_state\":");
    builder_append_json_string(&builder, business_state);
    builder_append_text(&builder, ",\"gate\":{\"mode\":");
    builder_append_json_string(&builder, gate_mode);
    builder_append_text(&builder, ",\"state\":");
    builder_append_json_string(&builder, gate_state);
    builder_append_text(&builder, "},\"components\":{\"camera\":");
    builder_append_json_string(&builder, camera_state);
    builder_append_text(&builder, ",\"recognizer\":");
    builder_append_json_string(&builder, recognizer_state);
    builder_append_text(&builder, ",\"database\":");
    builder_append_json_string(&builder, database_state);
    builder_append_text(&builder, "},\"spaces\":{");
    append_spaces(&builder, &message->spaces);
    builder_append_text(&builder, "}}");
    return finish_builder(&builder, required_size);
}

parking_protocol_status_t parking_protocol_encode_vehicle_event(
    const parking_protocol_vehicle_event_t *event,
    char *buffer,
    size_t capacity,
    size_t *required_size)
{
    const char *event_name;
    const char *lane_mode;
    const char *passage_source;
    json_builder_t builder;

    if (event == NULL || !valid_output(buffer, capacity))
        return PARKING_PROTOCOL_BAD_ARG;

    event_name = vehicle_event_name(event->event_type);
    lane_mode = event->event_type == PARKING_PROTOCOL_EVENT_ENTRY
        ? "entry" : event->event_type == PARKING_PROTOCOL_EVENT_EXIT
            ? "exit" : NULL;
    passage_source = passage_source_name(event->passage_source);
    if (!valid_metadata(&event->metadata) || event_name == NULL ||
        lane_mode == NULL || passage_source == NULL ||
        event->operation_id == 0U ||
        !valid_text(event->plate_number,
                    PARKING_PROTOCOL_PLATE_MAX_LENGTH, 0) ||
        !valid_text(event->plate_color,
                    PARKING_PROTOCOL_COLOR_MAX_LENGTH, 0) ||
        !isfinite(event->detect_confidence) ||
        event->detect_confidence < 0.0F ||
        event->detect_confidence > 1.0F ||
        (event->event_type == PARKING_PROTOCOL_EVENT_ENTRY &&
         event->amount_cents != 0U) ||
        !valid_spaces(&event->spaces))
        return PARKING_PROTOCOL_INVALID_VALUE;

    builder.buffer = buffer;
    builder.capacity = capacity;
    builder.length = 0U;
    builder_terminate(&builder);
    append_metadata(&builder, &event->metadata, event_name);
    builder_append_text(&builder, ",\"operation_id\":");
    builder_append_uint64(&builder, event->operation_id);
    builder_append_text(&builder, ",\"lane_mode\":");
    builder_append_json_string(&builder, lane_mode);
    builder_append_text(&builder, ",\"plate_number\":");
    builder_append_json_string(&builder, event->plate_number);
    builder_append_text(&builder, ",\"plate_color\":");
    builder_append_json_string(&builder, event->plate_color);
    builder_append_text(&builder, ",\"detect_confidence\":");
    builder_append_float(&builder, event->detect_confidence);
    builder_append_text(&builder, ",\"amount_cents\":");
    builder_append_uint64(&builder, event->amount_cents);
    builder_append_text(&builder, ",\"passage_source\":");
    builder_append_json_string(&builder, passage_source);
    builder_append_text(&builder, ",\"result\":\"committed\",\"spaces\":{");
    append_spaces(&builder, &event->spaces);
    builder_append_text(&builder, "}}");
    return finish_builder(&builder, required_size);
}

const char *parking_protocol_status_name(parking_protocol_status_t status)
{
    switch (status) {
        case PARKING_PROTOCOL_OK:
            return "OK";
        case PARKING_PROTOCOL_BAD_ARG:
            return "BAD_ARG";
        case PARKING_PROTOCOL_INVALID_VALUE:
            return "INVALID_VALUE";
        case PARKING_PROTOCOL_BUFFER_TOO_SMALL:
            return "BUFFER_TOO_SMALL";
        default:
            return "UNKNOWN";
    }
}
