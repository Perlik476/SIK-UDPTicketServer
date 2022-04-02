//#include <cstdio>
//#include <cstdlib>
//#include <cstring>
//
//void fatal(char *message) {
//    printf("%s\n", message);
//    exit(1);
//}
//
//struct Parameters {
//    FILE *file_ptr;
//    int port;
//    int time_limit;
//};
//
//Parameters parse_args(int argc, char *argv[]) {
//    if (argc > 7 || argc < 3 || !(argc & 1)) {
//        exit(1);
//    }
//
//    FILE *file_ptr = NULL;
//    int port = 2022;
//    int time_limit = 5;
//
//    bool file_set = false;
//    bool port_set = false;
//    bool time_set = false;
//
//    for (int current_arg = 1; current_arg < argc; current_arg += 2) {
//        if (strcmp(argv[current_arg], "-f") == 0) {
//            if (file_set) {
//                fatal("file already set.");
//            }
//            file_set = true;
//            file_ptr = fopen(argv[current_arg + 1], "r");
//            if (!file_ptr) {
//                exit(1);
//            }
//        }
//        else if (strcmp(argv[current_arg], "-p") == 0) {
//            if (port_set) {
//                exit(1);
//            }
//            port_set = true;
//            char *ptr;
//            port = (int) strtol(argv[current_arg + 1], &ptr, 10);
//            if (*ptr != '\0' || port < 1024 || port > 49151) {
//                exit(1);
//            }
//        }
//        else if (strcmp(argv[current_arg], "-t") == 0) {
//            if (time_set) {
//                exit(1);
//            }
//            time_set = true;
//            char *ptr;
//            time_limit = (int) strtol(argv[current_arg + 1], &ptr, 10);
//            if (*ptr != '\0' || time_limit < 1 || time_limit > 86400) {
//                exit(1);
//            }
//        }
//        else {
//            exit(1);
//        }
//    }
//
//    if (!file_ptr) {
//        exit(1);
//    }
//
//    return Parameters { .file_ptr = file_ptr, .port = port, .time_limit = time_limit };
//}
//
//int main(int argc, char *argv[]) {
//    Parameters parameters = parse_args(argc, argv);
//
//    return 0;
//}
