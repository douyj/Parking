#include "parking_controller.h"
#include "parking_plate_policy.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void expect_decision(const char *plate,
                            float confidence,
                            const char *active_plate,
                            int lane_idle,
                            parking_plate_decision_t expected)
{
    parking_plate_policy_result_t result;

    assert(parking_plate_policy_evaluate(
               plate, confidence, active_plate, lane_idle, &result) == 0);
    if (result.decision != expected) {
        fprintf(stderr, "plate=%s expected=%s actual=%s\n",
                plate,
                parking_plate_decision_name(expected),
                parking_plate_decision_name(result.decision));
        assert(result.decision == expected);
    }
}

static void verify_controller_integration(void)
{
    parking_controller_config_t config;
    parking_controller_t *controller;
    parking_command_list_t commands;
    parking_event_t event;

    memset(&config, 0, sizeof(config));
    config.mode = PARKING_LANE_ENTRY;
    config.gate_initially_closed = 1;
    controller = parking_controller_create(&config);
    assert(controller != NULL);

    memset(&event, 0, sizeof(event));
    event.type = PARKING_EVENT_PLATE_CONFIRMED;
    event.occurred_at_us = 100U;
    event.data.plate.confidence = 0.90F;
    snprintf(event.data.plate.plate_number,
             sizeof(event.data.plate.plate_number), "%s", "粤A3333G");
    assert(parking_controller_handle_event(
               controller, &event, &commands) == 0);
    assert(commands.count == 1U);
    assert(commands.items[0].type == PARKING_COMMAND_CHECK_ENTRY);

    event.occurred_at_us = 200U;
    assert(parking_controller_handle_event(
               controller, &event, &commands) == 0);
    assert(commands.count == 0U);

    event.occurred_at_us = 300U;
    snprintf(event.data.plate.plate_number,
             sizeof(event.data.plate.plate_number), "%s", "粤A33338");
    assert(parking_controller_handle_event(
               controller, &event, &commands) == 0);
    assert(commands.count == 1U);
    assert(commands.items[0].type == PARKING_COMMAND_REJECT);
    assert(commands.items[0].reason == PARKING_REASON_AMBIGUOUS_PLATE);

    event.occurred_at_us = 400U;
    snprintf(event.data.plate.plate_number,
             sizeof(event.data.plate.plate_number), "%s", "粤A3333");
    assert(parking_controller_handle_event(
               controller, &event, &commands) == 0);
    assert(commands.count == 1U);
    assert(commands.items[0].type == PARKING_COMMAND_REJECT);
    assert(commands.items[0].reason == PARKING_REASON_INVALID_PLATE);

    parking_controller_destroy(controller);
}

int main(void)
{
    parking_plate_policy_result_t result;
    const char invalid_utf8[] = {'\xE7', '\xB2', '\0'};

    expect_decision("粤A3333G", 0.90F, "", 1, PARKING_PLATE_ACCEPT);
    expect_decision("粤AD12345", 0.90F, "", 1, PARKING_PLATE_ACCEPT);
    expect_decision("粤A3333", 0.90F, "", 1,
                    PARKING_PLATE_REJECT_INVALID);
    expect_decision("粤O3333G", 0.90F, "", 1,
                    PARKING_PLATE_REJECT_INVALID);
    expect_decision(invalid_utf8, 0.90F, "", 1,
                    PARKING_PLATE_REJECT_INVALID);
    expect_decision("粤A3333G", NAN, "", 1,
                    PARKING_PLATE_REJECT_INVALID);

    expect_decision("粤A3333G", 0.90F, "粤A3333G", 0,
                    PARKING_PLATE_IGNORE_DUPLICATE);
    expect_decision("粤A33338", 0.90F, "粤A3333G", 0,
                    PARKING_PLATE_REJECT_AMBIGUOUS);
    expect_decision("粤A3333", 0.90F, "粤A3333G", 0,
                    PARKING_PLATE_REJECT_INVALID);
    expect_decision("粤B99999", 0.90F, "粤A3333G", 0,
                    PARKING_PLATE_REJECT_BUSY);

    assert(parking_plate_policy_evaluate(
               "粤A33338", 0.90F, "粤A3333G", 0, &result) == 0);
    assert(result.edit_distance == 1U);

    verify_controller_integration();

    puts("parking_plate_policy: all tests passed");
    return 0;
}
