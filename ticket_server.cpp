#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace std;

void fatal(string message) {
    printf("%s\n", message.);
    exit(1);
}

typedef struct Parameters {
    FILE *file_ptr;
    int port;
    int time_limit;
} Parameters;

typedef struct Event {
    char *description;
    u_int8_t tickets;
} Event;

Parameters parse_args(int argc, char *argv[]) {
    if (argc > 7 || argc < 3 || !(argc & 1)) {
        fatal("improper usage.");
    }

    FILE *file_ptr = NULL;
    int port = 2022;
    int time_limit = 5;

    bool file_set = false;
    bool port_set = false;
    bool time_set = false;

    for (int current_arg = 1; current_arg < argc; current_arg += 2) {
        if (strcmp(argv[current_arg], "-f") == 0) {
            if (file_set) {
                fatal("file already set.");
            }
            file_set = true;
            file_ptr = fopen(argv[current_arg + 1], "r");
            if (!file_ptr) {
                fatal("opening of file failed.");
            }
        }
        else if (strcmp(argv[current_arg], "-p") == 0) {
            if (port_set) {
                fatal("port already set.");
            }
            port_set = true;
            char *ptr;
            port = (int) strtol(argv[current_arg + 1], &ptr, 10);
            if (*ptr != '\0' || port < 1024 || port > 49151) {
                fatal("illegal port.");
            }
        }
        else if (strcmp(argv[current_arg], "-t") == 0) {
            if (time_set) {
                fatal("time already set.");
            }
            time_set = true;
            char *ptr;
            time_limit = (int) strtol(argv[current_arg + 1], &ptr, 10);
            if (*ptr != '\0' || time_limit < 1 || time_limit > 86400) {
                fatal("illegal time.");
            }
        }
        else {
            fatal("unexpected expression.");
        }
    }

    if (!file_ptr) {
        fatal("file not set.");
    }

    return (Parameters) { .file_ptr = file_ptr, .port = port, .time_limit = time_limit };
}

int main(int argc, char *argv[]) {
    Parameters parameters = parse_args(argc, argv);

    char *buff = NULL;
    size_t buff_len;
    char *description;
    int description_length;
    int tickets;

    while ((description_length = getline(&buff, &buff_len, parameters.file_ptr)) >= 0) {
        fprintf(stderr, "length: %d\n", description_length);
        description = (char *) malloc((size_t) description_length * sizeof(char));
        strcpy(description, buff);
        description[description_length - 1] = '\0';
        getline(&buff, &buff_len, parameters.file_ptr);
        tickets = (int) strtol(buff, NULL, 10);
        printf("%s: %d\n", description, tickets);
        free(description);
    }

    return 0;
}
