#include "parking_plate_policy.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define PLATE_CODEPOINT_CAPACITY (PARKING_PLATE_MAX_CHARACTERS + 1U)

typedef struct {
    uint32_t values[PLATE_CODEPOINT_CAPACITY];
    size_t count;
} plate_codepoints_t;

static int decode_one_utf8(const unsigned char **cursor, uint32_t *value)
{
    const unsigned char *text;
    uint32_t codepoint;
    size_t continuation_count;
    size_t i;

    if (cursor == NULL || *cursor == NULL || value == NULL)
        return -1;

    text = *cursor;
    if (text[0] == '\0')
        return 0;

    if (text[0] <= 0x7FU) {
        *value = text[0];
        *cursor = text + 1;
        return 1;
    }

    if (text[0] >= 0xC2U && text[0] <= 0xDFU) {
        codepoint = (uint32_t)(text[0] & 0x1FU);
        continuation_count = 1U;
    } else if (text[0] >= 0xE0U && text[0] <= 0xEFU) {
        codepoint = (uint32_t)(text[0] & 0x0FU);
        continuation_count = 2U;
    } else if (text[0] >= 0xF0U && text[0] <= 0xF4U) {
        codepoint = (uint32_t)(text[0] & 0x07U);
        continuation_count = 3U;
    } else {
        return -1;
    }

    for (i = 1U; i <= continuation_count; ++i) {
        if (text[i] == '\0' || (text[i] & 0xC0U) != 0x80U)
            return -1;
        codepoint = (codepoint << 6U) | (uint32_t)(text[i] & 0x3FU);
    }

    if ((continuation_count == 1U && codepoint < 0x80U) ||
        (continuation_count == 2U && codepoint < 0x800U) ||
        (continuation_count == 3U && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        codepoint > 0x10FFFFU)
        return -1;

    *value = codepoint;
    *cursor = text + continuation_count + 1U;
    return 1;
}

static int decode_plate(const char *text, plate_codepoints_t *plate)
{
    const unsigned char *cursor;

    if (text == NULL || plate == NULL)
        return -1;

    memset(plate, 0, sizeof(*plate));
    cursor = (const unsigned char *)text;
    while (*cursor != '\0') {
        uint32_t codepoint;
        int decoded;

        if (plate->count >= PLATE_CODEPOINT_CAPACITY)
            return -1;

        decoded = decode_one_utf8(&cursor, &codepoint);
        if (decoded <= 0)
            return -1;

        plate->values[plate->count++] = codepoint;
    }

    return 0;
}

static int string_contains_codepoint(const char *text, uint32_t target)
{
    const unsigned char *cursor;

    cursor = (const unsigned char *)text;
    while (*cursor != '\0') {
        uint32_t codepoint;

        if (decode_one_utf8(&cursor, &codepoint) <= 0)
            return 0;
        if (codepoint == target)
            return 1;
    }

    return 0;
}

static int is_ascii_letter(uint32_t codepoint)
{
    return codepoint >= (uint32_t)'A' && codepoint <= (uint32_t)'Z' &&
           codepoint != (uint32_t)'I' && codepoint != (uint32_t)'O';
}

static int is_ascii_digit(uint32_t codepoint)
{
    return codepoint >= (uint32_t)'0' && codepoint <= (uint32_t)'9';
}

static int is_valid_prefix(uint32_t codepoint)
{
    static const char prefixes[] =
        "京沪津渝冀晋蒙辽吉黑苏浙皖闽赣鲁豫鄂湘粤桂琼川贵云"
        "藏陕甘青宁新使领民航";

    return string_contains_codepoint(prefixes, codepoint);
}

static int is_valid_suffix(uint32_t codepoint)
{
    static const char special_suffixes[] = "学警港澳挂使领民航危险品";

    return is_ascii_letter(codepoint) || is_ascii_digit(codepoint) ||
           string_contains_codepoint(special_suffixes, codepoint);
}

static int plate_format_valid(const plate_codepoints_t *plate)
{
    size_t i;

    if (plate == NULL ||
        plate->count < PARKING_PLATE_MIN_CHARACTERS ||
        plate->count > PARKING_PLATE_MAX_CHARACTERS)
        return 0;

    if (!is_valid_prefix(plate->values[0]) ||
        !is_ascii_letter(plate->values[1]))
        return 0;

    for (i = 2U; i < plate->count; ++i) {
        if (!is_valid_suffix(plate->values[i]))
            return 0;
    }

    return 1;
}

static size_t minimum3(size_t first, size_t second, size_t third)
{
    size_t minimum = first < second ? first : second;

    return minimum < third ? minimum : third;
}

static size_t edit_distance(const plate_codepoints_t *left,
                            const plate_codepoints_t *right)
{
    size_t previous[PLATE_CODEPOINT_CAPACITY];
    size_t current[PLATE_CODEPOINT_CAPACITY];
    size_t i;
    size_t j;

    for (j = 0U; j <= right->count; ++j)
        previous[j] = j;

    for (i = 1U; i <= left->count; ++i) {
        current[0] = i;
        for (j = 1U; j <= right->count; ++j) {
            size_t substitution_cost =
                left->values[i - 1U] == right->values[j - 1U] ? 0U : 1U;

            current[j] = minimum3(previous[j] + 1U,
                                  current[j - 1U] + 1U,
                                  previous[j - 1U] + substitution_cost);
        }

        memcpy(previous, current, (right->count + 1U) * sizeof(previous[0]));
    }

    return previous[right->count];
}

int parking_plate_policy_evaluate(
    const char *plate_number,
    float confidence,
    const char *active_plate,
    int lane_idle,
    parking_plate_policy_result_t *result)
{
    plate_codepoints_t candidate;
    plate_codepoints_t active;

    if (plate_number == NULL || result == NULL ||
        (lane_idle != 0 && lane_idle != 1))
        return -1;

    memset(result, 0, sizeof(*result));
    result->decision = PARKING_PLATE_REJECT_INVALID;
    result->edit_distance = SIZE_MAX;

    if (!isfinite(confidence) || confidence < 0.0F || confidence > 1.0F ||
        decode_plate(plate_number, &candidate) != 0) {
        return 0;
    }

    result->character_count = candidate.count;
    if (!plate_format_valid(&candidate))
        return 0;

    if (lane_idle) {
        result->decision = PARKING_PLATE_ACCEPT;
        return 0;
    }

    if (active_plate == NULL || active_plate[0] == '\0' ||
        decode_plate(active_plate, &active) != 0 ||
        !plate_format_valid(&active)) {
        result->decision = PARKING_PLATE_REJECT_BUSY;
        return 0;
    }

    result->edit_distance = edit_distance(&candidate, &active);
    if (result->edit_distance == 0U)
        result->decision = PARKING_PLATE_IGNORE_DUPLICATE;
    else if (result->edit_distance <= PARKING_PLATE_AMBIGUOUS_DISTANCE)
        result->decision = PARKING_PLATE_REJECT_AMBIGUOUS;
    else
        result->decision = PARKING_PLATE_REJECT_BUSY;

    return 0;
}

const char *parking_plate_decision_name(parking_plate_decision_t decision)
{
    switch (decision) {
        case PARKING_PLATE_ACCEPT:
            return "ACCEPT";
        case PARKING_PLATE_IGNORE_DUPLICATE:
            return "IGNORE_DUPLICATE";
        case PARKING_PLATE_REJECT_INVALID:
            return "REJECT_INVALID";
        case PARKING_PLATE_REJECT_AMBIGUOUS:
            return "REJECT_AMBIGUOUS";
        case PARKING_PLATE_REJECT_BUSY:
            return "REJECT_BUSY";
        default:
            return "UNKNOWN";
    }
}
