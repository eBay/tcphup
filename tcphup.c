/* SPDX-License-Identifier: MIT */
/*
 * tcphup.c	"tcphup.c", close tcp connections and manipulate TCP options
 *
 * Authors:     John Jawed, <jawed@php.net>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <linux/inet_diag.h>
#include <dirent.h>
#include <errno.h>

#define thup_exit(msg) perror(msg); exit(EXIT_FAILURE);
#define MAX_TCP_OPTIONS 32

/* Option data types */
typedef enum {
    OPT_TYPE_INT,
    OPT_TYPE_TIMEVAL,
    OPT_TYPE_LINGER
} opt_type_t;

/* Structure to hold socket option settings */
typedef struct {
    int level;       /* SOL_TCP or SOL_SOCKET */
    int optname;
    opt_type_t type;
    union {
        int int_val;
        struct timeval tv_val;
        struct linger linger_val;
    } value;
} tcp_option_t;

/* Mapping of option names to their constants */
typedef struct {
    const char *name;
    int level;
    int optname;
    opt_type_t type;
} tcp_option_map_t;

static const tcp_option_map_t tcp_option_mapping[] = {
    /* TCP-level options (SOL_TCP) */
    {"TCP_KEEPIDLE", SOL_TCP, TCP_KEEPIDLE, OPT_TYPE_INT},
    {"TCP_KEEPINTVL", SOL_TCP, TCP_KEEPINTVL, OPT_TYPE_INT},
    {"TCP_KEEPCNT", SOL_TCP, TCP_KEEPCNT, OPT_TYPE_INT},
    {"TCP_SYNCNT", SOL_TCP, TCP_SYNCNT, OPT_TYPE_INT},
    {"TCP_LINGER2", SOL_TCP, TCP_LINGER2, OPT_TYPE_INT},
    {"TCP_DEFER_ACCEPT", SOL_TCP, TCP_DEFER_ACCEPT, OPT_TYPE_INT},
    {"TCP_WINDOW_CLAMP", SOL_TCP, TCP_WINDOW_CLAMP, OPT_TYPE_INT},
    {"TCP_USER_TIMEOUT", SOL_TCP, TCP_USER_TIMEOUT, OPT_TYPE_INT},
    {"TCP_NODELAY", SOL_TCP, TCP_NODELAY, OPT_TYPE_INT},
    {"TCP_MAXSEG", SOL_TCP, TCP_MAXSEG, OPT_TYPE_INT},
    {"TCP_CORK", SOL_TCP, TCP_CORK, OPT_TYPE_INT},
    {"TCP_QUICKACK", SOL_TCP, TCP_QUICKACK, OPT_TYPE_INT},
    /* Socket-level options (SOL_SOCKET) */
    {"SO_KEEPALIVE", SOL_SOCKET, SO_KEEPALIVE, OPT_TYPE_INT},
    {"SO_SNDBUF", SOL_SOCKET, SO_SNDBUF, OPT_TYPE_INT},
    {"SO_RCVBUF", SOL_SOCKET, SO_RCVBUF, OPT_TYPE_INT},
    {"SO_REUSEADDR", SOL_SOCKET, SO_REUSEADDR, OPT_TYPE_INT},
    {"SO_REUSEPORT", SOL_SOCKET, SO_REUSEPORT, OPT_TYPE_INT},
    {"SO_SNDTIMEO", SOL_SOCKET, SO_SNDTIMEO, OPT_TYPE_TIMEVAL},
    {"SO_RCVTIMEO", SOL_SOCKET, SO_RCVTIMEO, OPT_TYPE_TIMEVAL},
    {"SO_LINGER", SOL_SOCKET, SO_LINGER, OPT_TYPE_LINGER},
    {NULL, 0, 0, OPT_TYPE_INT}
};

/* Find option mapping by name */
static const tcp_option_map_t* find_option_mapping(const char *name) {
    for (int i = 0; tcp_option_mapping[i].name != NULL; i++) {
        if (strcmp(name, tcp_option_mapping[i].name) == 0) {
            return &tcp_option_mapping[i];
        }
    }
    return NULL;
}

/* Parse a single -m argument in format "OPTION:value" */
static bool parse_tcp_option(const char *arg, tcp_option_t *opt) {
    char option_name[64];
    char value_str[128];

    if (sscanf(arg, "%63[^:]:%127s", option_name, value_str) != 2) {
        fprintf(stderr, "Invalid format for -m argument: %s (expected OPTION:value)\n", arg);
        return false;
    }

    const tcp_option_map_t *mapping = find_option_mapping(option_name);
    if (mapping == NULL) {
        fprintf(stderr, "Unknown socket option: %s\n", option_name);
        return false;
    }

    opt->level = mapping->level;
    opt->optname = mapping->optname;
    opt->type = mapping->type;

    /* Parse value based on type */
    switch (mapping->type) {
        case OPT_TYPE_INT: {
            long val;
            if (sscanf(value_str, "%ld", &val) != 1) {
                fprintf(stderr, "Invalid integer value for %s: %s\n", option_name, value_str);
                return false;
            }
            opt->value.int_val = (int)val;
            break;
        }
        case OPT_TYPE_TIMEVAL: {
            double seconds;
            if (sscanf(value_str, "%lf", &seconds) != 1) {
                fprintf(stderr, "Invalid time value for %s: %s (expected decimal seconds)\n",
                        option_name, value_str);
                return false;
            }
            opt->value.tv_val.tv_sec = (time_t)seconds;
            opt->value.tv_val.tv_usec = (suseconds_t)((seconds - (time_t)seconds) * 1000000);
            break;
        }
        case OPT_TYPE_LINGER: {
            int onoff, linger_time;
            if (sscanf(value_str, "%d,%d", &onoff, &linger_time) == 2) {
                opt->value.linger_val.l_onoff = onoff;
                opt->value.linger_val.l_linger = linger_time;
            } else if (sscanf(value_str, "%d", &linger_time) == 1) {
                /* If only one value, assume onoff=1 */
                opt->value.linger_val.l_onoff = 1;
                opt->value.linger_val.l_linger = linger_time;
            } else {
                fprintf(stderr, "Invalid linger value for %s: %s (expected seconds or onoff,seconds)\n",
                        option_name, value_str);
                return false;
            }
            break;
        }
        default:
            fprintf(stderr, "Unsupported option type\n");
            return false;
    }

    return true;
}

/* Display all TCP options for a socket */
static void display_tcp_options(int sfd, int pid, int fd) {
    int val;
    socklen_t len;

    printf("=== Connection: pid=%d, fd=%d ===\n", pid, fd);

    /* Query and display each TCP option */
    len = sizeof(val);
    if (getsockopt(sfd, SOL_TCP, TCP_KEEPIDLE, &val, &len) == 0) {
        printf("  TCP_KEEPIDLE:      %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_TCP, TCP_KEEPINTVL, &val, &len) == 0) {
        printf("  TCP_KEEPINTVL:     %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_TCP, TCP_KEEPCNT, &val, &len) == 0) {
        printf("  TCP_KEEPCNT:       %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_TCP, TCP_SYNCNT, &val, &len) == 0) {
        printf("  TCP_SYNCNT:        %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_TCP, TCP_LINGER2, &val, &len) == 0) {
        printf("  TCP_LINGER2:       %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_TCP, TCP_DEFER_ACCEPT, &val, &len) == 0) {
        printf("  TCP_DEFER_ACCEPT:  %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_TCP, TCP_WINDOW_CLAMP, &val, &len) == 0) {
        printf("  TCP_WINDOW_CLAMP:  %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_TCP, TCP_USER_TIMEOUT, &val, &len) == 0) {
        printf("  TCP_USER_TIMEOUT:  %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_TCP, TCP_NODELAY, &val, &len) == 0) {
        printf("  TCP_NODELAY:       %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_TCP, TCP_MAXSEG, &val, &len) == 0) {
        printf("  TCP_MAXSEG:        %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_TCP, TCP_CORK, &val, &len) == 0) {
        printf("  TCP_CORK:          %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_TCP, TCP_QUICKACK, &val, &len) == 0) {
        printf("  TCP_QUICKACK:      %d\n", val);
    }

    /* Get TCP_INFO structure for additional details */
    struct tcp_info info;
    len = sizeof(info);
    if (getsockopt(sfd, SOL_TCP, TCP_INFO, &info, &len) == 0) {
        printf("  TCP State:         %u\n", info.tcpi_state);
        printf("  TCP RTT:           %u us\n", info.tcpi_rtt);
        printf("  TCP RTT Variance:  %u us\n", info.tcpi_rttvar);
        printf("  TCP RTO:           %u us\n", info.tcpi_rto);
        printf("  TCP SND_MSS:       %u\n", info.tcpi_snd_mss);
        printf("  TCP RCV_MSS:       %u\n", info.tcpi_rcv_mss);
        printf("  TCP Retransmits:   %u\n", info.tcpi_retransmits);
        printf("  TCP Total Retrans: %u\n", info.tcpi_total_retrans);
    }

    /* Get socket-level options */
    len = sizeof(val);
    if (getsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, &val, &len) == 0) {
        printf("  SO_KEEPALIVE:      %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &val, &len) == 0) {
        printf("  SO_SNDBUF:         %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_SOCKET, SO_RCVBUF, &val, &len) == 0) {
        printf("  SO_RCVBUF:         %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &val, &len) == 0) {
        printf("  SO_REUSEADDR:      %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, &val, &len) == 0) {
        printf("  SO_REUSEPORT:      %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_SOCKET, SO_ERROR, &val, &len) == 0) {
        printf("  SO_ERROR:          %d\n", val);
    }

    len = sizeof(val);
    if (getsockopt(sfd, SOL_SOCKET, SO_TYPE, &val, &len) == 0) {
        printf("  SO_TYPE:           %d\n", val);
    }

    struct timeval tv;
    len = sizeof(tv);
    if (getsockopt(sfd, SOL_SOCKET, SO_SNDTIMEO, &tv, &len) == 0) {
        printf("  SO_SNDTIMEO:       %ld.%06ld s\n", (long)tv.tv_sec, (long)tv.tv_usec);
    }

    len = sizeof(tv);
    if (getsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, &len) == 0) {
        printf("  SO_RCVTIMEO:       %ld.%06ld s\n", (long)tv.tv_sec, (long)tv.tv_usec);
    }

    struct linger ling;
    len = sizeof(ling);
    if (getsockopt(sfd, SOL_SOCKET, SO_LINGER, &ling, &len) == 0) {
        printf("  SO_LINGER:         l_onoff=%d, l_linger=%d\n", ling.l_onoff, ling.l_linger);
    }

    printf("\n");
}

/* this is how iproute2/misc/ss.c largely does it too */
bool thup_find_pid_and_fd_by_inode(int inode, int *pid, int *fd) {
    DIR *proc_dir = opendir("/proc");

    if (!proc_dir) {
        thup_exit("opendir");
    }

    struct dirent *entry;

    while ((entry = readdir(proc_dir))) {
        int current_pid;
        char fd_dir_path[64];
        struct dirent *fd_entry;
        DIR *fd_dir;

        if (sscanf(entry->d_name, "%d", &current_pid) != 1) {
            continue;
        }

        snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%d/fd", current_pid);

        fd_dir = opendir(fd_dir_path);

        if (!fd_dir) {
            continue;
        }

        while ((fd_entry = readdir(fd_dir))) {
            char target[256];
            char fd_link[512];
            ssize_t len;
            int current_inode;

            snprintf(fd_link, sizeof(fd_link), "%s/%s", fd_dir_path, fd_entry->d_name);

            len = readlink(fd_link, target, sizeof(target) - 1);

            if (len == -1) {
                continue;
            }

            target[len] = '\0';

            if (sscanf(target, "socket:[%d]", &current_inode) == 1 && current_inode == inode) {
                sscanf(fd_entry->d_name, "%d", fd);
                *pid = current_pid;
                closedir(fd_dir);
                closedir(proc_dir);
                return true;
            }
        }
        closedir(fd_dir);
    }

    closedir(proc_dir);

    return false;
}

static int pidfd_open(pid_t pid, unsigned int flags) {
    return syscall(__NR_pidfd_open, pid, flags);
}

static int pidfd_getfd(int pidfd, int targetfd, unsigned int flags) {
    return syscall(__NR_pidfd_getfd, pidfd, targetfd, flags);
}

void thup_parse_tcp_info(struct nlmsghdr *nlh, const char *search_ip, int search_port,
                         tcp_option_t *options, int num_options, bool info_mode) {
    struct inet_diag_msg *diag_msg = NLMSG_DATA(nlh);
    struct rtattr *attr;
    int len = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*diag_msg));

    for (attr = (struct rtattr *)(diag_msg + 1); RTA_OK(attr, len); attr = RTA_NEXT(attr, len)) {
        char src_ip[INET_ADDRSTRLEN];

        inet_ntop(AF_INET, &(diag_msg->id.idiag_dst), src_ip, INET_ADDRSTRLEN);

        if (strcmp(src_ip, search_ip) == 0 && (search_port==0 || ntohs(diag_msg->id.idiag_dport) == search_port)) {
            int pid, fd;

            thup_find_pid_and_fd_by_inode(diag_msg->idiag_inode, &pid, &fd);

            if (pid==0 || fd==0) {
                continue;
            }

            int pidfd = pidfd_open(pid, 0);

            if (pidfd == -1) {
                continue;
            }

            int sfd = pidfd_getfd(pidfd, fd, 0);

            if (sfd == -1) {
                continue;
            }

            if (info_mode) {
                /* Info mode: display all TCP options */
                display_tcp_options(sfd, pid, fd);
            } else {
                /* Normal mode: apply options and shutdown */
                /* Apply user-specified socket options */
                for (int i = 0; i < num_options; i++) {
                    void *optval;
                    socklen_t optlen;

                    switch (options[i].type) {
                        case OPT_TYPE_INT:
                            optval = &options[i].value.int_val;
                            optlen = sizeof(options[i].value.int_val);
                            break;
                        case OPT_TYPE_TIMEVAL:
                            optval = &options[i].value.tv_val;
                            optlen = sizeof(options[i].value.tv_val);
                            break;
                        case OPT_TYPE_LINGER:
                            optval = &options[i].value.linger_val;
                            optlen = sizeof(options[i].value.linger_val);
                            break;
                        default:
                            fprintf(stderr, "Warning: Unknown option type for socket (pid=%d, fd=%d)\n",
                                    pid, fd);
                            continue;
                    }

                    if (setsockopt(sfd, options[i].level, options[i].optname, optval, optlen) == -1) {
                        fprintf(stderr, "Warning: Failed to set socket option %d (level=%d) for socket (pid=%d, fd=%d): %s\n",
                                options[i].optname, options[i].level, pid, fd, strerror(errno));
                    }
                }

                shutdown(sfd, SHUT_RDWR);
            }

            close(sfd);
            close(pidfd);
        }
    }
}

int main(int argc, char *argv[]) {
    tcp_option_t options[MAX_TCP_OPTIONS];
    int num_options = 0;
    const char *search_ip = NULL;
    int search_port = -1;
    bool info_mode = false;
    int opt;

    /* Parse command-line arguments */
    while ((opt = getopt(argc, argv, "im:")) != -1) {
        switch (opt) {
            case 'i':
                info_mode = true;
                break;
            case 'm':
                if (num_options >= MAX_TCP_OPTIONS) {
                    fprintf(stderr, "Error: Too many TCP options (max %d)\n", MAX_TCP_OPTIONS);
                    exit(EXIT_FAILURE);
                }
                if (!parse_tcp_option(optarg, &options[num_options])) {
                    exit(EXIT_FAILURE);
                }
                num_options++;
                break;
            default:
                fprintf(stderr, "Usage: %s [-i] [-m OPTION:value] ... <IP> <port>\n", argv[0]);
                fprintf(stderr, "\nOptions:\n");
                fprintf(stderr, "  -i              Display TCP option information (don't shutdown)\n");
                fprintf(stderr, "  -m OPTION:value Set TCP option before shutdown\n");
                fprintf(stderr, "\nSupported TCP options:\n");
                for (int i = 0; tcp_option_mapping[i].name != NULL; i++) {
                    fprintf(stderr, "  %s\n", tcp_option_mapping[i].name);
                }
                exit(EXIT_FAILURE);
        }
    }

    /* Get positional arguments (IP and port) */
    if (optind + 2 != argc) {
        fprintf(stderr, "Usage: %s [-i] [-m OPTION:value] ... <IP> <port>\n", argv[0]);
        fprintf(stderr, "\nExamples:\n");
        fprintf(stderr, "  %s -i 192.168.1.1 80\n", argv[0]);
        fprintf(stderr, "  %s -m TCP_KEEPINTVL:3 -m TCP_SYNCNT:1 192.168.1.1 80\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    search_ip = argv[optind];
    search_port = atoi(argv[optind + 1]);

    /* Validate: -m options don't make sense with -i */
    if (info_mode && num_options > 0) {
        fprintf(stderr, "Warning: -m options are ignored in info mode (-i)\n");
    }

    int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_INET_DIAG);

    if (sock == -1) {
        thup_exit("socket");
    }

    struct {
        struct nlmsghdr nlh;
        struct inet_diag_req r;
    } req;

    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(req.r));
    req.nlh.nlmsg_type = TCPDIAG_GETSOCK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.r.idiag_family = AF_INET;
    req.r.idiag_states = -1;

    if (send(sock, &req, sizeof(req), 0) == -1) {
        thup_exit("send");
    }

    bool done = false;

    while (!done) {
        char buf[8192];
        int len = recv(sock, buf, sizeof(buf), 0);

        if (len == -1) {
            thup_exit("recv");
        }

        for (struct nlmsghdr *nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, len); nlh = NLMSG_NEXT(nlh, len)) {
            switch (nlh->nlmsg_type) {
                case NLMSG_DONE:
                    done = true;
                    break;
                case NLMSG_ERROR:
                    fprintf(stderr, "Error in netlink response\n");
                    close(sock);
                    exit(EXIT_FAILURE);
                case TCPDIAG_GETSOCK:
                    thup_parse_tcp_info(nlh, search_ip, search_port, options, num_options, info_mode);
                    break;
                default:
                    break;
            }
        }
    }
    close(sock);
    return 0;
}
