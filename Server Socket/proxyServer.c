#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "threadpool.h"

#define BUFFER_SIZE 8192

int pool_size, port, max_number_requests, tasksSent = 0;
int lineCount = 0;
char filterList[10][100];

typedef struct response_info_st
{
    char *absPath; // Absolute path of the requested resource
    char *host;
} response_info_t;

void handle_get_request(const char *path, const char *host, int client_socket)
{
    // Open a socket to connect to hsot server
    int host_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (host_socket < 0)
    {
        printf("error: socket\n");
        return;
    }

    char request[1024];
    snprintf(request, 1024, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);

    // Resolve host ip address
    struct hostent *server = gethostbyname(host);
    if (server == NULL)
    {
        perror("Error resolving host");
        close(host_socket);
        return;
    }

    struct sockaddr_in server_addr;
    bzero((char *)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr_list[0], (char *)&server_addr.sin_addr.s_addr, server->h_length);
    server_addr.sin_port = htons(80);
    // printf("got to ip address\n");

    // Connect to the host server
    if (connect(host_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        printf("error: connect\n");
        close(host_socket);
        return;
    }
    // printf("got to connect\n");

    // Send HTTP request
    if (send(host_socket, request, strlen(request), 0) < 0)
    {
        printf("error: send\n");
        close(host_socket);
        return;
    }
    // printf("sent the request\n");

    // Receive and forward the response to the client
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;
    while ((bytes_received = recv(host_socket, buffer, BUFFER_SIZE, 0)) > 0)
    {
        if (write(client_socket, buffer, bytes_received) < 0)
        {
            printf("error: write\n");
            close(host_socket);
            return;
        }
    }

    // printf("finished the handle\n");

    close(host_socket);
}

int readRequest(char *request, int sockfd)
{
    // read with loop:
    int nBytes;
    int bytesRead = 0;
    char buffer[1024];

    while (bytesRead < sizeof(buffer) + 1)
    {

        if ((nBytes = read(sockfd, buffer + bytesRead, sizeof(buffer) - bytesRead - 1)) < 0)
        {
            return 500;
        }
        else if (nBytes == 0)
        {
            return 999;
        }

        bytesRead += nBytes;
        buffer[bytesRead] = '\0';

        if (strstr(buffer, "\r\n\r\n") != NULL)
        {
            break; // Exit the loop if newline character is found
        }
    }
    strncpy(request, buffer, bytesRead);
    request[bytesRead] = '\0'; // Null terminate the request

    return 0;
}

int parseRequest(char *request, char *path, response_info_t *info)
{
    char method[4];
    char protocol[64];
    char hostHeader[100];
    memset(method, 0, sizeof(method));
    memset(protocol, 0, sizeof(protocol));
    memset(hostHeader, 0, sizeof(hostHeader));

    int assigned = sscanf(request, "%4s %s %8s", method, path, protocol);
    if (assigned != 3)
    {
        return 400;
    }
    if (strcmp(method, "GET"))
    {
        return 501;
    }
    if (strcmp(protocol, "HTTP/1.0") && strcmp(protocol, "HTTP/1.1"))
    {
        return 400;
    }

    // Extract Host header from request
    char *hostStart = strstr(request, "Host:");
    if (hostStart == NULL)
    {
        return 400; // Bad Request
    }
    hostStart += 6; // Move past "Host: "
    char *hostEnd = strchr(hostStart, '\r');
    if (hostEnd == NULL)
    {
        return 400; // Bad Request
    }
    strncpy(hostHeader, hostStart, hostEnd - hostStart);
    hostHeader[hostEnd - hostStart] = '\0'; // Null-terminate the host header

    // Prepend "www." to the hostname
    char modifiedHost[120];
    if (strncmp(hostHeader, "www.", 4) != 0)
    {
        snprintf(modifiedHost, sizeof(modifiedHost), "www.%s", hostHeader);
    }
    else
    {
        strcat(modifiedHost, hostHeader);
    }

    // // Try to get the IP address from the Host header
    struct hostent *hostAddr;

    if ((hostAddr = gethostbyname(hostHeader)) == NULL)
    {
        return 404; // not found
    }

    struct in_addr *ip_addr = (struct in_addr *)hostAddr->h_addr_list[0];
    char *ip_str = inet_ntoa(*ip_addr);

    if (ip_str == NULL)
    {
        return 404;
    }

    for (int i = 0; i < lineCount; i++)
    {
        if (strncmp(modifiedHost, filterList[i], strlen(filterList[i])) == 0)
        {
            // printf("compared and found!\n");
            return 403;
        }
        if (isdigit(filterList[i][0]))
        {
            char n1[10], n2[10], newStr[20];
            if (sscanf(filterList[i], "%[^.].%[^.]", n1, n2) == 2)
            {
                sprintf(newStr, "%s.%s", n1, n2);
                if (memcmp(ip_str, newStr, strlen(newStr)) == 0)
                {
                    // printf("compared and found!\n");
                    return 403;
                }
            }
        }
    }

    info->absPath = strdup(path);
    info->host = strdup(hostHeader);

    return 200;
}

int writeResponse(int sockfd, char *response, char *path, response_info_t *info)
{
    int response_length = strlen(response);
    int bytes_written = 0;
    int nBytes;

    while (bytes_written < response_length)
    {
        if ((nBytes = write(sockfd, response, response_length)) < 0)
        {
            return -1;
        }
        bytes_written += nBytes;
    }

    return 0;
}

char *getResponseBody(int type)
{
    char title[128], body[128];
    memset(title, 0, sizeof(title));
    memset(body, 0, sizeof(body));

    switch (type)
    {
    case 302:
        strcat(title, "302 Found");
        strcat(body, "Directories must end with a slash.");
        break;

    case 400:
        strcat(title, "400 Bad Request");
        strcat(body, "Bad Request.");
        break;

    case 403:
        strcat(title, "403 Forbidden");
        strcat(body, "Access denied.");
        break;

    case 404:
        strcat(title, "404 Not Found");
        strcat(body, "File not found.");
        break;

    case 500:
        strcat(title, "500 Internal Server Error");
        strcat(body, "Some server side error.");
        break;

    case 501:
        strcat(title, "501 Not Supported");
        strcat(body, "Method is not supported.");
        break;
    }

    int length = strlen("<HTML><HEAD><TITLE>%s</TITLE></HEAD>\n<BODY><H4>%s</H4>\n%s\n</BODY></HTML>\n") + 2 * strlen(title) + strlen(body);
    char *responseBody = (char *)calloc(length + 1, sizeof(char));
    if (!responseBody)
    {
        return NULL;
    }
    sprintf(responseBody, "<HTML><HEAD><TITLE>%s</TITLE></HEAD>\n<BODY><H4>%s</H4>\n%s\n</BODY></HTML>\n", title, title, body);

    return responseBody;
}

char *get_file_type(char *name)
{
    char *ext = strrchr(name, '.');

    if (!ext)
        return NULL;
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
        return "text/html";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(ext, ".gif"))
        return "image/gif";
    if (strcmp(ext, ".pgn"))
        return "image/png";
    if (strcmp(ext, ".css"))
        return "text/css";
    if (strcmp(ext, ".au"))
        return "audio/basic";
    if (strcmp(ext, ".wav"))
        return "audio/wav";
    if (strcmp(ext, ".avi"))
        return "video/x-msvideo";
    if (strcmp(ext, ".mpeg"))
        return "video/mpeg";
    if (strcmp(ext, ".mp3"))
        return "audio/mpeg";

    return NULL;
}

char *constructResponse(int type, char *path, response_info_t *info)
{
    char header[64] = "Server: webserver/1.0\r\n";
    char connection[64] = "Connection: close\r\n\r\n";
    int path_length = path ? strlen(path) : 0;
    char location_header[64 + path_length];
    char type_string[64];
    char response_type[200];
    char dateStr[64 + 128];
    char timebuff[128];
    char last_modified[64 + 128];
    char content_length[64];
    char content_type[64];

    memset(location_header, 0, sizeof(location_header));
    memset(type_string, 0, sizeof(type_string));
    memset(dateStr, 0, sizeof(dateStr));
    memset(timebuff, 0, sizeof(timebuff));
    memset(response_type, 0, sizeof(response_type));
    memset(last_modified, 0, sizeof(last_modified));
    memset(content_length, 0, sizeof(content_length));
    memset(content_type, 0, sizeof(content_type));

    switch (type)
    {
    case 200:
        strcat(type_string, "200 OK");
        break;

    case 302:
        strcat(type_string, "302 Found");
        break;

    case 400:
        strcat(type_string, "400 Bad Request");
        break;

    case 403:
        strcat(type_string, "403 Forbidden");
        break;

    case 404:
        strcat(type_string, "404 Not Found");
        break;

    case 500:
        strcat(type_string, "500 Internal Server Error");
        break;

    case 501:
        strcat(type_string, "501 Not Supported");
        break;
    }

    sprintf(response_type, "HTTP/1.1 %s\r\n", type_string);
    time_t currTime;
    currTime = time(NULL);
    strftime(timebuff, sizeof(timebuff), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&currTime));
    sprintf(dateStr, "Date: %s\r\n", timebuff);

    char *mime = !(type == 200) ? get_file_type("index.html") : get_file_type(strrchr(path, '/'));
    if (mime)
        sprintf(content_type, "Content-Type: %s\r\n", mime);

    char *responeBody = NULL;
    if (type == 200)
    {
    }
    else
    {
        responeBody = getResponseBody(type);
        if (!responeBody)
        {
            return NULL;
        }
        sprintf(content_length, "Content-Length: %d\r\n", (int)strlen(responeBody));
    }

    int responseBody_length = !responeBody ? 0 : (int)strlen(responeBody);
    int length = strlen(response_type) + strlen(header) + strlen(dateStr) + strlen(location_header) + strlen(content_length) + strlen(content_type) + strlen(last_modified) + strlen(connection) + responseBody_length;

    char *response = (char *)calloc(length + 1, sizeof(char));
    if (!response)
    {
        free(responeBody);
        return NULL;
    }

    sprintf(response, "%s%s%s%s%s%s%s%s%s", response_type, header, dateStr, location_header, content_type, content_length, last_modified, connection, responeBody ? responeBody : "");

    if (responeBody)
    {
        free(responeBody);
    }

    // printf("%s\n", response);

    return response;
}

int task_function(void *arg)
{
    if (!arg)
    {
        return -1;
    }
    int *temp = (int *)(arg);
    int sockfd = *temp;
    free(arg);

    response_info_t *info = (response_info_t *)calloc(1, sizeof(response_info_t));
    if (!info)
    {
        char *response = constructResponse(500, NULL, NULL);
        if (!response)
        {
            return -1;
        }

        if (writeResponse(sockfd, response, NULL, NULL))
        {
            free(response);
            return -1;
        }
        free(response);
        close(sockfd);
        return -1;
    }

    info->absPath = NULL;
    info->host = NULL;

    int toReturnCode;
    char request[4000];
    char path[4000];
    memset(request, 0, sizeof(request));
    memset(path, 0, sizeof(path));

    toReturnCode = readRequest(request, sockfd);

    if (toReturnCode == 999)
    {
        printf("error: reading request\n");
        if (info->absPath)
        {
            free(info->absPath);
        }
        if (info->host)
        {
            free(info->host);
        }

        free(info);
        close(sockfd);
        return -1;
    }
    else
    {
        if (toReturnCode == 500)
        {
            char *response = constructResponse(toReturnCode, path, info);
            if (!response)
            {
                printf("error: construct response\n");
                return -1;
            }
            if (writeResponse(sockfd, response, path, info))
            {
                free(response);
                return -1;
            }
            free(response);
        }
        else
        {
            toReturnCode = parseRequest(request, path, info);
            if (toReturnCode == 200)
            {
                handle_get_request(path, info->host, sockfd);
            }
            else
            {
                // printf("i continued to here after\n");
                char *response = constructResponse(toReturnCode, path, info);
                if (!response)
                {
                    printf("error: construct response\n");
                    return -1;
                }
                if (writeResponse(sockfd, response, path, info))
                {
                    free(response);
                    return -1;
                }
                free(response);
            }
        }
    }

    if (info->absPath)
    {
        free(info->absPath);
    }
    if (info->host)
    {
        free(info->host);
    }

    if (info)
    {
        free(info);
    }
    close(sockfd);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 5)
    {
        printf("Usage: proxyServer <port> <pool-size> <max-number-of-request> <filter>\n");
        return -1;
    }

    port = atoi(argv[1]);
    pool_size = atoi(argv[2]);
    max_number_requests = atoi(argv[3]);

    FILE *filepointer;
    lineCount = 0;
    filepointer = fopen(argv[4], "r");
    if (filepointer == NULL)
    {
        printf("error: fopen\n");
        return -1;
    }

    while (fgets(filterList[lineCount], 100, filepointer) != NULL && lineCount < 10)
    {
        lineCount++;
    }

    // Close the file
    fclose(filepointer);

    // printf("%d\n%d\n%d\n%s\n", port, pool_size, max_number_requests, filter);

    if (pool_size <= 0 || port <= 0 || port > 65535 || max_number_requests <= 0)
    {
        printf("Usage: proxyServer <port> <pool-size> <max-number-of-request> <filter>\n");
        return -1;
    }

    threadpool *pool = create_threadpool(pool_size);
    if (pool == NULL)
    {
        perror("error: create_threadpool\n");
        return -1;
    }

    int server_socket = 0;
    if ((server_socket = socket(PF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("error: socket\n");
        exit(-1);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("error: bind\n");
        exit(1);
    }

    if (listen(server_socket, max_number_requests) < 0)
    {
        perror("error: listen\n");
        exit(1);
    }

    int *new_socket;
    for (int i = 0; i < max_number_requests; i++)
    {
        new_socket = (int *)calloc(1, sizeof(int));

        if (!new_socket)
        {
            perror("error: calloc\n");
            continue;
        }

        if ((*new_socket = accept(server_socket, NULL, NULL)) < 0)
        {
            perror("error: accept\n");
            continue;
        }

        dispatch(pool, task_function, (void *)new_socket);
    }

    close(server_socket);
    destroy_threadpool(pool);

    return 0;
}