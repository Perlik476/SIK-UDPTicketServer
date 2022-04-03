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
#define NO_TICKETS (-1)

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
    size_t outdated_count;
    size_t first_not_outdated;
    uint32_t next_id;
} ReservationsContainer;

typedef struct __attribute__((__packed__)) Reservation {
    uint32_t reservation_id;
    uint32_t event_id;
    uint16_t ticket_count;
    char cookie[COOKIE_SIZE];
    uint64_t expiration_time;
    int64_t first_ticket_id;
} Reservation;

typedef struct Server {
    Parameters parameters;
    int socket_fd;
    EventArray event_array;
    ReservationsContainer reservations;
    int64_t next_ticket_id;
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

Reservation *add_new_reservation(Server *server, uint32_t event_id, uint16_t ticket_count) {
    ReservationsContainer *reservations = &server->reservations;
    Reservation *reservation = safe_malloc(sizeof(Reservation));
    uint32_t id = reservations->next_id++;
    char *cookie = get_new_cookie(id);

    *reservation = (Reservation) { .event_id = event_id, .ticket_count = ticket_count, .reservation_id = id,
                                   .expiration_time = time(NULL) + server->parameters.time_limit,
                                   .first_ticket_id = NO_TICKETS };
    memcpy(reservation->cookie, cookie, COOKIE_SIZE);
    free(cookie);

    server->event_array.array[event_id].tickets -= ticket_count;

    add_to_dynamic_array(&reservations->array, reservation);
    return reservation;
}

typedef struct __attribute__((__packed__)) ReservationToSend {
    uint32_t reservation_id;
    uint32_t event_id;
    uint16_t ticket_count;
    char cookie[COOKIE_SIZE];
    uint64_t expiration_time;
} ReservationToSend;

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

    ReservationToSend reservation_net;
    memcpy(&reservation_net, reservation, sizeof(ReservationToSend));
    printf("tickets: %d\n", reservation_net.ticket_count);
    reservation_net.reservation_id = htonl(reservation_net.reservation_id);
    reservation_net.event_id = htonl(reservation_net.event_id);
    reservation_net.ticket_count = htons(reservation_net.ticket_count);
    reservation_net.expiration_time = htonll(reservation_net.expiration_time);

    size_t message_length = 1 + sizeof(ReservationToSend);
    printf("%lu\n", sizeof(ReservationToSend));

    char message[message_length];
    message[0] = RESERVATION;
    memcpy(message + 1, &reservation_net, sizeof(ReservationToSend));

    send_message(server->socket_fd, &client_address, message, message_length);
}

void remove_outdated_reservations(ReservationsContainer *reservations, uint64_t current_time) {
    DynamicArray *array = &reservations->array;
    size_t current_index = 0;
    for (size_t i = 0; i < array->count; i++) {
        Reservation *reservation = (Reservation *)array->array[i];
        if (reservation->first_ticket_id != NO_TICKETS || reservation->expiration_time > current_time) {
            array->array[current_index++] = reservation;
        }
    }
    reservations->array.count = current_index;
    reservations->outdated_count = 0;
    reservations->first_not_outdated = 0;
    while (reservations->array.reserved / 2 > current_index) {
        reservations->array.reserved /= 2;
    }
    if (current_index == 0) {
        reservations->array.array = safe_malloc(sizeof(void *));
    }
    else {
        reservations->array.array = realloc(array->array, reservations->array.reserved * sizeof(void *));
        // TODO safe_realloc
    }
}


void check_outdated_reservations(Server *server) {
    uint64_t current_time = time(NULL);

    ReservationsContainer *reservations = &server->reservations;
    DynamicArray *array = &reservations->array;
    for (size_t i = reservations->first_not_outdated; i < array->count; i++) {
        Reservation *reservation = ((Reservation *)array->array[i]);
        if (reservation->expiration_time > current_time) {
            break;
        }
        reservations->first_not_outdated++;
        if (reservation->first_ticket_id == NO_TICKETS) {
            reservations->outdated_count++;
            server->event_array.array[reservation->event_id].tickets += reservation->ticket_count;
        }
    }

    if (reservations->outdated_count >= array->count / 2) {
        remove_outdated_reservations(reservations, current_time);
    }
}

Reservation *find_reservation(ReservationsContainer *reservations, uint32_t reservation_id, char *cookie) {
    DynamicArray *array = &reservations->array;
     for (size_t i = 0; i < array->count; i++) {
         printf("i: %zu, count: %zu\n", i, array->count);
         Reservation *reservation = (Reservation *) array->array[i];
         printf("res_id: %d vs %d\n", reservation->reservation_id, reservation_id);
         printf("cookie: %.*s vs %.*s\n", COOKIE_SIZE, reservation->cookie, COOKIE_SIZE, cookie);
         if (reservation->reservation_id == reservation_id && memcmp(reservation->cookie, cookie, COOKIE_SIZE) == 0) {
             printf("ok\n");
             return reservation;
         }
     }
     return NULL;
}

void ticket_id_to_str(char *str, int64_t id) {
    printf("id: %ld\n", id);
    for (size_t i = 0; i < 7; i++) {
        char temp = (char) (id % 36);
        printf("  temp: %d\n", temp);
        str[i] = temp < 10 ? (48 + temp) : (55 + temp);
        id /= 36;
    }
}

void process_tickets(const char *buffer, Server *server, struct sockaddr_in client_address) {
    uint32_t reservation_id;
    char cookie[COOKIE_SIZE];

    memcpy(&reservation_id, buffer, 4);
    reservation_id = ntohl(reservation_id);

    memcpy(&cookie, buffer + 4, COOKIE_SIZE);

    printf("reservation_id: %d\n", reservation_id);

    Reservation *reservation = find_reservation(&server->reservations, reservation_id, cookie);
    if (reservation == NULL || (reservation->first_ticket_id == NO_TICKETS && reservation->expiration_time < time(NULL))) {
        send_bad_request(reservation_id, server->socket_fd, client_address);
        return;
    }
    uint16_t ticket_count = reservation->ticket_count;

    printf("ticket_count: %d\n", ticket_count);

    if (reservation->first_ticket_id == NO_TICKETS) {
        reservation->first_ticket_id = server->next_ticket_id;
        server->next_ticket_id += ticket_count;
    }

    size_t message_length = 7 + 7 * ticket_count;
    char message[message_length];
    message[0] = TICKETS;

//    memset(message + 7, '0', 7 * ticket_count);
    for (size_t i = 0; i < ticket_count; i++) {
        ticket_id_to_str(message + 7 * (i + 1), reservation->first_ticket_id + (int64_t) i);
    }

    reservation_id = htonl(reservation_id);
    ticket_count = htons(ticket_count);

    memcpy(message + 1, &reservation_id, 4);
    memcpy(message + 5, &ticket_count, 2);

    printf("message_length: %zu\n", message_length);
    for (size_t i = 0; i < message_length; i++) {
        printf("%d, ", message[i]);
    }
    printf("\n");

    send_message(server->socket_fd, &client_address, message, message_length);

    send_events(server, client_address);
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

    fclose(parameters.file_ptr);

    print_event_array(&event_array);
    print_event_array_dyn(&dynamic_event_array);

    int socket_fd = bind_socket(parameters.port);
    char shared_buffer[BUFFER_SIZE];

    Server server = (Server) { .parameters = parameters, .event_array = event_array, .socket_fd = socket_fd,
                               .reservations = (ReservationsContainer)
                                       { .array = new_dynamic_array(), .first_not_outdated = 0,
                                         .outdated_count = 0, .next_id = 1000000 } };

    srand(2137); // TODO

    struct sockaddr_in client_address;
    size_t read_length;
    do {
        read_length = read_message(socket_fd, &client_address, shared_buffer, sizeof(shared_buffer));
        char* client_ip = inet_ntoa(client_address.sin_addr);
        uint16_t client_port = ntohs(client_address.sin_port);
        printf("received %zd bytes from client %s:%u: '%d', time: %ld\n", read_length, client_ip, client_port,
               shared_buffer[0], time(NULL));
        check_outdated_reservations(&server);
        if (shared_buffer[0] == GET_EVENTS && read_length == 1) {
            send_events(&server, client_address);
        }
        else if (shared_buffer[0] == GET_RESERVATION && read_length == 7) {
            process_reservation(shared_buffer + 1, &server, client_address);
        }
        else if (shared_buffer[0] == GET_TICKETS && read_length == 53) {
            process_tickets(shared_buffer + 1, &server, client_address);
        }
    } while (read_length > 0); // TODO

    destroy_event_array(&event_array);
    destroy_dynamic_array(&dynamic_event_array);

    return 0;
}
