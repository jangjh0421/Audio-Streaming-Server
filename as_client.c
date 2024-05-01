/*****************************************************************************/
/*                       CSC209-24s A4 Audio Stream                          */
/*       Copyright 2024 -- Demetres Kostas PhD (aka Darlene Heliokinde)      */
/*****************************************************************************/
#include "as_client.h"

static int connect_to_server(int port, const char *hostname)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("connect_to_server");
        return -1;
    }

    struct sockaddr_in addr;

    // Allow sockets across machines.
    addr.sin_family = AF_INET;
    // The port the server will be listening on.
    // htons() converts the port number to network byte order.
    // This is the same as the byte order of the big-endian architecture.
    addr.sin_port = htons(port);
    // Clear this field; sin_zero is used for padding for the struct.
    memset(&(addr.sin_zero), 0, 8);

    // Lookup host IP address.
    struct hostent *hp = gethostbyname(hostname);
    if (hp == NULL)
    {
        ERR_PRINT("Unknown host: %s\n", hostname);
        return -1;
    }

    addr.sin_addr = *((struct in_addr *)hp->h_addr);

    // Request connection to server.
    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        perror("connect");
        return -1;
    }

    return sockfd;
}

/*
** Helper for: list_request
** This function reads from the socket until it finds a network newline.
** This is processed as a list response for a single library file,
** of the form:
**                   <index>:<filename>\r\n
**
** returns index on success, -1 on error
** filename is a heap allocated string pointing to the parsed filename
*/
static int get_next_filename(int sockfd, char **filename)
{
    static int bytes_in_buffer = 0;
    static char buf[RESPONSE_BUFFER_SIZE];

    while ((*filename = find_network_newline(buf, &bytes_in_buffer)) == NULL)
    {
        int num = read(sockfd, buf + bytes_in_buffer,
                       RESPONSE_BUFFER_SIZE - bytes_in_buffer);
        if (num < 0)
        {
            perror("list_request");
            return -1;
        }
        bytes_in_buffer += num;
        if (bytes_in_buffer == RESPONSE_BUFFER_SIZE)
        {
            ERR_PRINT("Response buffer filled without finding file\n");
            ERR_PRINT("Bleeding data, this shouldn't happen, but not giving up\n");
            memmove(buf, buf + BUFFER_BLEED_OFF, RESPONSE_BUFFER_SIZE - BUFFER_BLEED_OFF);
        }
    }

    char *parse_ptr = strtok(*filename, ":");
    int index = strtol(parse_ptr, NULL, 10);
    parse_ptr = strtok(NULL, ":");
    // moves the filename to the start of the string (overwriting the index)
    memmove(*filename, parse_ptr, strlen(parse_ptr) + 1);

    return index;
}

int list_request(int sockfd, Library *library)
{
    const char *list_req = "LIST\r\n";

    if (library == NULL)
    {
        ERR_PRINT("list_request: library pointer is NULL\n");
        return -1;
    }

    if (write(sockfd, list_req, strlen(list_req)) < 0)
    {
        perror("list_request: failed to write to sockfd");
        return -1;
    }

    _free_library(library);

    int num_files = 0;
    char *filename = NULL;

    // Temporary storage to accumulate filenames
    char **temp_filenames = NULL;

    while (1)
    {
        // Get the names of the files one-by-one:
        int index = get_next_filename(sockfd, &filename);
        if (index == -1)
        {
            break;
        }

        // We have to use realloc since we are reading the files one-by-one so we increment by one for each iteration.
        char **new_temp_filenames = realloc(temp_filenames, (num_files + 1) * sizeof(char *));
        if (new_temp_filenames == NULL)
        {
            perror("list_request: realloc failed");
            free(filename);
            while (num_files--)
                free(temp_filenames[num_files]);
            free(temp_filenames);
            return -1;
        }

        temp_filenames = new_temp_filenames;
        temp_filenames[num_files++] = filename;

        // To terminate the loop and void being blocked at the read(),
        // it should be very explicit when to terminate the loop.
        if (index == 0)
        {
            break;
        }
    }

    // Allocate the library's files array with the correct size
    library->files = malloc(num_files * sizeof(char *));
    if (library->files == NULL)
    {
        perror("list_request: malloc failed for library->files");
        while (num_files--)
            free(temp_filenames[num_files]);
        free(temp_filenames);
        return -1;
    }

    // Transfer the filenames to the library's files array in reverse order
    for (int i = 0; i < num_files; i++)
    {
        library->files[i] = temp_filenames[num_files - 1 - i];
    }

    // Print filenames in reverse order
    for (int i = num_files - 1; i >= 0; i--)
    {
        printf("%d: %s\n", num_files - 1 - i, library->files[num_files - 1 - i]);
    }

    // Free the temporary storage
    free(temp_filenames);

    library->num_files = num_files;
    return num_files;
}

/*
** Get the permission of the library directory. If the library
** directory does not exist, this function shall create it.
**
** library_dir: the path of the directory storing the audio files
** perpt:       an output parameter for storing the permission of the
**              library directory.
**
** returns 0 on success, -1 on error
*/
static int get_library_dir_permission(const char *library_dir, mode_t *perpt)
{

    // As hinted by the handout
    struct stat st;

    // Check if the directory exists, and if it exists...:
    if (stat(library_dir, &st) == 0)
    {
        *perpt = st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO); // After some research, I learned how to use this stat structure
        return 0;
    }

    // If the directory does not exist...:
    else
    {
        // Create one with permission 0700 (rwx for owner only)
        mode_t mode = S_IRWXU;
        if (mkdir(library_dir, mode) == 0)
        {
            *perpt = mode;
            return 0; // Directory created successfully
        }
        else
        {
            perror("Failed to create directory");
            return -1;
        }
    }
}

/*
** Creates any directories needed within the library dir so that the file can be
** written to the correct destination. All directories will inherit the permissions
** of the library_dir.
**
** This function is recursive, and will create all directories needed to reach the
** file in destination.
**
** Destination shall be a path without a leading /
**
** library_dir can be an absolute or relative path, and can optionally end with a '/'
**
*/
static void create_missing_directories(const char *destination, const char *library_dir)
{
    mode_t permissions;
    if (get_library_dir_permission(library_dir, &permissions) == -1) {
        exit(1);
    }

    char *str_de_tokville = strdup(destination);
    if (str_de_tokville == NULL)
    {
        perror("create_missing_directories");
        return;
    }

    char *before_filename = strrchr(str_de_tokville, '/');
    if (!before_filename)
    {
        goto free_tokville;
    }

    char *path = malloc(strlen(library_dir) + strlen(destination) + 2);
    if (path == NULL)
    {
        goto free_tokville;
    }
    *path = '\0';

    char *dir = strtok(str_de_tokville, "/");
    if (dir == NULL)
    {
        goto free_path;
    }
    strcpy(path, library_dir);
    if (path[strlen(path) - 1] != '/')
    {
        strcat(path, "/");
    }
    strcat(path, dir);

    while (dir != NULL && dir != before_filename + 1)
    {
#ifdef DEBUG
        printf("Creating directory %s\n", path);
#endif
        if (mkdir(path, permissions) == -1)
        {
            if (errno != EEXIST)
            {
                perror("create_missing_directories");
                goto free_path;
            }
        }
        dir = strtok(NULL, "/");
        if (dir != NULL)
        {
            strcat(path, "/");
            strcat(path, dir);
        }
    }
free_path:
    free(path);
free_tokville:
    free(str_de_tokville);
}

/*
** Helper for: get_file_request
*/
static int file_index_to_fd(uint32_t file_index, const Library *library)
{
    create_missing_directories(library->files[file_index], library->path);

    char *filepath = _join_path(library->path, library->files[file_index]);
    if (filepath == NULL)
    {
        return -1;
    }

    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
#ifdef DEBUG
    printf("Opened file %s\n", filepath);
#endif
    free(filepath);
    if (fd < 0)
    {
        perror("file_index_to_fd");
        return -1;
    }

    return fd;
}

int get_file_request(int sockfd, uint32_t file_index, const Library *library)
{
#ifdef DEBUG
    printf("Getting file %s\n", library->files[file_index]);
#endif

    int file_dest_fd = file_index_to_fd(file_index, library);
    if (file_dest_fd == -1)
    {
        return -1;
    }

    int result = send_and_process_stream_request(sockfd, file_index, -1, file_dest_fd);
    if (result == -1)
    {
        return -1;
    }

    return 0;
}

int start_audio_player_process(int *audio_out_fd)
{
    // Since we have to fork for execvp, we should set up a pipe.
    int pipefd[2];
    if (pipe(pipefd) == -1)
    {
        perror("start_audio_player_process: pipe");
        return -1;
    }

    // Child process required for execvp.
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("start_audio_player_process: fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    // For child process on which execvp will be called...:
    else if (pid == 0)
    {

        // Close the writing end
        close(pipefd[1]);

        if (dup2(pipefd[0], STDIN_FILENO) == -1)
        {
            perror("dup2");
            exit(EXIT_FAILURE);
        }

        // Since no longer needed
        close(pipefd[0]);

        char *args[] = AUDIO_PLAYER_ARGS;
        execvp(AUDIO_PLAYER, args);

        // execvp only returns when failed
        perror("execvp");
        exit(EXIT_FAILURE);
    }

    // For parents
    else
    {
        close(pipefd[0]);
        *audio_out_fd = pipefd[1]; // Return write end of the pipe to caller

        sleep(AUDIO_PLAYER_BOOT_DELAY);

        return pid;
    }
}

static void _wait_on_audio_player(int audio_player_pid)
{
    int status;
    if (waitpid(audio_player_pid, &status, 0) == -1)
    {
        perror("_wait_on_audio_player");
        return;
    }
    if (WIFEXITED(status))
    {
        fprintf(stderr, "Audio player exited with status %d\n", WEXITSTATUS(status));
    }
    else
    {
        printf("Audio player exited abnormally\n");
    }
}

int stream_request(int sockfd, uint32_t file_index)
{
    int audio_out_fd;
    int audio_player_pid = start_audio_player_process(&audio_out_fd);

    int result = send_and_process_stream_request(sockfd, file_index, audio_out_fd, -1);
    if (result == -1)
    {
        ERR_PRINT("stream_request: send_and_process_stream_request failed\n");
        return -1;
    }

    _wait_on_audio_player(audio_player_pid);

    return 0;
}

/*
** Helper for: send_and_process_stream_request
*/
void refreshDynamicBuffer(char **dynamicBuffer, size_t *dynamicBufferSize,
                          int offset, int *processedBytes,
                          int sockfd, int *fd, int other_fd)
{
    memmove(*dynamicBuffer, *dynamicBuffer + offset, *dynamicBufferSize - offset);
    *dynamicBuffer = realloc(*dynamicBuffer, *dynamicBufferSize - offset);
    if (dynamicBuffer == NULL){
        perror("send_and_process_stream_request: Realloc failed.\n");
        exit (-1);
    }
    *dynamicBufferSize -= offset;
    *processedBytes += offset;

    // Update max file descriptor for the next select() call
    *fd = sockfd > other_fd ? sockfd : other_fd;
}

/*
** Helper for: send_and_process_stream_request
*/
static int determine_max_fd(int sockfd, int audio_out_fd, int file_dest_fd)
{
    int max_fd = sockfd;

    if (audio_out_fd > max_fd)
    {
        max_fd = audio_out_fd;
    }
    if (file_dest_fd > max_fd)
    {
        max_fd = file_dest_fd;
    }

    return max_fd;
}

int stream_and_get_request(int sockfd, uint32_t file_index, const Library *library)
{
    int audio_out_fd;
    int audio_player_pid = start_audio_player_process(&audio_out_fd);

#ifdef DEBUG
    printf("Getting file %s\n", library->files[file_index]);
#endif

    int file_dest_fd = file_index_to_fd(file_index, library);
    if (file_dest_fd == -1)
    {
        ERR_PRINT("stream_and_get_request: file_index_to_fd failed\n");
        return -1;
    }

    int result = send_and_process_stream_request(sockfd, file_index,
                                                 audio_out_fd, file_dest_fd);
    if (result == -1)
    {
        ERR_PRINT("stream_and_get_request: send_and_process_stream_request failed\n");
        return -1;
    }

    _wait_on_audio_player(audio_player_pid);

    return 0;
}

int send_and_process_stream_request(int sockfd, uint32_t file_index,
                                    int audio_out_fd, int file_dest_fd)
{

    // None of the file descriptors are "on."
    if (audio_out_fd == -1 && file_dest_fd == -1)
    {
        ERR_PRINT("send_and_process_stream_request: None of the output file descriptors are activated.\n");
        return -1;
    }

    // Send STREAM request with the index of the requested file converted into networkbyte order
    const char *stream_req = "STREAM\r\n";
    uint32_t net_file_index = htonl(file_index);
    if (write(sockfd, stream_req, strlen(stream_req)) != strlen(stream_req))
    {
        perror("send_and_process_stream_request: Writing the request failed.\n");
        return -1;
    }
    if (write(sockfd, &net_file_index, sizeof(net_file_index)) != sizeof(net_file_index))
    {
        perror("send_and_process_stream_request: Writing the file index failed.\n");
        return -1;
    }

    // Set up the dynamic buffer as instructed in the handout.
    char *dynamic_buffer = malloc(sizeof(char));
    if (dynamic_buffer == NULL){
        perror("send_and_process_stream_request: Malloc failed.\n");
        return -1;
    }
    size_t dynamic_buffer_size = 0;
    char network_buffer[NETWORK_PRE_DYNAMIC_BUFF_SIZE];

    // Set up the timeout macro as instructed in the handout.
    struct timeval timeout = {SELECT_TIMEOUT_SEC, SELECT_TIMEOUT_USEC};

    // Assign the max_fd for select call
    int max_fd;
    max_fd = determine_max_fd(sockfd, audio_out_fd, file_dest_fd);

    // Read the size of the file from the first four bytes.
    // The value has to be converted back to the host byte order.
    uint32_t net_file_size;
    if (read(sockfd, &net_file_size, sizeof(net_file_size)) < sizeof(net_file_size))
    {
        perror("send_and_process_stream_request: Reading the file size failed.\n");
        return -1;
    }
    int file_size = ntohl(net_file_size);

    // initialize loop variables
    // Two offset are required for the use of dynamic buffer.
    int processed_bytes = 0;
    int audio_fd_offset = 0;
    int file_fd_offset = 0;

    while (processed_bytes < file_size)
    {
        fd_set read_fds, write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);

        FD_SET(sockfd, &read_fds);

        if (dynamic_buffer_size > 0)
        {
            // Once we have checked that there is something in the buffer, we have to identify to which file descriptor
            // we are writing to. It should be writing to both of them for stream+ operation.
            if (audio_out_fd != -1)
            {
                FD_SET(audio_out_fd, &write_fds);
            }
            if (file_dest_fd != -1)
            {
                FD_SET(file_dest_fd, &write_fds);
            }
        }

        int selected_fd = select(max_fd + 1, &read_fds, &write_fds, NULL, &timeout);

        // Select failed
        if (selected_fd == -1)
        {
            ERR_PRINT("send_and_process_stream_request: Select failed.\n");
            return -1;
        }

        // Select timeout
        // (Piazza) When select timeout happens, just let it have another loop iteration
        else if (selected_fd == 0)
        {
            continue;
        }
        // Select success
        else if (selected_fd > 0)
        {
            // Read from the server first if available.
            // Notice that sockfd is the only fd we can read from.
            if (FD_ISSET(sockfd, &read_fds))
            {
                // First, read as much as the buffer allows.
                // load it to the network_buffer first, and then update the dynamic buffer.
                int bytes_read = read(sockfd, network_buffer, NETWORK_PRE_DYNAMIC_BUFF_SIZE);
                if (bytes_read < 0)
                {
                    ERR_PRINT("send_and_process_stream_request: Reading from the server failed.\n");
                    return -1;
                }

                // Update the dynamic buffer to fit the data just read.
                int new_size = dynamic_buffer_size + bytes_read;
                dynamic_buffer = realloc(dynamic_buffer, new_size);
                memcpy(dynamic_buffer + dynamic_buffer_size, network_buffer, bytes_read);
                dynamic_buffer_size += bytes_read;
            }

            // Write to the audio_out_fd if it is available and specified as the output fd
            if (audio_out_fd != -1 && FD_ISSET(audio_out_fd, &write_fds))
            {
                int bytes_written = write(audio_out_fd, dynamic_buffer + audio_fd_offset, dynamic_buffer_size - audio_fd_offset);
                if (bytes_written < 0)
                {
                    ERR_PRINT("send_and_process_stream_request: Writing to the audio failed.\n");
                    return -1;
                }
                // If the chunk is successfully written, adjust the offset to indicate the data written without
                // affecting the buffer and its size.
                else
                {
                    audio_fd_offset += bytes_written;
                }

                if (file_dest_fd == -1)
                {
                    // Call the helper function to refresh and update the buffer
                    refreshDynamicBuffer(&dynamic_buffer, &dynamic_buffer_size,
                                         audio_fd_offset, &processed_bytes,
                                         sockfd, &max_fd, audio_out_fd);

                    // Now, this conditional statement implies that the audio_fd is open, therefore
                    // we have to get the offset set for the next step.
                    audio_fd_offset = 0;
                }
            }

            // Write to the file_dest_fd if it is available and specified as the output fd
            if (file_dest_fd != -1 && FD_ISSET(file_dest_fd, &write_fds))
            {
                int bytes_written = write(file_dest_fd, dynamic_buffer + file_fd_offset, dynamic_buffer_size - file_fd_offset);
                if (bytes_written < 0)
                {
                    ERR_PRINT("send_and_process_stream_request: Writing to the file failed.\n");
                    return -1;
                }
                // Update file offset to keep track of how much has been written without affecting the buffer
                file_fd_offset += bytes_written;

                // If only writing to file, remove the written data from the buffer
                if (audio_out_fd == -1)
                {
                    // Call the helper function to refresh and update the buffer
                    refreshDynamicBuffer(&dynamic_buffer, &dynamic_buffer_size,
                                         file_fd_offset, &processed_bytes,
                                         sockfd, &max_fd, file_dest_fd);

                    // Now, this conditional statement implies that the file_fd is open, therefore
                    // we have to get the offset set for the next step.
                    file_fd_offset = 0;
                }
            }

            // In case of writing to both the audio and the file, clean the buffer after writing is done.
            if (audio_fd_offset != -1 && file_fd_offset != -1)
            {
                max_fd = determine_max_fd(sockfd, audio_out_fd, file_dest_fd);

                // Notice that we have to memmove with the buffer_size - smaller_offset
                // to not accidentally ignore other written data.
                int smaller_offset;
                if (audio_fd_offset < file_fd_offset)
                {
                    smaller_offset = audio_fd_offset;
                }
                else
                {
                    smaller_offset = file_fd_offset;
                }

                // Update the offsets to get ready for the modified dynamic buffer.
                file_fd_offset -= smaller_offset;
                audio_fd_offset -= smaller_offset;

                // Update the dynamic buffer accordingly.
                int dynamic_reduced = dynamic_buffer_size - smaller_offset;
                char *new_start = dynamic_buffer + smaller_offset;

                memmove(dynamic_buffer, new_start, dynamic_reduced);

                dynamic_buffer = realloc(dynamic_buffer, dynamic_reduced);
                dynamic_buffer_size -= smaller_offset;

                processed_bytes += smaller_offset;
            }
        }
    }
    if (audio_out_fd != -1)
        close(audio_out_fd);
    if (file_dest_fd != -1)
        close(file_dest_fd);

    // DYNAMIC BUFFERS ARE DYNAMIC!!!
    free(dynamic_buffer);

    return 0;
}

static void _print_shell_help()
{
    printf("Commands:\n");
    printf("  list: List the files in the library\n");
    printf("  get <file_index>: Get a file from the library\n");
    printf("  stream <file_index>: Stream a file from the library (without saving it)\n");
    printf("  stream+ <file_index>: Stream a file from the library\n");
    printf("                        and save it to the local library\n");
    printf("  help: Display this help message\n");
    printf("  quit: Quit the client\n");
}

/*
** Shell to handle the client options
** ----------------------------------
** This function is a mini shell to handle the client options. It prompts the
** user for a command and then calls the appropriate function to handle the
** command. The user can enter the following commands:
** - "list" to list the files in the library
** - "get <file_index>" to get a file from the library
** - "stream <file_index>" to stream a file from the library (without saving it)
** - "stream+ <file_index>" to stream a file from the library and save it to the local library
** - "help" to display the help message
** - "quit" to quit the client
*/
static int client_shell(int sockfd, const char *library_directory)
{
    char buffer[REQUEST_BUFFER_SIZE];
    char *command;
    int file_index;

    Library library = {"client", library_directory, NULL, 0};

    while (1)
    {
        if (library.files == 0)
        {
            printf("Server library is empty or not retrieved yet\n");
        }

        printf("Enter a command: ");
        if (fgets(buffer, REQUEST_BUFFER_SIZE, stdin) == NULL)
        {
            perror("client_shell");
            goto error;
        }

        command = strtok(buffer, " \n");
        if (command == NULL)
        {
            continue;
        }

        // List Request -- list the files in the library
        if (strcmp(command, CMD_LIST) == 0)
        {
            if (list_request(sockfd, &library) == -1)
            {
                goto error;
            }

            // Get Request -- get a file from the library
        }
        else if (strcmp(command, CMD_GET) == 0)
        {
            char *file_index_str = strtok(NULL, " \n");
            if (file_index_str == NULL)
            {
                printf("Usage: get <file_index>\n");
                continue;
            }
            file_index = strtol(file_index_str, NULL, 10);
            if (file_index < 0 || file_index >= library.num_files)
            {
                printf("Invalid file index\n");
                continue;
            }

            if (get_file_request(sockfd, file_index, &library) == -1)
            {
                goto error;
            }

            // Stream Request -- stream a file from the library (without saving it)
        }
        else if (strcmp(command, CMD_STREAM) == 0)
        {
            char *file_index_str = strtok(NULL, " \n");
            if (file_index_str == NULL)
            {
                printf("Usage: stream <file_index>\n");
                continue;
            }
            file_index = strtol(file_index_str, NULL, 10);
            if (file_index < 0 || file_index >= library.num_files)
            {
                printf("Invalid file index\n");
                continue;
            }

            if (stream_request(sockfd, file_index) == -1)
            {
                goto error;
            }

            // Stream and Get Request -- stream a file from the library and save it to the local library
        }
        else if (strcmp(command, CMD_STREAM_AND_GET) == 0)
        {
            char *file_index_str = strtok(NULL, " \n");
            if (file_index_str == NULL)
            {
                printf("Usage: stream+ <file_index>\n");
                continue;
            }
            file_index = strtol(file_index_str, NULL, 10);
            if (file_index < 0 || file_index >= library.num_files)
            {
                printf("Invalid file index\n");
                continue;
            }

            if (stream_and_get_request(sockfd, file_index, &library) == -1)
            {
                goto error;
            }
        }
        else if (strcmp(command, CMD_HELP) == 0)
        {
            _print_shell_help();
        }
        else if (strcmp(command, CMD_QUIT) == 0)
        {
            printf("Quitting shell\n");
            break;
        }
        else
        {
            printf("Invalid command\n");
        }
    }

    _free_library(&library);
    return 0;
error:
    _free_library(&library);
    return -1;
}

static void print_usage()
{
    printf("Usage: as_client [-h] [-a NETWORK_ADDRESS] [-p PORT] [-l LIBRARY_DIRECTORY]\n");
    printf("  -h: Print this help message\n");
    printf("  -a NETWORK_ADDRESS: Connect to server at NETWORK_ADDRESS (default 'localhost')\n");
    printf("  -p  Port to listen on (default: " XSTR(DEFAULT_PORT) ")\n");
    printf("  -l LIBRARY_DIRECTORY: Use LIBRARY_DIRECTORY as the library directory (default 'as-library')\n");
}

int main(int argc, char *const *argv)
{
    int opt;
    int port = DEFAULT_PORT;
    const char *hostname = "localhost";
    const char *library_directory = "saved";

    while ((opt = getopt(argc, argv, "ha:p:l:")) != -1)
    {
        switch (opt)
        {
        case 'h':
            print_usage();
            return 0;
        case 'a':
            hostname = optarg;
            break;
        case 'p':
            port = strtol(optarg, NULL, 10);
            if (port < 0 || port > 65535)
            {
                ERR_PRINT("Invalid port number %d\n", port);
                return 1;
            }
            break;
        case 'l':
            library_directory = optarg;
            break;
        default:
            print_usage();
            return 1;
        }
    }

    printf("Connecting to server at %s:%d, using library in %s\n",
           hostname, port, library_directory);

    int sockfd = connect_to_server(port, hostname);
    if (sockfd == -1)
    {
        return -1;
    }

    int result = client_shell(sockfd, library_directory);
    if (result == -1)
    {
        close(sockfd);
        return -1;
    }

    close(sockfd);
    return 0;
}