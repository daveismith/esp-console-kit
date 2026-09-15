/*
 * Console commands for the servos. Discovery is the application's: it knows which boards
 * are on which bus and what their channels are called, and hands register_servo() the
 * function that attaches them. That is what `servo_register` runs, and what every other
 * command runs once if nothing has been attached yet.
 */
#include "cmd_servo.h"
#include "servo.h"

#include <mutex>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "Servo.hpp"
#include "ServoManager.hpp"

static servo_attach_fn_t s_attach;
static std::mutex s_attach_lock;
static bool s_probed = false;   /* a command has already asked for the boards once */

/*
 * One attach if nothing has looked yet, and never again after that. Re-probing on every
 * command meant a log line about the board that is NOT fitted each time one was typed;
 * picking up a board plugged in later is servo_register's job. The boot path normally
 * attaches before any command is typed, in which case there are servos and nothing to do.
 */
static void ensure_attached(void)
{
    {
        std::lock_guard<std::mutex> guard(s_attach_lock);
        if (s_probed) {
            return;
        }
        s_probed = true;
    }
    if (s_attach != nullptr && servo::ServoManager::GetInstance()->Servos().empty()) {
        s_attach();
    }
}

/* Every command starts here: hardware on first use, and a message when there is none. */
static servo::Servo *find_servo(const char *ident)
{
    ensure_attached();
    servo::Servo *servo = servo::ServoManager::GetInstance()->GetServo(ident);
    if (servo == nullptr) {
        printf("no servo '%s' -- 'servo_list' shows what there is\n", ident);
    }
    return servo;
}

/* servo_list [-o wide] */
static struct {
    struct arg_str *output;
    struct arg_end *end;
} list_args;

static int list_servos(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&list_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, list_args.end, argv[0]);
        return 1;
    }
    const bool wide = (list_args.output->count > 0) && (strcmp(list_args.output->sval[0], "wide") == 0);

    ensure_attached();
    auto servos = servo::ServoManager::GetInstance()->Servos();
    if (servos.empty()) {
        printf("no servos: no board answered -- 'servo_register' looks again\n");
        return 0;
    }

    if (wide) {
        printf("+---------+------+------+------+------+------+--------+------------------+\n");
        printf("| servo   |absMin|absMax| opMin| opMax|invert| output | policy c/o/m ms  |\n");
        printf("+---------+------+------+------+------+------+--------+------------------+\n");
    }
    for (const auto &entry : servos) {
        if (!wide) {
            printf("%s\n", entry.first.c_str());
            continue;
        }
        uint16_t absMin, absMax, opMin, opMax;
        bool invert;
        entry.second->GetLimits(&absMin, &absMax, &opMin, &opMax, &invert);
        const servo::DrivePolicy &p = entry.second->Policy();
        printf("| %-7s | %4u | %4u | %4u | %4u | %-4s | %-6s | %c/%c/%c %5u ms |\n",
               entry.first.c_str(), absMin, absMax, opMin, opMax, invert ? "yes" : "no",
               entry.second->Driven() ? "driven" : entry.second->Released() ? "RELEAS" : "off",
               p.closed == servo::Drive::Release ? 'R' : 'H', p.open == servo::Drive::Release ? 'R' : 'H',
               p.mid == servo::Drive::Release ? 'R' : 'H', (unsigned)p.settleMs);
    }
    if (wide) {
        printf("+---------+------+------+------+------+------+--------+------------------+\n");
    }
    return 0;
}

/* servo_register: attach again, for a board plugged in after boot. */
static int attach_servos(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_attach == nullptr) {
        printf("this application has not said what is on the bus\n");
        return 1;
    }
    esp_err_t err = s_attach();
    if (err == ESP_ERR_INVALID_STATE) {
        printf("no I2C master bus yet\n");
        return 1;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        printf("no board answered\n");
        return 1;
    }
    printf("%u servos registered\n", (unsigned)servo::ServoManager::GetInstance()->Servos().size());
    return 0;
}

/* servo_move <ident> <position> [-p] */
static struct {
    struct arg_str *ident;
    struct arg_int *position;
    struct arg_lit *percentage;
    struct arg_end *end;
} move_args;

static int move_servo(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&move_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, move_args.end, argv[0]);
        return 1;
    }

    servo::Servo *servo = find_servo(move_args.ident->sval[0]);
    if (servo == nullptr) {
        return 1;
    }

    const int position = move_args.position->ival[0];
    servo::ServoManager *manager = servo::ServoManager::GetInstance();
    if (move_args.percentage->count > 0) {
        if (position < 0 || position > 1000) {
            printf("a percentage is 0..1000, in tenths\n");
            return 1;
        }
        servo->Enable();
        const uint16_t us = servo->MoveToPercentage((uint16_t)position);
        printf("%s: %d/1000 -> %u us\n", move_args.ident->sval[0], position, us);
    } else {
        uint16_t absMin, absMax;
        servo->GetLimits(&absMin, &absMax, NULL, NULL, NULL);
        if (position < absMin || position > absMax) {
            printf("%s takes %u..%u us\n", move_args.ident->sval[0], absMin, absMax);
            return 1;
        }
        servo->Enable();
        servo->MoveToValue((uint16_t)position);
        printf("%s: %d us\n", move_args.ident->sval[0], position);
    }
    manager->Update();
    return 0;
}

/* servo_sweep <ident>... [-t seconds]: back and forth across the working range. */
static struct {
    struct arg_str *idents;
    struct arg_int *seconds;
    struct arg_end *end;
} sweep_args;

static int sweep_servo(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&sweep_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, sweep_args.end, argv[0]);
        return 1;
    }

    std::vector<servo::Servo *> servos;
    for (int i = 0; i < sweep_args.idents->count; i++) {
        servo::Servo *servo = find_servo(sweep_args.idents->sval[i]);
        if (servo == nullptr) {
            return 1;
        }
        servos.push_back(servo);
    }

    const int seconds = (sweep_args.seconds->count > 0) ? sweep_args.seconds->ival[0] : 10;
    if (seconds <= 0 || seconds > 600) {
        printf("seconds is 1..600\n");
        return 1;
    }

    for (servo::Servo *servo : servos) {
        servo->Enable();
    }
    printf("sweeping %u servo%s for %d s\n", (unsigned)servos.size(), servos.size() == 1 ? "" : "s", seconds);

    /* One frame per step: 20 ms, which is also how often the servo task flushes. A full
     * traverse of the range takes a second. */
    const int frames = seconds * 50;
    int position = 500;
    int step = 20;
    TickType_t lastWake = xTaskGetTickCount();
    for (int frame = 0; frame < frames; frame++) {
        for (servo::Servo *servo : servos) {
            servo->MoveToPercentage((uint16_t)position);
        }
        if (position + step > 1000 || position + step < 0) {
            step = -step;
        }
        position += step;
        xTaskDelayUntil(&lastWake, pdMS_TO_TICKS(20));
    }

    for (servo::Servo *servo : servos) {
        servo->MoveToPercentage(500);
    }
    printf("done, back at centre\n");
    return 0;
}

/* servo_config <ident> <min> <max> [-i] */
static struct {
    struct arg_str *ident;
    struct arg_int *op_min;
    struct arg_int *op_max;
    struct arg_lit *invert;
    struct arg_end *end;
} config_args;

static int config_servo(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&config_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, config_args.end, argv[0]);
        return 1;
    }

    servo::Servo *servo = find_servo(config_args.ident->sval[0]);
    if (servo == nullptr) {
        return 1;
    }

    const int min = config_args.op_min->ival[0];
    const int max = config_args.op_max->ival[0];
    uint16_t absMin, absMax;
    servo->GetLimits(&absMin, &absMax, NULL, NULL, NULL);
    if (min < absMin || max > absMax || min > max) {
        printf("%s takes a range within %u..%u us\n", config_args.ident->sval[0], absMin, absMax);
        return 1;
    }
    if (!servo->SetOpLimits((uint16_t)min, (uint16_t)max, config_args.invert->count > 0)) {
        printf("could not save the range\n");
        return 1;
    }
    printf("%s: %d..%d us%s\n", config_args.ident->sval[0], min, max,
           (config_args.invert->count > 0) ? ", inverted" : "");
    return 0;
}

/* servo_off <ident>... | all: stop driving -- the servos go limp until their next move. */
static struct {
    struct arg_str *idents;
    struct arg_end *end;
} off_args;

static int off_servo(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&off_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, off_args.end, argv[0]);
        return 1;
    }

    ensure_attached();
    std::vector<std::string> idents;
    if (off_args.idents->count == 1 && strcmp(off_args.idents->sval[0], "all") == 0) {
        for (const auto &entry : servo::ServoManager::GetInstance()->Servos()) {
            idents.push_back(entry.first);
        }
    } else {
        for (int i = 0; i < off_args.idents->count; i++) {
            if (find_servo(off_args.idents->sval[i]) == nullptr) {
                return 1;
            }
            idents.push_back(off_args.idents->sval[i]);
        }
    }
    /* servo_set_enable() puts it on the wire at once, rather than at the next frame. */
    for (const std::string &ident : idents) {
        servo_set_enable(ident.c_str(), false);
        printf("%s: off\n", ident.c_str());
    }
    return 0;
}

/* Every esp_console_cmd_t here names all seven members. In C the two unused ones could
 * be left out; this is C++, where GCC's -Wmissing-field-initializers fires on a
 * designated initializer too, and IDF builds with -Werror. */
extern "C" void register_servo(servo_attach_fn_t attach)
{
    s_attach = attach;

    list_args.output = arg_str0("o", "output", "<list|wide>", "output format; wide adds the limits");
    list_args.end = arg_end(1);
    const esp_console_cmd_t list_servos_cmd = {
        .command = "servo_list",
        .help = "List the servos",
        .hint = NULL,
        .func = &list_servos,
        .argtable = &list_args,
        .func_w_context = NULL,
        .context = NULL,
    };

    const esp_console_cmd_t attach_servos_cmd = {
        .command = "servo_register",
        .help = "Attach the boards the application expects, for one plugged in after boot",
        .hint = NULL,
        .func = &attach_servos,
        .argtable = NULL,
        .func_w_context = NULL,
        .context = NULL,
    };

    move_args.ident = arg_str1(NULL, NULL, "<ident>", "servo, as listed by servo_list");
    move_args.position = arg_int1(NULL, NULL, "<position>", "pulse width in us, or 0..1000 with -p");
    move_args.percentage = arg_lit0("p", "percentage", "position is tenths of a percent of the working range");
    move_args.end = arg_end(3);
    const esp_console_cmd_t move_servo_cmd = {
        .command = "servo_move",
        .help = "Enable a servo and move it",
        .hint = NULL,
        .func = &move_servo,
        .argtable = &move_args,
        .func_w_context = NULL,
        .context = NULL,
    };

    sweep_args.idents = arg_strn(NULL, NULL, "<ident>", 1, 32, "servos to sweep");
    sweep_args.seconds = arg_int0("t", "seconds", "<n>", "how long to sweep for (default 10)");
    sweep_args.end = arg_end(3);
    const esp_console_cmd_t sweep_servo_cmd = {
        .command = "servo_sweep",
        .help = "Sweep servos across their working range",
        .hint = NULL,
        .func = &sweep_servo,
        .argtable = &sweep_args,
        .func_w_context = NULL,
        .context = NULL,
    };

    config_args.ident = arg_str1(NULL, NULL, "<ident>", "servo, as listed by servo_list");
    config_args.op_min = arg_int1(NULL, NULL, "<min>", "working range minimum, us");
    config_args.op_max = arg_int1(NULL, NULL, "<max>", "working range maximum, us");
    config_args.invert = arg_lit0("i", "invert", "0% is <max> rather than <min>");
    config_args.end = arg_end(4);
    const esp_console_cmd_t config_servo_cmd = {
        .command = "servo_config",
        .help = "Set and save a servo's working range",
        .hint = NULL,
        .func = &config_servo,
        .argtable = &config_args,
        .func_w_context = NULL,
        .context = NULL,
    };

    off_args.idents = arg_strn(NULL, NULL, "<ident>|all", 1, 32, "servos to stop driving, or all of them");
    off_args.end = arg_end(2);
    const esp_console_cmd_t off_servo_cmd = {
        .command = "servo_off",
        .help = "Stop driving servos: they go limp until their next move",
        .hint = NULL,
        .func = &off_servo,
        .argtable = &off_args,
        .func_w_context = NULL,
        .context = NULL,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&list_servos_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&attach_servos_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&move_servo_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&sweep_servo_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&config_servo_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&off_servo_cmd));
}
