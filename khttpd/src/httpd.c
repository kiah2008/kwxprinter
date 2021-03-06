/*
 * httpd.c
 *
 *  Created on: 2017/12/22
 *      Author: Think
 */
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stdlib.h>

#include <log.h>
#include <config.h>
#include <ktypedef.h>
#include <kevent.h>
#include <khandler.h>

#define ISspace(x) isspace((int)(x))

#define SERVER_STRING "Server: khttpd/0.1.1\r\n"

void* accept_request(void* param);
void bad_request(int);
void dump_file(int, FILE *, enum_data_type type);
void cannot_execute(int);
void error_die(const char *);
void execute_cgi(int, const char *, const char *, const char *);
int get_line(int, char *, int);
void headers(int, const char *);
void not_found(int);
void serve_file(int, const char *);
int startListenSocket(u_short *);
void unimplemented(int);
void abandon_remaining(int);

/**********************************************************************/
/* A request has caused a call to accept() on the server port to
 * return.  Process the request appropriately.
 * Parameters: the socket connected to the connfd */
/**********************************************************************/
void* accept_request(void* param) {
	char buf[1024];
	int numchars;
	char method[255];
	char url[255];
	char path[512];
	size_t i, j;
	struct stat st;
	int cgi = 0;
	char *query_string = NULL;
	int connfd = *((int*) param);

	numchars = get_line(connfd, buf, sizeof(buf));

	i = 0;
	j = 0;
	while (!ISspace(buf[j]) && (i < sizeof(method) - 1))
	{
		method[i] = buf[j];
		i++;
		j++;
	}
	method[i] = '\0';
	LOGD("method is %s\n", method);
	if (strcasecmp(method, "GET") && strcasecmp(method, "POST")) {
		LOGD("unimplemented %s\n", method);
		abandon_remaining(connfd);
		unimplemented(connfd);
		close(connfd);
		return NULL;
	}

	if (strcasecmp(method, "POST") == 0) {
		cgi = 1;
	}

	i = 0;
	while (ISspace(buf[j]) && (j < sizeof(buf)))
		j++;

	while (!ISspace(buf[j]) && (i < sizeof(url) - 1) && (j < sizeof(buf))) {
		url[i] = buf[j];
		i++;
		j++;
	}
	url[i] = '\0';

	if (strcasecmp(method, "GET") == 0) {
		query_string = url;

		while ((*query_string != '?') && (*query_string != '\0'))
			query_string++;

		if (*query_string == '?') {
			cgi = 1;
			*query_string = '\0';
			query_string++;
		}
	}

	sprintf(path, "htdocs%s", url);

	if (path[strlen(path) - 1] == '/') {
		strcat(path, "index.html");
	}

	LOGD("request file %s", path);
	if (stat(path, &st) == -1) {
		abandon_remaining(connfd);
		not_found(connfd);
	} else {
		if ((st.st_mode & S_IFMT) == S_IFDIR)
			strcat(path, "/index.html");
		if ((st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP)
				|| (st.st_mode & S_IXOTH)) {
			cgi = 1;
		}

		if (!cgi) {
			serve_file(connfd, path);
		} else {
			execute_cgi(connfd, path, method, query_string);
		}
	}

	close(connfd);
	return NULL;
}

/**********************************************************************/
/* Inform the connfd that a request it has made has a problem.
 * Parameters: connfd socket */
/**********************************************************************/
void bad_request(int connfd) {
	char buf[1024];

	sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
	send(connfd, buf, sizeof(buf), 0);
	sprintf(buf, "Content-type: text/html\r\n");
	send(connfd, buf, sizeof(buf), 0);
	sprintf(buf, "\r\n");
	send(connfd, buf, sizeof(buf), 0);
	sprintf(buf, "<P>Your browser sent a bad request, ");
	send(connfd, buf, sizeof(buf), 0);
	sprintf(buf, "such as a POST without a Content-Length.\r\n");
	send(connfd, buf, sizeof(buf), 0);
}

/**********************************************************************/
/* Put the entire contents of a file out on a socket.  This function
 * is named after the UNIX "cat" command, because it might have been
 * easier just to do something like pipe, fork, and exec("cat").
 * Parameters: the connfd socket descriptor
 *             FILE pointer for the file to cat */
/**********************************************************************/
void dump_file(int connfd, FILE *resource, enum_data_type type) {
	char buf[1024];
	size_t readsize = 0;
	if (type == DATA_BINARY) {
		readsize = fread(buf, 1, sizeof(buf), resource);
		LOGD("read size %d", readsize);
		while (!feof(resource) || readsize > 0) {
			send(connfd, buf, readsize, 0);
			readsize = fread(buf, 1, sizeof(buf), resource);
			LOGD("read size %d", readsize);
		}
	} else {
		fgets(buf, sizeof(buf), resource);
		while (!feof(resource)) {
			send(connfd, buf, strlen(buf), 0);
			fgets(buf, sizeof(buf), resource);
		}
	}
}

/**********************************************************************/
/* Inform the connfd that a CGI script could not be executed.
 * Parameter: the connfd socket descriptor. */
/**********************************************************************/
void cannot_execute(int connfd) {
	char buf[1024];

	sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "Content-type: text/html\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
	send(connfd, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Print out an error message with perror() (for system errors; based
 * on value of errno, which indicates system call errors) and exit the
 * program indicating an error. */
/**********************************************************************/
void error_die(const char *sc) {
	LOGD("%s", sc);
	perror(sc);
	exit(1);
}

/**********************************************************************/
/* ִ��һ��cgi�ű�,����һЩ��������

 * Parameters: connfd   socket descriptor
 *             path     to the CGI script */
/**********************************************************************/
void execute_cgi(int connfd, const char *path, const char *method,
		const char *query_string) {
	char buf[1024];
	int cgi_output[2];
	int cgi_input[2];
	pid_t pid;
	int status;
	int i;
	char c;
	int numchars = 1;
	int content_length = -1;

	buf[0] = 'A';
	buf[1] = '\0';

	LOGD("execute cgi path %s, method %s\n", path, method);
	if (strcasecmp(method, "GET") == 0) {
		while ((numchars > 0) && strcmp("\n", buf)) /* read & discard headers */
			numchars = get_line(connfd, buf, sizeof(buf));
	} else {
		//ֻ�� POST �����ż���������
		numchars = get_line(connfd, buf, sizeof(buf));
		//���ѭ����Ŀ���Ƕ���ָʾ body ���ȴ�С�Ĳ���������¼ body �ĳ��ȴ�С������� header ����Ĳ���һ�ɺ���
		//ע������ֻ���� header �����ݣ�body ������û�ж�
		while ((numchars > 0) && strcmp("\n", buf)) {
			buf[15] = '\0';
			if (strcasecmp(buf, "Content-Length:") == 0)
				content_length = atoi(&(buf[16])); //��¼ body �ĳ��ȴ�С
			numchars = get_line(connfd, buf, sizeof(buf));
		}

		//��� http ����� header û��ָʾ body ���ȴ�С�Ĳ������򱨴���
		if (content_length == -1) {
			bad_request(connfd);
			return;
		}
	}

	sprintf(buf, "HTTP/1.0 200 OK\r\n");
	send(connfd, buf, strlen(buf), 0);

	if (pipe(cgi_output) < 0) {
		cannot_execute(connfd);
		return;
	}
	if (pipe(cgi_input) < 0) {
		cannot_execute(connfd);
		return;
	}

	if ((pid = fork()) < 0) {
		cannot_execute(connfd);
		return;
	}

	if (pid == 0) {
		/* child: CGI script */
		char meth_env[255];
		char query_env[255];
		char length_env[255];

		dup2(cgi_output[1], 1);
		dup2(cgi_input[0], 0);
		close(cgi_output[0]);
		close(cgi_input[1]);

		//����һ����������
		sprintf(meth_env, "REQUEST_METHOD=%s", method);

		//��������������ӽ��ӽ��̵����л�����
		putenv(meth_env);

		//����http ����Ĳ�ͬ���������첢�洢��ͬ�Ļ�������
		if (strcasecmp(method, "GET") == 0) {
			sprintf(query_env, "QUERY_STRING=%s", query_string);
			putenv(query_env);
		} else {
			/* POST */
			sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
			putenv(length_env);
		}

		execl(path, path, NULL);
		exit(0);

	} else { /* parent */
		close(cgi_output[1]);
		close(cgi_input[0]);

		if (strcasecmp(method, "POST") == 0) {

			for (i = 0; i < content_length; i++) {
				recv(connfd, &c, 1, 0);
				write(cgi_input[1], &c, 1);
			}
		}

		while (read(cgi_output[0], &c, 1) > 0)
			send(connfd, &c, 1, 0);

		close(cgi_output[0]);
		close(cgi_input[1]);
		waitpid(pid, &status, 0);
	}
}

/**********************************************************************/
/* ��socket��ȡһ�С�
 * ������Ի���(\n)���߻س�(\r)����CRLF(\r\n)��Ϊ����,��ʹ��'\0'ֹͣ�ַ�����ȡ
 * ���buffer��ȡ�궼û�з��ֻ��з�,ʹ��'\0'�����ַ���
 * ����ϱ���������һ������ֹ��������,���ᱻ�滻Ϊ\n,������ĩβ����'\0'
 * ��˼���ǲ��ܻ��з���\r����\n����\r\n�����ᱻ�滻Ϊ\n\0
 *
 * ���ڻس��ͻ��У�
 * '\r'�ǻس���ʹ��굽���ף���carriage return��
 * '\n'�ǻ��У�ʹ�������һ�񣬣�line feed / newline��
 * '\r\n'����CRLF��
 *
 * Parameters: the socket descriptor
 *             the buffer to save the data in
 *             the size of the buffer
 * Returns: the number of bytes stored (excluding null) */
/**********************************************************************/
int get_line(int sock, char *buf, int size) {
	int i = 0;
	char c = '\0'; //���䵽��β���ַ�
	int n;

	while ((i < size - 1) && (c != '\n')) {
		n = recv(sock, &c, 1, 0);
		if (n > 0) {
			if (c == '\r') {

				n = recv(sock, &c, 1, MSG_PEEK);
				if ((n > 0) && (c == '\n'))
					recv(sock, &c, 1, 0);
				else
					c = '\n';
			}

			buf[i] = c;
			i++;
		} else {
			c = '\n';
		}
	}

	buf[i] = '\0';

	return (i);
}

void abandon_remaining(int connfd) {
	int numchars = 1;
	char buf[1024];

	int content_length = -1;
	int i;
	char c;

	buf[0] = 'A';
	buf[1] = '\0';

	while ((numchars > 0) && strcmp("\n", buf)) {
		buf[15] = '\0';
		if (strcasecmp(buf, "Content-Length:") == 0) {
			content_length = atoi(&(buf[16]));
		}
		numchars = get_line(connfd, buf, sizeof(buf));
	}

	if (content_length != -1) {
		for (i = 0; i < content_length; i++) {
			recv(connfd, &c, 1, 0);
		}
	}
}

/**********************************************************************/
/* Return the informational HTTP headers about a file. */
/* Parameters: the socket to print the headers on
 *             the name of the file */
/**********************************************************************/
void headers(int connfd, const char *filename) {
	char buf[1024];
	(void) filename; /* could use filename to determine file type */

	strcpy(buf, "HTTP/1.0 200 OK\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, SERVER_STRING);
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(connfd, buf, strlen(buf), 0);
	strcpy(buf, "\r\n");
	send(connfd, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Give a connfd a 404 not found status message. */
/**********************************************************************/
void not_found(int connfd) {
	char buf[1024];

	sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, SERVER_STRING);
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "your request because the resource specified\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "is unavailable or nonexistent.\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "</BODY></HTML>\r\n");
	send(connfd, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Send a regular file to the connfd.  Use headers, and report
 * errors to connfd if they occur.
 * Parameters: a pointer to a file structure produced from the socket
 *              file descriptor
 *             the name of the file to serve */
/**********************************************************************/
void serve_file(int connfd, const char *filename) {
	FILE *resource = NULL;
	int numchars = 1;
	char buf[1024];

	buf[0] = 'A';
	buf[1] = '\0';

	LOGD("serve file %s\n", filename);
	while ((numchars > 0) && strcmp("\n", buf)) /* read & discard headers */
		numchars = get_line(connfd, buf, sizeof(buf));

	resource = fopen(filename, "r");
	if (resource == NULL) {
		not_found(connfd);
	} else {
		headers(connfd, filename);
		dump_file(connfd, resource, DATA_BINARY);
	}

	fclose(resource);
}

/**********************************************************************/
 /* Parameters: pointer to variable containing the port to connect on
 * Returns: the socket */
/**********************************************************************/
int startListenSocket(u_short *port) {
	int httpd = 0;
	struct sockaddr_in name;

	httpd = socket(PF_INET, SOCK_STREAM, 0);
	if (httpd == -1)
		error_die("socket");

	int reuse = 1;
	if (setsockopt(httpd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))
			< 0) {
		error_die("setsockopet");
	}

	memset(&name, 0, sizeof(name));
	name.sin_family = AF_INET;
	name.sin_port = htons(*port);
	name.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(httpd, (struct sockaddr *) &name, sizeof(name)) < 0)
		error_die("bind");
	if (*port == 0) /* if dynamically allocating a port */
	{
		int namelen = sizeof(name);
		if (getsockname(httpd, (struct sockaddr *) &name,
				(socklen_t *) &namelen) == -1)
			error_die("getsockname");
		*port = ntohs(name.sin_port);
	}

	if (listen(httpd, KMAX_CONNECTIONS) < 0)
		error_die("listen");
	return httpd;
}

/**********************************************************************/
/* ֪ͨconnfd�����web����û��ʵ��                                    */
/**********************************************************************/
void unimplemented(int connfd) {
	char buf[1024];

	sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, SERVER_STRING);
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "</TITLE></HEAD>\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
	send(connfd, buf, strlen(buf), 0);
	sprintf(buf, "</BODY></HTML>\r\n");
	send(connfd, buf, strlen(buf), 0);
}

/**********************************************************************/

int main(void) {
	int listenfd = -1;
	u_short port = KPORT;
	int connfd_sock = -1;

	struct sockaddr_in client;
	int client_len = sizeof(client);
	char client_ip[16] = { 0 };

	pthread_t newthread;
	OPENLOG();
	if(event_init(event_handler, event_handler)) {
		LOGE("event init failed!");
	}

	listenfd = startListenSocket(&port);
	LOGD("httpd running on port %d\n", port);
	while (1) {
		//loop waiting for client connection
		connfd_sock = accept(listenfd, (struct sockaddr *) &client,
				(socklen_t *) &client_len);
		if (connfd_sock == -1) {
			error_die("accept");
		}
		strcpy(client_ip, inet_ntoa(client.sin_addr));
		LOGD("recv client %s\n", client_ip);
		if (pthread_create(&newthread, NULL, accept_request,
				(void*) &connfd_sock) != 0)
			perror("pthread_create");
	}
	close(listenfd);
	CLOSELOG();
	LOGD("exiting");
	return (0);
}
