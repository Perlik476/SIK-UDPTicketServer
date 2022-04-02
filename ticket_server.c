#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include "err.h"

#define GET_EVENTS 1
#define EVENTS 2
#define GET_RESERVATIONS 3
#define RESERVATIONS 4
#define GET_TICKETS 5
#define TICKETS 6
#define BAD_REQUEST 255

void error(char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

void *safe_malloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        error("memory allocation failed.");
    }
    return ptr;
}

typedef struct Parameters {
    FILE *file_ptr;
    int port;
    int time_limit;
} Parameters;

typedef struct Event {
    char *description;
    uint8_t description_length;
    uint16_t tickets;
} Event;

typedef struct EventArray {
    Event *array;
    size_t reserved;
    size_t count;
    size_t description_length_sum;
} EventArray;

EventArray new_event_array() {
    size_t reserved = 1;
    size_t count = 0;
    Event *array = safe_malloc(reserved * sizeof(Event));
    return (EventArray) { .count = count, .reserved = reserved, .array = array , .description_length_sum = 0};
}

void add_to_event_array(EventArray *array, Event item) {
    array->count++;
    if (array->count == array->reserved) {
        array->reserved *= 2;
        array->array = realloc(array->array, array->reserved * sizeof(Event));
    }
    array->array[array->count - 1] = item;
    array->description_length_sum += item.description_length;
}

void print_event_array(EventArray *array) {
    for (size_t i = 0; i < array->count; i++) {
        printf("%zu: desc: '%s', tickets: %hhu\n", i, array->array[i].description, array->array[i].tickets);
    }
}

void destroy_event_array(EventArray *array) {
    for (size_t i = 0; i < array->count; i++) {
        free(array->array[i].description);
    }
}

int bind_socket(uint16_t port) {
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0); // creating IPv4 UDP socket
    ENSURE(socket_fd > 0);
    // after socket() call; we should close(sock) on any execution path;

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET; // IPv4
    server_address.sin_addr.s_addr = htonl(INADDR_ANY); // listening on all interfaces
    server_address.sin_port = htons(port);

    // bind the socket to a concrete address
    CHECK_ERRNO(bind(socket_fd, (struct sockaddr *) &server_address,
                     (socklen_t) sizeof(server_address)));

    return socket_fd;
}

size_t read_message(int socket_fd, struct sockaddr_in *client_address, char *buffer, size_t max_length) {
    socklen_t address_length = (socklen_t) sizeof(*client_address);
    int flags = 0; // we do not request anything special
    errno = 0;
    ssize_t len = recvfrom(socket_fd, buffer, max_length, flags,
                           (struct sockaddr *) client_address, &address_length);
    if (len < 0) {
        PRINT_ERRNO();
    }
    return (size_t) len;
}

void send_message(int socket_fd, const struct sockaddr_in *client_address, const char *message, size_t length) {
    socklen_t address_length = (socklen_t) sizeof(*client_address);
    int flags = 0;
    ssize_t sent_length = sendto(socket_fd, message, length, flags,
                                 (struct sockaddr *) client_address, address_length);
    ENSURE(sent_length == (ssize_t) length);
}


Parameters parse_args(int argc, char *argv[]) {
    if (argc > 7 || argc < 3 || !(argc & 1)) {
        error("improper usage.");
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
                error("file already set.");
            }
            file_set = true;
            file_ptr = fopen(argv[current_arg + 1], "r");
            if (!file_ptr) {
                error("opening of file failed.");
            }
        }
        else if (strcmp(argv[current_arg], "-p") == 0) {
            if (port_set) {
                error("port already set.");
            }
            port_set = true;
            char *ptr;
            port = (int) strtol(argv[current_arg + 1], &ptr, 10);
            if (*ptr != '\0' || port < 1024 || port > 49151) {
                error("illegal port.");
            }
        }
        else if (strcmp(argv[current_arg], "-t") == 0) {
            if (time_set) {
                error("time already set.");
            }
            time_set = true;
            char *ptr;
            time_limit = (int) strtol(argv[current_arg + 1], &ptr, 10);
            if (*ptr != '\0' || time_limit < 1 || time_limit > 86400) {
                error("illegal time.");
            }
        }
        else {
            error("unexpected expression.");
        }
    }

    if (!file_ptr) {
        error("file not set.");
    }

    return (Parameters) { .file_ptr = file_ptr, .port = port, .time_limit = time_limit };
}

#define BUFFER_SIZE 10
char shared_buffer[BUFFER_SIZE];

struct __attribute__((__packed__)) EventToSend {
    uint32_t event_id;
    uint16_t ticket_count;
    uint8_t description_length;
};

typedef struct EventToSend EventToSend;

void send_events(EventArray *array, int socket_fd, struct sockaddr_in client_address) {
    // TODO nie może być za duże
    size_t message_size = 1 + 7 * array->count + array->description_length_sum;
    char *message = safe_malloc(1 + 7 * array->count + array->description_length_sum);
    message[0] = EVENTS;
    size_t index = 1;
    EventToSend event_to_send;
    for (size_t i = 0; i < array->count; i++) {
        Event event = array->array[i];
        event_to_send.event_id = htonl(i);
        event_to_send.ticket_count = htons(event.tickets);
        event_to_send.description_length = event.description_length;
        memcpy(message + index, &event_to_send, 7);
        index += 7;
        memcpy(message + index, event.description, event.description_length);
        index += event.description_length;
    }
    printf("%s\n", message);
    send_message(socket_fd, &client_address, message, message_size);
}

int main(int argc, char *argv[]) {
    Parameters parameters = parse_args(argc, argv);

    char *buff = NULL;
    size_t buff_len;
    char *description;
    int description_length;
    uint16_t tickets;

    EventArray event_array = new_event_array();

    while ((description_length = getline(&buff, &buff_len, parameters.file_ptr)) >= 0) {
        description = safe_malloc((size_t) (description_length - 1) * sizeof(char));
        strcpy(description, buff);
        description[description_length - 1] = '\0';

        getline(&buff, &buff_len, parameters.file_ptr);
        tickets = (uint16_t) strtol(buff, NULL, 10);

        add_to_event_array(&event_array, (Event) { .description = description, .tickets = tickets,
                                                   .description_length = (uint8_t) description_length - 1});
    }

    print_event_array(&event_array);

    int socket_fd = bind_socket(parameters.port);

    struct sockaddr_in client_address;
    size_t read_length;
    do {
        read_length = read_message(socket_fd, &client_address, shared_buffer, sizeof(shared_buffer));
        char* client_ip = inet_ntoa(client_address.sin_addr);
        uint16_t client_port = ntohs(client_address.sin_port);
//        printf("received %zd bytes from client %s:%u: '%.*s'\n", read_length, client_ip, client_port, (int) read_length,
//               shared_buffer);
        printf("received %zd bytes from client %s:%u: '%d'\n", read_length, client_ip, client_port, shared_buffer[0]);
        if (shared_buffer[0] == GET_EVENTS) {
            send_events(&event_array, socket_fd, client_address);
        }
    } while (read_length > 0);

    destroy_event_array(&event_array);

    return 0;
}
