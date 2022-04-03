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
#include <time.h>
#include "err.h"

#define GET_EVENTS 1
#define EVENTS 2
#define GET_RESERVATION 3
#define RESERVATION 4
#define GET_TICKETS 5
#define TICKETS 6
#define BAD_REQUEST 255

#define COOKIE_SIZE 48


uint64_t static inline htonll(uint64_t x) {
    return ((((uint64_t)htonl(x)) << 32) + htonl((x) >> 32));
}

uint64_t static inline ntohll(uint64_t x) {
    return htonll(x);
}

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
        printf("%zu: desc: '%s', tickets: %hu\n", i, array->array[i].description, array->array[i].tickets);
    }
}

void destroy_event_array(EventArray *array) {
    for (size_t i = 0; i < array->count; i++) {
        free(array->array[i].description);
    }
}

typedef struct DynamicArray {
    void **array;
    size_t reserved;
    size_t count;
} DynamicArray;


DynamicArray new_dynamic_array() {
    size_t reserved = 1;
    size_t count = 0;
    void **array = safe_malloc(reserved * sizeof(void *));
    return (DynamicArray) { .count = count, .reserved = reserved, .array = array };
}

void add_to_dynamic_array(DynamicArray *array, void *item) {
    array->count++;
    if (array->count == array->reserved) {
        array->reserved *= 2;
        array->array = realloc(array->array, array->reserved * sizeof(Event));
    }
    array->array[array->count - 1] = item;
}

void print_event_array_dyn(DynamicArray *array) {
    Event **events = (Event **) array->array;
    for (size_t i = 0; i < array->count; i++) {
        printf("%zu: desc: '%s', tickets: %hu\n", i, events[i]->description, events[i]->tickets);
    }
}

void destroy_dynamic_array(DynamicArray *array) {
    for (size_t i = 0; i < array->count; i++) {
        free(array->array[i]);
    }
}

typedef struct ReservationsContainer {
    DynamicArray array;
} ReservationsContainer;

typedef struct __attribute__((__packed__)) Reservation {
    uint32_t reservation_id;
    uint32_t event_id;
    uint16_t ticket_count;
    char cookie[COOKIE_SIZE];
    uint64_t expiration_time;
} Reservation;

typedef struct Server {
    Parameters parameters;
    int socket_fd;
    EventArray event_array;
    ReservationsContainer reservations;
} Server;

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

#define BUFFER_SIZE 1000

struct __attribute__((__packed__)) EventToSend {
    uint32_t event_id;
    uint16_t ticket_count;
    uint8_t description_length;
};

typedef struct EventToSend EventToSend;

void send_events(Server *server, struct sockaddr_in client_address) {
    // TODO nie może być za duże
    size_t message_size = 1 + 7 * server->event_array.count + server->event_array.description_length_sum;
    char message[message_size];
    message[0] = EVENTS;
    size_t index = 1;
    EventToSend event_to_send;
    for (size_t i = 0; i < server->event_array.count; i++) {
        Event event = server->event_array.array[i];
        event_to_send.event_id = htonl(i);
        event_to_send.ticket_count = htons(event.tickets);
        event_to_send.description_length = event.description_length;
        memcpy(message + index, &event_to_send, 7);
        index += 7;
        memcpy(message + index, event.description, event.description_length);
        index += event.description_length;
    }
    printf("%s\n", message);
    send_message(server->socket_fd, &client_address, message, message_size);
}

void send_bad_request(uint32_t id, int socket_fd, struct sockaddr_in client_address) {
    char message[5];
    message[0] = BAD_REQUEST;
    id = htonl(id);
    memcpy(message + 1, &id, 4);
    send_message(socket_fd, &client_address, message, 5);
}

char *get_new_cookie(uint32_t reservation_id) {
    char *cookie = safe_malloc(COOKIE_SIZE * sizeof(char));
    printf("reservation_id: %d\n", reservation_id);
    sprintf(cookie, "%d", reservation_id);
    size_t len = strlen(cookie);
    for (size_t i = len - 1; i < COOKIE_SIZE; i++) {
        cookie[i] = 33 + (rand() % 94);
    }
    return cookie;
}

uint32_t get_next_reservation_id(ReservationsContainer *reservations) {
    if (reservations->array.count == 0) {
        return 1000000;
    }
    else {
        return ((Reservation *) reservations->array.array[reservations->array.count - 1])->reservation_id + 1;
    }
}


Reservation *add_new_reservation(Server *server, uint32_t event_id, uint16_t ticket_count) {
    ReservationsContainer *reservations = &server->reservations;
    Reservation *reservation = safe_malloc(sizeof(Reservation));
    uint32_t id = get_next_reservation_id(reservations);
    char *cookie = get_new_cookie(id);

    *reservation = (Reservation) { .event_id = event_id, .ticket_count = ticket_count, .reservation_id = id,
                                   .expiration_time = time(NULL) + server->parameters.time_limit };
    memcpy(reservation->cookie, cookie, COOKIE_SIZE);
    free(cookie);

    server->event_array.array[event_id].tickets -= ticket_count;

    add_to_dynamic_array(&reservations->array, reservation);
    return reservation;
}

void process_reservation(const char *buffer, Server *server, struct sockaddr_in client_address) {
    uint32_t event_id;
    uint16_t ticket_count;

    memcpy(&event_id, buffer, 4);
    event_id = ntohl(event_id);

    memcpy(&ticket_count, buffer + 4, 2);
    ticket_count = ntohs(ticket_count);

    if (event_id >= server->event_array.count || ticket_count == 0) {
        send_bad_request(event_id, server->socket_fd, client_address);
        return;
    }

    if (server->event_array.array[event_id].tickets < ticket_count) {
        send_bad_request(event_id, server->socket_fd, client_address);
        return;
    }

    Reservation *reservation = add_new_reservation(server, event_id, ticket_count);
    Reservation reservation_net;
    memcpy(&reservation_net, reservation, sizeof(Reservation));
    reservation_net.reservation_id = htonl(reservation_net.reservation_id);
    reservation_net.event_id = htonl(reservation_net.event_id);
    reservation_net.ticket_count = htons(reservation_net.ticket_count);
    reservation_net.expiration_time = htonll(reservation_net.expiration_time);

    size_t message_length = 1 + sizeof(Reservation);
    printf("%lu\n", sizeof(Reservation));
    char message[message_length];
    message[0] = RESERVATION;
    memcpy(message + 1, &reservation_net, sizeof(Reservation));
//    printf("%s\n", message);
    send_message(server->socket_fd, &client_address, message, message_length);
}

int main(int argc, char *argv[]) {
    Parameters parameters = parse_args(argc, argv);

    char *buff = NULL;
    size_t buff_len;
    char *description;
    int description_length;
    uint16_t tickets;

    EventArray event_array = new_event_array();
    DynamicArray dynamic_event_array = new_dynamic_array();

    while ((description_length = getline(&buff, &buff_len, parameters.file_ptr)) >= 0) {
        description = safe_malloc((size_t) (description_length - 1) * sizeof(char));
        strcpy(description, buff);
        description[description_length - 1] = '\0';

        getline(&buff, &buff_len, parameters.file_ptr);
        tickets = (uint16_t) strtol(buff, NULL, 10);

        add_to_event_array(&event_array, (Event) { .description = description, .tickets = tickets,
                                                   .description_length = (uint8_t) description_length - 1});
        Event *event = malloc(sizeof(Event));
        *event = (Event) { .description = description, .description_length = description_length, .tickets = tickets };
        add_to_dynamic_array(&dynamic_event_array, event);
    }

    print_event_array(&event_array);
    print_event_array_dyn(&dynamic_event_array);

    int socket_fd = bind_socket(parameters.port);
    char shared_buffer[BUFFER_SIZE];

    Server server = (Server) { .parameters = parameters, .event_array = event_array, .socket_fd = socket_fd,
                               .reservations = (ReservationsContainer) { .array = new_dynamic_array() } };

    srand(2137); // TODO

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
            send_events(&server, client_address);
        }
        else if (shared_buffer[0] == GET_RESERVATION) {
            process_reservation(shared_buffer + 1, &server, client_address);
        }
    } while (read_length > 0);

    destroy_event_array(&event_array);
    destroy_dynamic_array(&dynamic_event_array);

    return 0;
}
