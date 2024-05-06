#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include <sys/select.h>
#include <fcntl.h>

#define MAX_CLIENTS 10
#define MAX_MSG_SIZE 1024

struct conn
{
    int sd;
    char buffer[MAX_MSG_SIZE];
    int buffLen;
    struct conn *next;
};

volatile sig_atomic_t flag = 0;

void sigint_handler(int sig)
{
    flag = 1;
}

void remove_connection(struct conn **head, int sd)
{
    struct conn *curr = *head;
    struct conn *prev = NULL;

    while (curr != NULL && curr->sd != sd)
    {
        prev = curr;
        curr = curr->next;
    }

    if (curr != NULL)
    {
        if (prev == NULL)
        {
            *head = curr->next;
        }
        else
        {
            prev->next = curr->next;
        }

        close(curr->sd);
        printf("Removing connection with sd %d\n", sd);
        free(curr);
    }
}

void handle_client(struct conn *curr, fd_set *read_fds, struct conn **head, int *maxfd)
{
    if (FD_ISSET(curr->sd, read_fds)) // if socket ready for reading...
    {
        printf("Descriptor %d is readable\n", curr->sd);

        ssize_t bytes_read = read(curr->sd, curr->buffer + curr->buffLen, MAX_MSG_SIZE - curr->buffLen);
        if (bytes_read < 0)
        {
            perror("error: read\n");
            // remove connection?
            return;
        }
        else if (bytes_read == 0)
        {
            printf("Connection closed for sd %d\n", curr->sd);
            // remove connection?
            remove_connection(head, curr->sd);
            if (curr->sd == *maxfd)
            {
                int i = curr->sd - 1;
                *maxfd = i;
            }
            return;
        }

        curr->buffLen += bytes_read;
        printf("%zd bytes received from sd %d\n", bytes_read, curr->sd);

        // process message and forward:....
        char *newline = strchr(curr->buffer, '\n');
        while (newline != NULL)
        {
            *newline = '\0';

            for (char *ptr = curr->buffer; *ptr != '\0'; ++ptr)
            {
                *ptr = toupper(*ptr);
            }

            // forward to other clients....
            struct conn *temp = *head;
            while (temp != NULL)
            {
                if (temp != curr)
                {
                    if (write(temp->sd, curr->buffer, strlen(curr->buffer) + 1) == -1)
                    {
                        printf("sd:%d\n", temp->sd);
                        perror("error: write\n");
                        // maybe an exit?
                        return;
                    }
                    if (write(temp->sd, "\n", 2) == -1)
                    {
                        perror("error: write\n");
                        // maybe an exit?
                        return;
                    }
                }
                temp = temp->next;
            }

            memmove(curr->buffer, newline + 1, curr->buffLen - (newline - curr->buffer) - 1);
            curr->buffLen -= (newline - curr->buffer) + 1;

            newline = strchr(curr->buffer, '\n');
        }
    }
}

void new_connection(int listener, fd_set *read_fds, struct conn **head, int *maxfd)
{
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int new_sd = accept(listener, (struct sockaddr *)&client_addr, &client_len);
    if (new_sd == -1)
    {
        perror("error: accept\n");
        return;
    }

    // int on = 1;
    if (fcntl(new_sd, F_SETFL, O_NONBLOCK) == -1)
    {
        perror("error: fcntl\n");
        close(new_sd);
        return;
    }

    struct conn *new_conn = (struct conn *)malloc(sizeof(struct conn));
    if (!new_conn)
    {
        perror("error: malloc\n");
        close(new_sd);
        return;
    }
    new_conn->sd = new_sd;
    new_conn->buffLen = 0;
    new_conn->next = *head;
    *head = new_conn;

    FD_SET(new_sd, read_fds);
    if (new_sd > *maxfd)
    {
        *maxfd = new_sd;
    }

    printf("New incoming connection on sd %d\n", new_sd);
}

void handle_connection(int listener)
{
    signal(SIGINT, sigint_handler);

    fd_set read_fds;
    struct conn *head = NULL;
    int maxfd = listener;

    while (!flag)
    {
        FD_ZERO(&read_fds);
        FD_SET(listener, &read_fds);

        struct conn *current = head;
        while (current != NULL)
        {
            FD_SET(current->sd, &read_fds);
            current = current->next;
        }
        printf("Waiting on select()...\nMaxFd %d\n", maxfd);

        if (select(maxfd + 1, &read_fds, NULL, NULL, NULL) < 0)
        {
            printf("error: select\n");
            break;
        }

        if (FD_ISSET(listener, &read_fds))
        {
            new_connection(listener, &read_fds, &head, &maxfd);
        }

        current = head;
        while (current != NULL)
        {
            handle_client(current, &read_fds, &head, &maxfd);
            current = current->next;
        }
    }

    // clean up and exit...
    while (head != NULL)
    {
        remove_connection(&head, head->sd);
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("Usage: server <port>\n");
        return -1;
    }

    int port = atoi(argv[1]);
    if (port < 1 || port > 65535)
    {
        printf("Usage: server <port>\n");
        return -1;
    }

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1)
    {
        perror("error: socket\n");
        return -1;
    }

    int opt = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    {
        perror("error: setsockopt\n");
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    if (bind(listener, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        perror("error: bind\n");
        return -1;
    }

    if (listen(listener, MAX_CLIENTS) == -1)
    {
        perror("error: listen\n");
        return -1;
    }

    fd_set read_fds;
    int maxfd = listener;
    struct conn *head = NULL;

    handle_connection(maxfd);
    close(listener);

    return 0;
}