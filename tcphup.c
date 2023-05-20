/* SPDX-License-Identifier: MIT */
/*
 * tcphup.c	"tcphup.c", manipulate existing tcp connections
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

#define thup_exit(msg) perror(msg); exit(EXIT_FAILURE);

static struct thup_filter {
	char *ip_a;
	u32 port;
} thup_filter = {
	.ip_a = NULL,
	.port = 0,
};

const char *argp_program_version = "tcphup 0.1";
const char *argp_program_bug_address =
"https://git.vip.ebay.com/jjawed/tcphup";
const char argp_program_doc[] =
"Manipulate existing tcp connections\n"
"\n"
"USAGE: tcphup [options]\n"
"\n"
"EXAMPLES:\n"
"	tcphup -k 10.0.2.15 -p 443	# close all connections to 10.0.2.15 on port 443\n"
"	tcphup -k 10.0.2.15		# close all connections to 10.0.2.15, regardless of port\n";

static const struct argp_option opts[] = {
	{ NULL, 'k', "", 0, "IP address to kill"},
	{ NULL, 'p', "", 0, "Port"},
	{ NULL, 'h', NULL, OPTION_HIDDEN, "Show the full help" },
	{},
};

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	switch (key) {
		case 'h':
			argp_state_help(state, stderr, ARGP_HELP_STD_HELP);
			break;
		case 'k':
			thup_filter.ip_a = arg;
			break;
		case 'p':
			thup_filter.port = atoi(arg);
			break;
		case ARGP_KEY_END:
			if(!tdc_opts.fwd_dns) {
				fprintf(stderr, "FATAL: failed to get forward server, did you set it with -s?\n");
				argp_usage(state);
			}
		default:
			return ARGP_ERR_UNKNOWN;
	}
	return 0;
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

void thup_parse_tcp_info(struct nlmsghdr *nlh, const char *search_ip, int search_port) {
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

			shutdown(sfd, SHUT_RDWR);

			close(sfd);
			close(pidfd);
		}
	}
}

int main(int argc, char *argv[]) {
	static const struct argp argp = {
		.options = opts,
		.parser = parse_arg,
		.doc = argp_program_doc,
	};

	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);

	if (err)
		return err;

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
					thup_parse_tcp_info(nlh, thup_filter.ip_a, thup_filter.port);
					break;
				default:
					break;
			}
		}
	}
	close(sock);
	return 0;
}
