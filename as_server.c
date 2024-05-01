/*****************************************************************************/
/*                       CSC209-24s A4 Audio Stream                          */
/*       Copyright 2024 -- Demetres Kostas PhD (aka Darlene Heliokinde)      */
/*****************************************************************************/
#include "as_server.h"

int init_server_addr(int port, struct sockaddr_in *addr)
{
    // Allow sockets across machines.
    addr->sin_family = AF_INET;
    // The port the process will listen on.
    addr->sin_port = htons(port);
    // Clear this field; sin_zero is used for padding for the struct.
    memset(&(addr->sin_zero), 0, 8);

    // Listen on all network interfaces.
    addr->sin_addr.s_addr = INADDR_ANY;

    return 0;
}

int set_up_server_socket(const struct sockaddr_in *server_options, int num_queue)
{
    int soc = socket(AF_INET, SOCK_STREAM, 0);
    if (soc < 0)
    {
        perror("socket");
        exit(1);
    }

    printf("Listen socket created\n");

    // Make sure we can reuse the port immediately after the
    // server terminates. Avoids the "address in use" error
    int on = 1;
    int status = setsockopt(soc, SOL_SOCKET, SO_REUSEADDR,
                            (const char *)&on, sizeof(on));
    if (status < 0)
    {
        perror("setsockopt");
        exit(1);
    }

    // Associate the process with the address and a port
    if (bind(soc, (struct sockaddr *)server_options, sizeof(*server_options)) < 0)
    {
        // bind failed; could be because port is in use.
        perror("bind");
        exit(1);
    }

    printf("Socket bound to port %d\n", ntohs(server_options->sin_port));

    // Set up a queue in the kernel to hold pending connections.
    if (listen(soc, num_queue) < 0)
    {
        // listen failed
        perror("listen");
        exit(1);
    }

    printf("Socket listening for connections\n");

    return soc;
}

ClientSocket accept_connection(int listenfd)
{
    ClientSocket client;
    socklen_t addr_size = sizeof(client.addr);
    client.socket = accept(listenfd, (struct sockaddr *)&client.addr,
                           &addr_size);
    if (client.socket < 0)
    {
        perror("accept_connection: accept");
        exit(-1);
    }

    // print out a message that we got the connection
    printf("Server got a connection from %s, port %d\n",
           inet_ntoa(client.addr.sin_addr), ntohs(client.addr.sin_port));

    return client;
}

int list_request_response(const ClientSocket *client, const Library *library)
{
    if (client == NULL || library == NULL)
    {
        ERR_PRINT("list_request_response: invalid inputs");
        exit(-1);
    }

    // Calculate the response length
    int response_length = 0;
    for (int i = 0; i < library->num_files; i++)
    {
        // +3 for the ": " and \n, +4 for the index (allowing up to 9999 files)
        response_length += strlen(library->files[i]) + 7;
    }

    // Allocate memory for the response
    char *response = malloc(response_length + 1); // +1 for the null terminator
    if (response == NULL)
    {
        ERR_PRINT("list_request_response: malloc failed");
        return -1;
    }
    response[0] = '\0'; // Initialize the string

    // Append the files in REVERSE ORDER!!!
    for (int i = library->num_files - 1; i >= 0; i--)
    {
        char line[response_length + 1]; // +1 for null terminator
        int chars_written = snprintf(line, response_length + 1, "%d: %s\r\n", i, library->files[i]);
        if (chars_written < 0 || chars_written > response_length)
        {
            ERR_PRINT("list_request_response: snprintf failed");
            free(response);
            return -1;
        }
        strcat(response, line);
    }

    int bytes_sent = write(client->socket, response, strlen(response));
    if (bytes_sent < 0)
    {
        perror("list_request_response: write failed");
        free(response);
        return -1;
    }

    free(response);
    return library->num_files;
}

static int _load_file_size_into_buffer(FILE *file, uint8_t *buffer)
{
    if (fseek(file, 0, SEEK_END) < 0)
    {
        ERR_PRINT("Error seeking to end of file\n");
        return -1;
    }
    uint32_t file_size = ftell(file);
    if (fseek(file, 0, SEEK_SET) < 0)
    {
        ERR_PRINT("Error seeking to start of file\n");
        return -1;
    }
    buffer[0] = (file_size >> 24) & 0xFF;
    buffer[1] = (file_size >> 16) & 0xFF;
    buffer[2] = (file_size >> 8) & 0xFF;
    buffer[3] = file_size & 0xFF;
    return 0;
}

int stream_request_response(const ClientSocket *client, const Library *library,
                            uint8_t *post_req, int num_pr_bytes)
{
    uint32_t file_index;
    uint8_t file_index_bytes[4];

    // Copy all of the bytes already received
    memcpy(file_index_bytes, post_req, num_pr_bytes);

    // Read the remaining bytes of the file index if there are some bytes lacking
    int bytes_needed = 4 - num_pr_bytes;
    if (bytes_needed > 0)
    {
        ssize_t n = read(client->socket, file_index_bytes + num_pr_bytes, bytes_needed);
        if (n < bytes_needed)
        {
            ERR_PRINT("stream_request_response: Failed to read the complete file index from client");
            return -1;
        }
    }

    // Convert the file index back from network byte order to host byte order
    file_index = ntohl(*(uint32_t *)file_index_bytes);

    // Validate the file index.
    // The file index should be less then or equal to the total number files.
    if (file_index >= library->num_files)
    {
        ERR_PRINT("stream_request_response: Invalid file index requested");
        return -1;
    }

    // Open the requested file with "rb" flag to read out the binary information.
    char *file_path = _join_path(library->path, library->files[file_index]);
    FILE *file = fopen(file_path, "rb");
    if (!file)
    {
        ERR_PRINT("stream_request_response: Failed to open requested file");
        fclose(file);
        return -1;
    }

    // Since we know for sure that the file size is the first four bytes,
    // prepare the buffer manually and load it with the provided helper function.
    uint8_t file_size_buffer[4];
    if (_load_file_size_into_buffer(file, file_size_buffer) != 0)
    {
        fclose(file);
        return -1;
    }

    // Send the file size to the client.
    if (write(client->socket, file_size_buffer, sizeof(file_size_buffer)) != sizeof(file_size_buffer))
    {
        ERR_PRINT("stream_request_response: Failed to send file size to client");
        fclose(file);
        return -1;
    }

    // After sending the file size, the rest of the file should be sent to the client,
    // separated over multiple chunks with pre-defined chunk size.
    uint8_t data_chunk[STREAM_CHUNK_SIZE];
    size_t bytes_read;

    // This loop condition for the while loop seperates the data into multiplke chunks and...
    while ((bytes_read = fread(data_chunk, 1, sizeof(data_chunk), file)) > 0)
    {
        // ...this if statement condition sends it one by one!
        if (write(client->socket, data_chunk, bytes_read) != bytes_read)
        {
            ERR_PRINT("stream_request_response: Failed to send data chunk to client");
            fclose(file);
            return -1;
        }
    }

    free(file_path);
    fclose(file);
    return 0;
}

static Library make_library(const char *path)
{
    Library library;
    library.path = path;
    library.num_files = 0;
    library.files = NULL;
    library.name = "server";

    printf("Initializing library\n");
    printf("Library path: %s\n", library.path);

    return library;
}

static void _wait_for_children(pid_t **client_conn_pids, int *num_connected_clients, uint8_t immediate)
{
    int status;
    for (int i = 0; i < *num_connected_clients; i++)
    {
        int options = immediate ? WNOHANG : 0;
        if (waitpid((*client_conn_pids)[i], &status, options) > 0)
        {
            if (WIFEXITED(status))
            {
                printf("Client process %d terminated\n", (*client_conn_pids)[i]);
                if (WEXITSTATUS(status) != 0)
                {
                    fprintf(stderr, "Client process %d exited with status %d\n",
                            (*client_conn_pids)[i], WEXITSTATUS(status));
                }
            }
            else
            {
                fprintf(stderr, "Client process %d terminated abnormally\n",
                        (*client_conn_pids)[i]);
            }

            for (int j = i; j < *num_connected_clients - 1; j++)
            {
                (*client_conn_pids)[j] = (*client_conn_pids)[j + 1];
            }

            (*num_connected_clients)--;
            *client_conn_pids = (pid_t *)realloc(*client_conn_pids,
                                                 (*num_connected_clients) * sizeof(pid_t));
        }
    }
}

/*
** Create a server socket and listen for connections
**
** port: the port number to listen on.
**
** On success, returns the file descriptor of the socket.
** On failure, return -1.
*/
static int initialize_server_socket(int port)
{

    struct sockaddr_in server_addr;

    // Initialize the address with provided information.
    if (init_server_addr(port, &server_addr) < 0)
    {
        fprintf(stderr, "Initializing_server_socket: failed initializing server_addr\n");
        return -1;
    }

    // fromo the header file, it is specified that the number of maximum pending connection is MAX_PENDING, which is 10.
    int listenfd = set_up_server_socket(&server_addr, MAX_PENDING);
    if (listenfd < 0)
    {
        fprintf(stderr, "Initializing_server_socket: failed to set up the lisenting end of the socket fd\n");
        return -1;
    }

    return listenfd;
}

int run_server(int port, const char *library_directory)
{
    Library library = make_library(library_directory);
    if (scan_library(&library) < 0)
    {
        ERR_PRINT("Error scanning library\n");
        return -1;
    }

    int num_connected_clients = 0;
    pid_t *client_conn_pids = NULL;

    int incoming_connections = initialize_server_socket(port);
    if (incoming_connections == -1)
    {
        return -1;
    }

    int maxfd = incoming_connections;
    fd_set incoming;
    SET_SERVER_FD_SET(incoming, incoming_connections);
    int num_intervals_without_scan = 0;

    while (1)
    {
        if (num_intervals_without_scan >= LIBRARY_SCAN_INTERVAL)
        {
            if (scan_library(&library) < 0)
            {
                fprintf(stderr, "Error scanning library\n");
                return 1;
            }
            num_intervals_without_scan = 0;
        }

        struct timeval select_timeout = SELECT_TIMEOUT;
        if (select(maxfd + 1, &incoming, NULL, NULL, &select_timeout) < 0)
        {
            perror("run_server");
            exit(1);
        }

        if (FD_ISSET(incoming_connections, &incoming))
        {
            ClientSocket client_socket = accept_connection(incoming_connections);

            pid_t pid = fork();
            if (pid == -1)
            {
                perror("run_server");
                exit(-1);
            }
            // child process
            if (pid == 0)
            {
                close(incoming_connections);
                free(client_conn_pids);
                int result = handle_client(&client_socket, &library);
                _free_library(&library);
                close(client_socket.socket);
                return result;
            }
            close(client_socket.socket);
            num_connected_clients++;
            client_conn_pids = (pid_t *)realloc(client_conn_pids,
                                                (num_connected_clients) * sizeof(pid_t));
            client_conn_pids[num_connected_clients - 1] = pid;
        }
        if (FD_ISSET(STDIN_FILENO, &incoming))
        {
            if (getchar() == 'q')
                break;
        }

        num_intervals_without_scan++;
        SET_SERVER_FD_SET(incoming, incoming_connections);

        // Immediate return wait for client processes
        _wait_for_children(&client_conn_pids, &num_connected_clients, 1);
    }

    printf("Quitting server\n");
    close(incoming_connections);
    _wait_for_children(&client_conn_pids, &num_connected_clients, 0);
    _free_library(&library);
    return 0;
}

static uint8_t _is_file_extension_supported(const char *filename)
{
    static const char *supported_file_exts[] = SUPPORTED_FILE_EXTS;

    for (int i = 0; i < sizeof(supported_file_exts) / sizeof(char *); i++)
    {
        char *files_ext = strrchr(filename, '.');
        if (files_ext != NULL && strcmp(files_ext, supported_file_exts[i]) == 0)
        {
            return 1;
        }
    }

    return 0;
}

static int _depth_scan_library(Library *library, char *current_path)
{

    char *path_in_lib = _join_path(library->path, current_path);
    if (path_in_lib == NULL)
    {
        return -1;
    }

    DIR *dir = opendir(path_in_lib);
    if (dir == NULL)
    {
        perror("scan_library");
        return -1;
    }
    free(path_in_lib);

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if ((entry->d_type == DT_REG) &&
            _is_file_extension_supported(entry->d_name))
        {
            library->files = (char **)realloc(library->files,
                                              (library->num_files + 1) * sizeof(char *));
            if (library->files == NULL)
            {
                perror("_depth_scan_library");
                return -1;
            }

            library->files[library->num_files] = _join_path(current_path, entry->d_name);
            if (library->files[library->num_files] == NULL)
            {
                perror("scan_library");
                return -1;
            }
#ifdef DEBUG
            printf("Found file: %s\n", library->files[library->num_files]);
#endif
            library->num_files++;
        }
        else if (entry->d_type == DT_DIR)
        {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            {
                continue;
            }

            char *new_path = _join_path(current_path, entry->d_name);
            if (new_path == NULL)
            {
                return -1;
            }

#ifdef DEBUG
            printf("Library scan descending into directory: %s\n", new_path);
#endif

            int ret_code = _depth_scan_library(library, new_path);
            free(new_path);
            if (ret_code < 0)
            {
                return -1;
            }
        }
    }

    closedir(dir);
    return 0;
}

// This function is implemented recursively and uses realloc to grow the files array
// as it finds more files in the library. It ignores MAX_FILES.
int scan_library(Library *library)
{
// Maximal flexibility, free the old strings and start again
// A hash table leveraging inode number would be a better way to do this
#ifdef DEBUG
    printf("^^^^ ----------------------------------- ^^^^\n");
    printf("Freeing library\n");
#endif
    _free_library(library);

#ifdef DEBUG
    printf("Scanning library\n");
#endif
    int result = _depth_scan_library(library, "");
#ifdef DEBUG
    printf("vvvv ----------------------------------- vvvv\n");
#endif
    return result;
}

int handle_client(const ClientSocket *client, Library *library)
{
    char *request = NULL;
    uint8_t *request_buffer = (uint8_t *)malloc(REQUEST_BUFFER_SIZE);
    if (request_buffer == NULL)
    {
        perror("handle_client");
        return 1;
    }
    uint8_t *buff_end = request_buffer;

    int bytes_read = 0;
    int bytes_in_buf = 0;
    while ((bytes_read = read(client->socket, buff_end, REQUEST_BUFFER_SIZE - bytes_in_buf)) > 0)
    {
#ifdef DEBUG
        printf("Read %d bytes from client\n", bytes_read);
#endif

        bytes_in_buf += bytes_read;

        request = find_network_newline((char *)request_buffer, &bytes_in_buf);

        if (request && strcmp(request, REQUEST_LIST) == 0)
        {
            if (list_request_response(client, library) < 0)
            {
                ERR_PRINT("Error handling LIST request\n");
                goto client_error;
            }
        }
        else if (request && strcmp(request, REQUEST_STREAM) == 0)
        {
            int num_pr_bytes = MIN(sizeof(uint32_t), (unsigned long)bytes_in_buf);
            if (stream_request_response(client, library, request_buffer, num_pr_bytes) < 0)
            {
                ERR_PRINT("Error handling STREAM request\n");
                goto client_error;
            }
            bytes_in_buf -= num_pr_bytes;
            memmove(request_buffer, request_buffer + num_pr_bytes, bytes_in_buf);
        }
        else if (request)
        {
            ERR_PRINT("Unknown request: %s\n", request);
        }

        free(request);
        request = NULL;
        buff_end = request_buffer + bytes_in_buf;
    }
    if (bytes_read < 0)
    {
        perror("handle_client");
        goto client_error;
    }

    printf("Client on %s:%d disconnected\n",
           inet_ntoa(client->addr.sin_addr),
           ntohs(client->addr.sin_port));

    free(request_buffer);
    if (request != NULL)
    {
        free(request);
    }
    return 0;
client_error:
    free(request_buffer);
    if (request != NULL)
    {
        free(request);
    }
    return -1;
}

static void print_usage()
{
    printf("Usage: as_server [-h] [-p port] [-l library_directory]\n");
    printf("  -h  Print this message\n");
    printf("  -p  Port to listen on (default: " XSTR(DEFAULT_PORT) ")\n");
    printf("  -l  Directory containing the library (default: ./library/)\n");
}

int main(int argc, char *const *argv)
{
    int opt;
    int port = DEFAULT_PORT;
    const char *library_directory = "library";

    // Check out man 3 getopt for how to use this function
    // The short version: it parses command line options
    // Note that optarg is a global variable declared in getopt.h
    while ((opt = getopt(argc, argv, "hp:l:")) != -1)
    {
        switch (opt)
        {
        case 'h':
            print_usage();
            return 0;
        case 'p':
            port = atoi(optarg);
            break;
        case 'l':
            library_directory = optarg;
            break;
        default:
            print_usage();
            return 1;
        }
    }

    printf("Starting server on port %d, serving library in %s\n",
           port, library_directory);

    return run_server(port, library_directory);
}
