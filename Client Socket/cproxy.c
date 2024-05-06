#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/stat.h>
#include <errno.h>

int mkdir_rec(const char *path, mode_t mode)
{
    struct stat st;
    if (stat(path, &st) == 0)
    {
        if (S_ISDIR(st.st_mode))
        {
            return 0;
        }
        else
        {
            perror("path's not a directory");
            return -1;
        }
    }
    else
    {
        if (mkdir(path, mode) == 0)
        {
            return 0;
        }
        else
        {
            if (errno == ENOENT)
            {
                char *parent = strdup(path);
                if (!parent)
                {
                    perror("strdup");
                    return -1;
                }
                char *sep = strrchr(parent, '/');
                if (sep)
                {
                    *sep = '\0';
                    if (mkdir_rec(parent, mode) != 0)
                    {
                        free(parent);
                        return -1;
                    }
                }
                free(parent);
                return mkdir(path, mode);
            }
            else
            {
                perror("mkdir");
                return -1;
            }
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2 || argv == NULL)
    {
        printf("Usage: cproxy <URL> [-s]\n");
        exit(-1);
    }

    char *url;
    url = argv[1];

    if (url == NULL || strstr(url, "http://") == NULL)
    {
        printf("Usage: cproxy <URL> [-s]\n");
        exit(-1);
    }

    url[strcspn(url, "\n")] = '\0';

    char *host, *path, *port, *nport;
    host = (char *)malloc(100 * sizeof(char));
    path = (char *)malloc(100 * sizeof(char));
    port = (char *)malloc(10 * sizeof(char));
    nport = (char *)malloc(10 * sizeof(char));

    if (host == NULL || path == NULL || port == NULL || nport == NULL)
    {
        perror("failed to allocate memory");
        exit(EXIT_FAILURE);
    }
    //-------------------------------------parsing the url-----------------------------------
    sscanf(url, "http://%99[^/]/%99s", host, path);
    int portNum = 0;

    if (strchr(host, ':') != NULL) // geting the port if found
    {
        nport = strchr(host, ':');
        strcpy(port, nport + 1);
        memmove(nport, nport + strlen(nport), strlen(nport + strlen(nport)) + 1);

        portNum = atoi(port);
    }

    // printf("host: %s\nport: %s\npath: %s\n", host, port, path);
    //---------------------------------------------------------------------------------------

    char file_path[50] = "";
    strcat(file_path, host);
    strcat(file_path, "/");
    strcat(file_path, path);
    char response[1024];
    ssize_t size;

    struct stat buffer;
    if (stat(file_path, &buffer) == 0)
    {

        size = buffer.st_size;
        sprintf(response, "HTTP/1.0 200 OK\r\nContent-Length: %ld\r\n\r\n ", size);
        printf("File is given from local filesystem\n");
    }
    else
    {
        char request[200];
        snprintf(request, sizeof(request),
                 "GET /%s HTTP/1.0\r\nHost: %s\r\n\r\n",
                 path, host);

        printf("HTTP request =\n%s\nLEN = %ld\n", request, strlen(request));

        if (portNum == 0)
        {
            portNum = 80;
        }
        else if (portNum < 0 || portNum > (65536))
        {
            printf("Usage: cproxy <URL> [-s]\n");
            exit(-1);
        }

        //-------------------Connect to Server--------------------------------
        int socketfd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket < 0)
        {
            perror("socket");
            exit(EXIT_FAILURE);
        }

        struct sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(portNum);
        struct hostent *hp;
        hp = gethostbyname(host);
        memcpy(&serverAddr.sin_addr.s_addr, hp->h_addr, hp->h_length);

        if (connect(socketfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
        {
            perror("connect");
            exit(EXIT_FAILURE);
        }
        //--------------------------------------------------------------------

        //----------------------send http request-----------------------------
        int nBytes;
        if ((nBytes = write(socketfd, request, strlen(request))) < 0)
        {
            perror("write");
            exit(EXIT_FAILURE);
        }
        //--------------------------------------------------------------------

        //----------------------receive & save response-----------------------
        const char *last_slash = strrchr(file_path, '/');
        if (!last_slash)
        {
            printf("Usage: cproxy <URL> [-s]\n");
            exit(EXIT_FAILURE);
        }
        char *dir_path = malloc(last_slash - file_path + 1);
        if (!dir_path)
        {
            perror("malloc");
            exit(EXIT_FAILURE);
        }

        strncpy(dir_path, file_path, last_slash - file_path);
        dir_path[last_slash - file_path] = '\0';

        if (mkdir_rec(dir_path, 0700) == -1)
        {
            perror("mkdir");
            exit(EXIT_FAILURE);
        }

        FILE *file = fopen(file_path, "w");
        if (file == NULL)
        {
            perror("fopen");
            exit(EXIT_FAILURE);
        }

        ssize_t bytes_recieved;
        while ((bytes_recieved = read(socketfd, response, 1023)) > 0)
        {
            fwrite(response, sizeof(char), bytes_recieved, file);
        }

        if (bytes_recieved < 0)
        {
            perror("receive");
            fclose(file);
            exit(EXIT_FAILURE);
        }

        struct stat sizer;
        if (stat(file_path, &sizer) == 0)
        {

            size = sizer.st_size;
        }
        //--------------------------------------------------------------------
        printf("HTTP/1.0 200 OK\r\nContent-Length: %ld\r\n\r\n ", size);
        fclose(file);
        close(socketfd);
        free(dir_path);
    }

    printf("%s\n", response);
    printf("\n Total response bytes: %ld\n", size + strlen(response));
    if (argv[2] != NULL)
    {
        if (strstr(argv[2], "-s") != NULL)
        {

            char openCmd[200];
            snprintf(openCmd, sizeof(openCmd), "xdg-open %s", file_path);
            system(openCmd);
        }
    }

    free(host);
    free(path);
    free(port);
    free(nport);

    return 0;
}