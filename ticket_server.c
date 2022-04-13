#define  _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>

#define GET_EVENTS 1
#define EVENTS 2
#define GET_RESERVATION 3
#define RESERVATION 4
#define GET_TICKETS 5
#define TICKETS 6
#define BAD_REQUEST 255

#define COOKIE_SIZE 48
#define NO_TICKETS (-1)
#define MAX_MESSAGE_LENGTH 65507

// Evaluate `x`: if false, print an error message and exit with an error.
#define ENSURE(x)                                                         \
    do {                                                                  \
        bool result = (x);                                                \
        if (!result) {                                                    \
            fprintf(stderr, "Error: %s was false in %s at %s:%d\n",       \
                #x, __func__, __FILE__, __LINE__);                        \
            exit(EXIT_FAILURE);                                           \
        }                                                                 \
    } while (0)

// Check if errno is non-zero, and if so, print an error message and exit with an error.
#define PRINT_ERRNO()                                                  \
    do {                                                               \
        if (errno != 0) {                                              \
            fprintf(stderr, "Error: errno %d in %s at %s:%d\n%s\n",    \
              errno, __func__, __FILE__, __LINE__, strerror(errno));   \
            exit(EXIT_FAILURE);                                        \
        }                                                              \
    } while (0)


// Set `errno` to 0 and evaluate `x`. If `errno` changed, describe it and exit.
#define CHECK_ERRNO(x)                                                             \
    do {                                                                           \
        errno = 0;                                                                 \
        (void) (x);                                                                \
        PRINT_ERRNO();                                                             \
    } while (0)

// Note: the while loop above wraps the statements so that the macro can be used with a semicolon
// for example: if (a) CHECK(x); else CHECK(y);

static inline uint64_t htonll(uint64_t x) {
    return ((((uint64_t)htonl(x)) << 32) + htonl((x) >> 32));
}

static inline uint64_t ntohll(uint64_t x) {
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

void *safe_realloc(void *ptr, size_t size) {
    ptr = realloc(ptr, size);
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
        array->array = safe_realloc(array->array, array->reserved * sizeof(Event));
    }
    array->array[array->count - 1] = item;
}

void print_event_array_dyn(DynamicArray *array) { // TODO
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
    DynamicArray event_array;
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
    FILE *file_ptr = NULL;
    int port = 2022;
    int time_limit = 5;

    bool file_set = false;

    int opt;

    while ((opt = getopt(argc, argv, "f:p:t:")) != -1) {
        char *ptr;
        switch (opt) {
            case 'f':
                if (file_set) {
                    fclose(file_ptr);
                }
                file_set = true;
                file_ptr = fopen(optarg, "r");
                if (!file_ptr) {
                    error("opening of file failed.");
                }
                break;
            case 'p':
                port = (int) strtol(optarg, &ptr, 10);
                if (*ptr != '\0' || port < 1024 || port > 65535) { // TODO
                    error("illegal port.");
                }
                break;
            case 't':
                time_limit = (int) strtol(optarg, &ptr, 10);
                if (*ptr != '\0' || time_limit < 1 || time_limit > 86400) {
                    error("illegal time.");
                }
                break;
            default:
                error("improper usage");
        }
    }

    if (optind < argc) { // TODO
        error("improper usage.");
    }
    if (strcmp(argv[argc - 1], "--") == 0) { // TODO
        error("improper usage.");
    }
    if (!file_ptr) {
        error("file not set.");
    }

    return (Parameters) { .file_ptr = file_ptr, .port = port, .time_limit = time_limit };
}

static inline size_t event_message_size(Event *event) {
    return 7 + event->description_length;
}

DynamicArray read_file(Parameters *parameters) {
    char *buff = NULL;
    size_t buff_len;
    char *description;
    long long description_length;
    long long digits_count;

    uint16_t tickets;
    size_t events_message_size = 1;

    DynamicArray event_array = new_dynamic_array();

    while ((description_length = getline(&buff, &buff_len, parameters->file_ptr)) >= 0) {
        description = safe_malloc((size_t) (description_length - 1) * sizeof(char));
        memcpy(description, buff, description_length - 1);

        digits_count = getline(&buff, &buff_len, parameters->file_ptr);
        if (digits_count <= 0) {
            exit(0);
        }

        tickets = (uint16_t) strtol(buff, NULL, 10);

        Event *event = safe_malloc(sizeof(Event));
        *event = (Event) { .description = description, .description_length = description_length - 1, .tickets = tickets };
        if (events_message_size + event_message_size(event) > MAX_MESSAGE_LENGTH) {
            free(event);
            break;
        }
        events_message_size += event_message_size(event);
        add_to_dynamic_array(&event_array, event);
    }

    fclose(parameters->file_ptr);
    free(buff);

    return event_array;
}

Server initialize_server(int argc, char *argv[]) {
    Parameters parameters = parse_args(argc, argv);
    DynamicArray event_array = read_file(&parameters);

    srand(2137); // TODO
    int socket_fd = bind_socket(parameters.port);

    ReservationsContainer reservations = (ReservationsContainer) { .array = new_dynamic_array(),
            .first_not_outdated = 0, .outdated_count = 0, .next_id = 1000000 };
    Server server = (Server) { .parameters = parameters, .event_array = event_array, .socket_fd = socket_fd,
            .reservations =  reservations };

    return server;
}

#define BUFFER_SIZE 66000

typedef struct __attribute__((__packed__)) EventToSend {
    uint32_t event_id;
    uint16_t ticket_count;
    uint8_t description_length;
} EventToSend;

void send_events(Server *server, struct sockaddr_in client_address) {
    char message[MAX_MESSAGE_LENGTH];
    message[0] = EVENTS;
    size_t index = 1;
    EventToSend event_to_send;

    for (size_t i = 0; i < server->event_array.count; i++) {
        Event *event = server->event_array.array[i];
        event_to_send.event_id = htonl(i);
        event_to_send.ticket_count = htons(event->tickets);
        event_to_send.description_length = event->description_length;
        memcpy(message + index, &event_to_send, 7);

        index += 7;
        memcpy(message + index, event->description, event->description_length);
        index += event->description_length;
    }

    printf("%s\n", message);
    send_message(server->socket_fd, &client_address, message, index);
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

    ((Event *) server->event_array.array[event_id])->tickets -= ticket_count;

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

    if ((ticket_count + 1) * 7 > MAX_MESSAGE_LENGTH) {
        send_bad_request(event_id, server->socket_fd, client_address);
        return;
    }

    if (event_id >= server->event_array.count || ticket_count == 0) {
        send_bad_request(event_id, server->socket_fd, client_address);
        return;
    }

    if (((Event *) server->event_array.array[event_id])->tickets < ticket_count) {
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
            if (current_index != i) {
                free(array->array[current_index]);
                array->array[current_index] = reservation;
                array->array[i] = NULL;
            }
            current_index++;
        }
    }

    for (size_t i = current_index; i < array->count; i++) {
        if (array->array[i] != NULL) {
            free(array->array[i]);
        }
    }

    reservations->array.count = current_index;
    reservations->outdated_count = 0;
    reservations->first_not_outdated = 0;
    while (reservations->array.reserved / 4 > current_index) {
        reservations->array.reserved /= 4;
    }

    if (current_index == 0 && reservations->array.reserved != 1) {
        free(reservations->array.array);
        reservations->array.array = safe_malloc(sizeof(void *));
    }
    else {
        reservations->array.array = safe_realloc(reservations->array.array, reservations->array.reserved * sizeof(void *));
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
            ((Event *) server->event_array.array[reservation->event_id])->tickets += reservation->ticket_count;
        }
    }

    if (reservations->outdated_count >= array->count / 2) {
        remove_outdated_reservations(reservations, current_time);
    }
}

Reservation *find_reservation(ReservationsContainer *reservations, uint32_t reservation_id, char *cookie) {
    DynamicArray *array = &reservations->array;
    if (array->count == 0) {
        return NULL;
    }
    long long begin = 0;
    long long end = (int) array->count - 1;
    long long mid = (begin + end) / 2;
    Reservation *reservation;
    do {
        reservation = array->array[mid];
//        printf("1. begin: %d, end: %d, mid: %d, cur: %d, wanted: %d\n", begin, end, mid,
//               reservation->reservation_id, reservation_id);
        if (reservation->reservation_id == reservation_id) {
            break;
        }
        if (reservation->reservation_id < reservation_id) {
            begin = mid + 1;
        }
        else if (reservation->reservation_id > reservation_id) {
            end = mid - 1;
        }
        mid = (begin + end) / 2;
//        printf("2. begin: %d, end: %d, mid: %d, cur: %d, wanted: %d\n", begin, end, mid,
//               reservation->reservation_id, reservation_id);
    } while (end > 0 && begin < (long long) (array->count - 1));

    reservation = array->array[mid];
    if (reservation->reservation_id == reservation_id && memcmp(reservation->cookie, cookie, COOKIE_SIZE) == 0) {
        return reservation;
    }
    return NULL;
}

void ticket_id_to_str(char *str, int64_t id) {
    for (size_t i = 0; i < 7; i++) {
        char temp = (char) (id % 36);
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

    Reservation *reservation = find_reservation(&server->reservations, reservation_id, cookie);
    if (reservation == NULL || (reservation->first_ticket_id == NO_TICKETS
        && reservation->expiration_time < (uint64_t) time(NULL))) {
        send_bad_request(reservation_id, server->socket_fd, client_address);
        return;
    }
    uint16_t ticket_count = reservation->ticket_count;

    if (reservation->first_ticket_id == NO_TICKETS) {
        reservation->first_ticket_id = server->next_ticket_id;
        server->next_ticket_id += ticket_count;
    }

    size_t message_length = 7 + 7 * ticket_count;
    char message[message_length];
    message[0] = TICKETS;

    for (size_t i = 0; i < ticket_count; i++) {
        ticket_id_to_str(message + 7 * (i + 1), reservation->first_ticket_id + (int64_t) i);
    }

    reservation_id = htonl(reservation_id);
    ticket_count = htons(ticket_count);

    memcpy(message + 1, &reservation_id, 4);
    memcpy(message + 5, &ticket_count, 2);

    send_message(server->socket_fd, &client_address, message, message_length);
}

void destroy_server(Server *server) {
    for (size_t i = 0; i < server->event_array.count; i++) {
        Event *event = server->event_array.array[i];
        free(event->description);
    }
    destroy_dynamic_array(&server->event_array);
    destroy_dynamic_array(&server->reservations.array);
    free(server->event_array.array);
    free(server->reservations.array.array);
}

void process_incoming_messages(Server *server) {
    char shared_buffer[BUFFER_SIZE];
    struct sockaddr_in client_address;
    size_t read_length;

    do {
        read_length = read_message(server->socket_fd, &client_address, shared_buffer, sizeof(shared_buffer));
        char *client_ip = inet_ntoa(client_address.sin_addr);
        uint16_t client_port = ntohs(client_address.sin_port);
        printf("received %zd bytes from client %s:%u: '%d', time: %ld\n", read_length, client_ip, client_port,
               shared_buffer[0], time(NULL));
        check_outdated_reservations(server);
        if (shared_buffer[0] == GET_EVENTS && read_length == 1) {
            send_events(server, client_address);
        }
        else if (shared_buffer[0] == GET_RESERVATION && read_length == 7) {
            process_reservation(shared_buffer + 1, server, client_address);
        }
        else if (shared_buffer[0] == GET_TICKETS && read_length == 53) {
            process_tickets(shared_buffer + 1, server, client_address);
        }
    } while (read_length > 0); // TODO
}

int main(int argc, char *argv[]) {
    Server server = initialize_server(argc, argv);

    process_incoming_messages(&server);
    destroy_server(&server);

    return 0;
}
