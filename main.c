/*
 * This is the main module of the CNTLM
 *
 * CNTLM is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * CNTLM is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
 * St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright (c) 2007 David Kubicek
 *
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <pthread.h>
#include <limits.h>
#include <stdio.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netdb.h>
#include <ctype.h>
#include <pwd.h>
#include <fcntl.h>
#include <syslog.h>
#include <termios.h>
#include <fnmatch.h>
#include <assert.h>
#ifdef __CYGWIN__
#include <windows.h>
#endif

/*
 * Some helping routines like linked list manipulation substr(), memory
 * allocation, NTLM authentication routines, etc.
 */
#include "config/config.h"
#include "socket.h"
#include "utils.h"
#include "ntlm.h"
#include "swap.h"
#include "config.h"
#include "auth.h"
#include "http.h"
#include "globals.h"
#include "pages.h"
#include "forward.h"				/* code serving via parent proxy */
#include "direct.h"				/* code serving directly without proxy */
#ifdef __CYGWIN__
#include "sspi.h"				/* code for SSPI management */
#endif

#ifdef ENABLE_KERBEROS
#include "kerberos.h"
#endif
#ifdef ENABLE_PACPARSER
#include <pacparser.h>
#endif

#define STACK_SIZE	sizeof(void *)*8*1024

/*
 * Global "read-only" data initialized in main(). Comments list funcs. which use
 * them. Having these global avoids the need to pass them to each thread and
 * from there again a few times to inner calls.
 */
int debug = 0;					/* all debug printf's and possibly external modules */

/**
 * Values and their meaning:
 * 0 - no requests are logged - default
 * 1 - requests are logged (old behavior)
 */
int request_logging_level = 0;

struct auth_s *g_creds = NULL;			/* throughout the whole module */

int quit = 0;					/* sighandler() */
int ntlmbasic = 0;				/* forward_request() */
int serialize = 0;
int scanner_plugin = 0;
long scanner_plugin_maxsize = 0;

/*
 * List of finished threads. Each forward_request() thread adds itself to it when
 * finished. Main regularly joins and removes all tid's in there.
 */
plist_t threads_list = NULL;
pthread_mutex_t threads_mtx = PTHREAD_MUTEX_INITIALIZER;

/*
 * List of cached connections. Accessed by each thread forward_request().
 */
plist_t connection_list = NULL;
pthread_mutex_t connection_mtx = PTHREAD_MUTEX_INITIALIZER;

/*
 * List of available proxies and current proxy id for proxy_connect().
 */
int parent_count = 0;
plist_t parent_list = NULL;

/*
 * List of custom header substitutions, SOCKS5 proxy users and
 * UserAgents for the scanner plugin.
 */
hlist_t header_list = NULL;			/* forward_request() */
hlist_t users_list = NULL;			/* socks5_thread() */
plist_t scanner_agent_list = NULL;		/* scanner_hook() */
plist_t noproxy_list = NULL;			/* proxy_thread() */

#ifdef ENABLE_PACPARSER
/* 1 = Pacparser engine is initialized and in use. */
int pacparser_initialized = 0;

/*
 * Pacparser Mutex
 */
pthread_mutex_t pacparser_mtx = PTHREAD_MUTEX_INITIALIZER;
#endif

/*
 * General signal handler. If in debug mode, quit immediately.
 */
void sighandler(int p) {
	if (!quit) {
		syslog(LOG_INFO, "Signal %d received, issuing clean shutdown\n", p);
#ifdef ENABLE_PACPARSER
		if (pacparser_initialized) {
			pacparser_cleanup();
		}
#endif
	} else
		syslog(LOG_INFO, "Signal %d received, forcing shutdown\n", p);

	if (quit++ || debug)
		quit++;
}

/*
 * Parse proxy parameter and add it to the global list.
 */
int parent_add(char *parent, int port) {
	int len;
	int i;
	char *spec;
	char *tmp;
	proxy_t *aux;
	struct addrinfo *addresses;

	/*
	 * Check format and parse it.
	 */
	spec = strdup(parent);
	char *q = strrchr(spec, ':');
	if (q != NULL) {
		int p;
		p = (int)(q - spec);
		if(spec[0] == '[' && spec[p-1] == ']') {
			tmp = substr(spec, 1, p-2);
	        } else {
			tmp = substr(spec, 0, p);
		}

		port = atoi(spec+p+1);
		if (!port || !so_resolv(&addresses, tmp, port)) {
			syslog(LOG_ERR, "Cannot resolve listen address %s\n", spec);
			myexit(1);
		}
	} else {
		syslog(LOG_ERR, "Cannot resolve listen address %s\n", spec);
		myexit(1);
	}

	aux = (proxy_t *)zmalloc(sizeof(proxy_t));
#ifdef ENABLE_PACPARSER
	aux->type = PROXY;
#endif
	strlcpy(aux->hostname, tmp, sizeof(aux->hostname));
	aux->port = port;
	aux->resolved = 0;
	aux->addresses = NULL;
	parent_list = plist_add(parent_list, ++parent_count, (char *)aux);

	free(spec);
	free(tmp);
	return 1;
}

#ifdef ENABLE_PACPARSER
/*
 * Create list of proxy_t structs parsed from the PAC string returned
 * by Pacparser.
 * TODO: Harden against malformed pacp_str.
 */
plist_t pac_create_list(plist_t paclist, char *pacp_str) {
	int paclist_count = 0;
	char *pacp_tmp = NULL;
	char *cur_proxy = NULL;

	if (pacp_str == NULL) {
		paclist = NULL;
		return 0;
	}

	/* Make a copy of shared PAC string pacp_str (coming
	 * from pacparser) to avoid manipulation by strsep.
	 */
	pacp_tmp = zmalloc(sizeof(char) * strlen(pacp_str) + 1);
	strcpy(pacp_tmp, pacp_str);

	cur_proxy = strsep(&pacp_tmp, ";");

	if (debug)
		printf("Parsed PAC Proxies:\n");

	while (cur_proxy != NULL) {
		int type = DIRECT; // default is DIRECT
		char *type_str = NULL;
		char *hostname = NULL;
		char *port = NULL;
		proxy_t *aux;

		/* skip whitespace after semicolon */
		if (*cur_proxy == ' ')
			cur_proxy = cur_proxy + 1;

		type_str = strsep(&cur_proxy, " ");
		if (strcmp(type_str, "PROXY") == 0) {
			type = PROXY; // TODO: support more types
			hostname = strsep(&cur_proxy, ":");
			port = cur_proxy; // last token is always the port
		}

		if (debug) {
			if (type != DIRECT) {
				printf("   %s %s %s\n", type_str, hostname, port);
			} else {
				printf("   %s\n", type_str);
			}
		}

		aux = (proxy_t *)zmalloc(sizeof(proxy_t));
		aux->type = type;
		if (type == PROXY) {
			strlcpy(aux->hostname, hostname, sizeof(aux->hostname));
			aux->port = atoi(port);
			aux->resolved = 0;
		}
		paclist = plist_add(paclist, ++paclist_count, (char *)aux);
		cur_proxy = strsep(&pacp_tmp, ";"); /* get next proxy */
	}

	if (debug) {
		printf("Created PAC list with %d item(s):\n", paclist_count);
		plist_dump(paclist);
	}

	free(pacp_tmp);

	return paclist;
}
#endif

/*
 * Register and bind new proxy service port.
 */
void listen_add(const char *service, plist_t *list, char *spec, int gateway) {
	struct addrinfo *addresses;
	int i;
	int p;
	int len;
	int port;
	char *tmp;

	char *q = strrchr(spec, ':');
	if (q != NULL) {
		p = (int)(q - spec);
		if(spec[0] == '[' && spec[p-1] == ']') {
			tmp = substr(spec, 1, p-2);
	        } else {
			tmp = substr(spec, 0, p);
		}

		port = atoi(spec+p+1);
		if (!port || !so_resolv(&addresses, tmp, port)) {
			syslog(LOG_ERR, "Cannot resolve listen address %s\n", spec);
			myexit(1);
		}

		free(tmp);
	} else {
		port = atoi(spec);
		if (!port) {
			syslog(LOG_ERR, "Cannot resolve listen address %s\n", spec);
			myexit(1);
		}
		so_resolv_wildcard(&addresses, port, gateway);
	}

	so_listen(list, addresses, NULL);
	freeaddrinfo(addresses);
}

/*
 * Register a new tunnel definition, bind service port.
 */
void tunnel_add(plist_t *list, char *spec, int gateway) {
	struct addrinfo *addresses;
	int i;
	int len;
	int count;
	int pos;
	int port;
	char *field[4];
	char *tmp;

	spec = strdup(spec);
	len = strlen(spec);
	field[0] = spec;
	for (count = 1, i = 0; count < 4 && i < len; ++i)
		if (spec[i] == ':') {
			spec[i] = 0;
			field[count++] = spec+i+1;
		}

	pos = 0;
	if (count == 4) {
		port = atoi(field[pos+1]);
		if (!port || !so_resolv(&addresses, field[pos], port)) {
			syslog(LOG_ERR, "Cannot resolve tunnel bind address: %s:%s\n", field[pos], field[pos+1]);
			myexit(1);
		}
		pos++;
	} else {
		port = atoi(field[pos]);
		if(!port) {
			syslog(LOG_ERR, "Invalid tunnel local port: %s\n", field[pos]);
			myexit(1);
		}
		so_resolv_wildcard(&addresses, port, gateway);
	}

	if (count-pos == 3) {
		if (!strlen(field[pos+1]) || !strlen(field[pos+2])) {
			syslog(LOG_ERR, "Invalid tunnel target: %s:%s\n", field[pos+1], field[pos+2]);
			myexit(1);
		}

		const size_t tmp_len = strlen(field[pos+1]) + strlen(field[pos+2]) + 2 + 1;
		tmp = zmalloc(tmp_len);
		strlcpy(tmp, field[pos+1], tmp_len);
		strlcat(tmp, ":", tmp_len);
		strlcat(tmp, field[pos+2], tmp_len);

		i = so_listen(list, addresses, tmp);
		if (i == 0) {
			syslog(LOG_INFO, "New tunnel to %s\n", tmp);
		} else {
			syslog(LOG_ERR, "Unable to bind tunnel");
			free(tmp);
		}
	} else {
		printf("Tunnel specification incorrect ([laddress:]lport:rserver:rport).\n");
		myexit(1);
	}

	free(spec);
	freeaddrinfo(addresses);
}

/*
 * Add no-proxy hostname/IP
 */
plist_t noproxy_add(plist_t list, char *spec) {
	char *tok;
	char *save;

	tok = strtok_r(spec, ", ", &save);
	while ( tok != NULL ) {
		if (debug)
			printf("Adding no-proxy for: '%s'\n", tok);
		list = plist_add(list, 0, strdup(tok));
		tok = strtok_r(NULL, ", ", &save);
	}

	return list;
}

int noproxy_match(const char *addr) {
	plist_const_t list;

	list = noproxy_list;
	while (list) {
		if (list->aux && strlen(list->aux)
				&& fnmatch(list->aux, addr, 0) == 0) {
			if (debug)
				printf("MATCH: %s (%s)\n", addr, (char *)list->aux);
			return 1;
		} else if (debug)
			printf("   NO: %s (%s)\n", addr, (char *)list->aux);

		list = list->next;
	}

	return 0;
}

/*
 * Proxy thread - decide between direct and forward based on NoProxy
 * TODO: update
 */
void *proxy_thread(void *thread_data) {
	rr_data_t request;
	rr_data_t ret;
	int keep_alive;				/* Proxy-Connection */
#ifdef ENABLE_PACPARSER
	int pac_found = 0;
	char *pacp_str;

	plist_t pac_list = NULL;
#endif
	int cd = ((struct thread_arg_s *)thread_data)->fd;

	do {
		ret = NULL;
		keep_alive = 0;

		if (debug) {
			printf("\n******* Round 1 C: %d *******\n", cd);
			printf("Reading headers (%d)...\n", cd);
		}

		request = new_rr_data();
		if (!headers_recv(cd, request)) {
			free_rr_data(&request);
			break;
		}

#ifdef ENABLE_PACPARSER
		if (!pac_found) {
			pac_found = 1;

			/*
			 * Create proxy list for request from PAC file.
			 */
			pthread_mutex_lock(&pacparser_mtx);
			pacp_str = pacparser_find_proxy(request->url, request->hostname);
			pthread_mutex_unlock(&pacparser_mtx);

			pac_list = pac_create_list(pac_list, pacp_str);
		}
#endif

		do {
			/*
			 * Are we being returned a request by forward_request or direct_request?
			 */
			if (ret) {
				free_rr_data(&request);
				request = ret;

#ifdef ENABLE_PACPARSER
				/*
				 * Create proxy list for new request from PAC file.
				 */
				pthread_mutex_lock(&pacparser_mtx);
				pacp_str = pacparser_find_proxy(request->url, request->hostname);
				pthread_mutex_unlock(&pacparser_mtx);

				plist_free(pac_list);
				pac_list = NULL;
				pac_list = pac_create_list(pac_list, pacp_str);
#endif
			}

			keep_alive = hlist_subcmp(request->headers, "Proxy-Connection", "keep-alive");

			if (noproxy_match(request->hostname)) {
				/* No-proxy-list has highest precedence */
				ret = direct_request(thread_data, request);
#ifdef ENABLE_PACPARSER
			} else if (pacparser_initialized) {
				/* If PAC is available, use it to serve request. */
				ret = pac_forward_request(thread_data, request, pac_list);
			} else {
				/* Else use statically configured proxies. */
				ret = forward_request(thread_data, request, NULL);
			}
#else
			}
			else
				ret = forward_request(thread_data, request);
#endif

			if (debug)
				printf("proxy_thread: request rc = %p\n", (void *)ret);
#ifdef ENABLE_PACPARSER
		} while (ret != NULL && ret != (void *)-1 && ret != (void *)-2);
#else
		} while (ret != NULL && ret != (void *)-1);
#endif

		free_rr_data(&request);
	/*
	 * If client asked for proxy keep-alive, loop unless the last server response
	 * requested (Proxy-)Connection: close.
	 */
#ifdef ENABLE_PACPARSER
	} while (keep_alive && ret != (void *)-1 && ret != (void *)-2 && !serialize);
#else
	} while (keep_alive && ret != (void *)-1 && !serialize);
#endif

	/*
	 * Add ourselves to the "threads to join" list.
	 */
	if (!serialize) {
		pthread_mutex_lock(&threads_mtx);
		pthread_t thread_id = pthread_self();
		threads_list = plist_add(threads_list, (unsigned long)thread_id, NULL);
		pthread_mutex_unlock(&threads_mtx);
	}

#ifdef ENABLE_PACPARSER
	plist_free(pac_list);
#endif
	free(thread_data);
	close(cd);

	return NULL;
}

/*
 * Tunnel/port forward thread - this method is obviously better solution than using extra
 * tools like "corkscrew" which after all require us for authentication and tunneling
 * their HTTP CONNECT in the first place.
 */
void *tunnel_thread(void *thread_data) {
	char *hostname;
	char *pos;

	assert(thread_data != NULL);
	const char * const thost = ((struct thread_arg_s *)thread_data)->target;

	hostname = strdup(thost);
	if ((pos = strchr(hostname, ':')) != NULL)
		*pos = 0;

	if (noproxy_match(hostname))
		direct_tunnel(thread_data);
	else
		forward_tunnel(thread_data);

	free(hostname);
	free(thread_data);

	/*
	 * Add ourself to the "threads to join" list.
	 */
	pthread_mutex_lock(&threads_mtx);
	pthread_t thread_id = pthread_self();
	threads_list = plist_add(threads_list, (unsigned long)thread_id, NULL);
	pthread_mutex_unlock(&threads_mtx);

	return NULL;
}

/*
 * SOCKS5 thread
 */
void *socks5_thread(void *thread_data) {
	static const uint8_t SOCKS5_AUTH_NO_AUTHENTICATION_REQUIRED = 0x00;
	static const uint8_t SOCKS5_AUTH_USERNAME_PASSWORD = 0x02;
	static const uint8_t SOCKS5_AUTH_NO_ACCEPTABLE_METHODS = 0xFF;
	char *thost;
	char *tport;
	char *uname;
	char *upass;
	unsigned short port;
	int ver;
	int r;
	int c;
	int i;
	int w;

	struct auth_s *tcreds = NULL;
	unsigned char *bs = NULL;
	unsigned char *auths = NULL;
	unsigned char *addr = NULL;
	int found = -1;
	int sd = -1;
	int open = !hlist_count(users_list);

	int cd = ((struct thread_arg_s *)thread_data)->fd;
	free(thread_data);

	/*
	 * Check client's version, possibly fuck'em
	 */
	bs = (unsigned char *)zmalloc(10);
	thost = zmalloc(MINIBUF_SIZE);
	tport = zmalloc(MINIBUF_SIZE);
	r = read(cd, bs, 2);
	if (r != 2 || bs[0] != 5)
		goto bailout;

	/*
	 * Read offered auth schemes
	 */
	c = bs[1];
	auths = (unsigned char *)zmalloc(c+1);
	r = read(cd, auths, c);
	if (r != c)
		goto bailout;

	/*
	 * Are we wide open and client is OK with no auth?
	 */
	if (open) {
		for (i = 0; i < c && found < 0; ++i) {
			if (auths[i] == SOCKS5_AUTH_NO_AUTHENTICATION_REQUIRED) {
				found = auths[i];
			}
		}
	}

	/*
	 * If not, accept plain auth if offered
	 */
	if (found < 0) {
		for (i = 0; i < c && found < 0; ++i) {
			if (auths[i] == SOCKS5_AUTH_USERNAME_PASSWORD) {
				found = auths[i];
			}
		}
	}

	/*
	 * If not open and no auth offered or open and auth requested, fuck'em
	 * and complete the handshake
	 */
	if (found < 0) {
		bs[0] = 5;
		bs[1] = SOCKS5_AUTH_NO_ACCEPTABLE_METHODS;
		(void) write_wrapper(cd, bs, 2); // We don't really care about the result
		goto bailout;
	} else {
		bs[0] = 5;
		bs[1] = found;
		w = write_wrapper(cd, bs, 2);
		if (w != 2) {
			syslog(LOG_ERR, "SOCKS5: write() for accepting AUTH method failed.\n");
		}
	}

	/*
	 * Plain auth negotiated?
	 */
	if (found != 0) {
		/*
		 * Check ver and read username len
		 */
		r = read(cd, bs, 2);
		if (r != 2) {
			bs[0] = 1;
			bs[1] = 0xFF;		/* Unsuccessful (not supported) */
			(void) write_wrapper(cd, bs, 2);
			goto bailout;
		}
		c = bs[1]; // ULEN

		/*
		 * Read username and pass len
		 */
		uname = zmalloc(c+1);
		r = read(cd, uname, c+1);
		if (r != c+1) {
			free(uname);
			goto bailout;
		}
		i = uname[c]; // PLEN
		uname[c] = 0;
		c = i;

		/*
		 * Read pass
		 */
		upass = zmalloc(c+1);
		r = read(cd, upass, c);
		if (r != c) {
			free(upass);
			free(uname);
			goto bailout;
		}
		upass[c] = 0;

		/*
		 * Check credentials against the list
		 */
		const char * tmp = hlist_get(users_list, uname);
		if (!hlist_count(users_list) || (tmp && !strcmp(tmp, upass))) {
			bs[0] = 1;
			bs[1] = 0;		/* Success */
		} else {
			bs[0] = 1;
			bs[1] = 0xFF;		/* Failed */
		}

		/*
		 * Send response
		 */
		w = write_wrapper(cd, bs, 2);
		if (w != 2) {
			syslog(LOG_ERR, "SOCKS5: write() for response of credentials check failed.\n");
		}
		free(upass);
		free(uname);

		/*
		 * Fuck'em if auth failed
		 */
		if (bs[1])
			goto bailout;
	}

	/*
	 * Read request type
	 */
	r = read(cd, bs, 4);
	if (r != 4)
		goto bailout;

	/*
	 * Is it connect for supported address type (IPv4 or DNS)? If not, fuck'em
	 */
	if (bs[1] != 1 || (bs[3] != 1 && bs[3] != 3)) {
		bs[0] = 5;
		bs[1] = 2;			/* Not allowed */
		bs[2] = 0;
		bs[3] = 1;			/* Dummy IPv4 */
		memset(bs+4, 0, 6);
		(void) write_wrapper(cd, bs, 10);
		goto bailout;
	}

	/*
	 * Ok, it's connect to a domain or IP
	 * Let's read dest address
	 */
	if (bs[3] == 1) {
		ver = 1;			/* IPv4, we know the length */
		c = 4;
	} else if (bs[3] == 3) {
		uint8_t string_length;
		ver = 2;			/* FQDN, get string length */
		r = read(cd, &string_length, 1);
		if (r != 1)
			goto bailout;
		c = string_length;
	} else
		goto bailout;

	addr = (unsigned char *)zmalloc(c + 10 + 1);
	r = read(cd, addr, c);
	if (r != c)
		goto bailout;
	addr[c] = 0;

	/*
	 * Convert the address to character string
	 */
	if (ver == 1) {
		snprintf(thost, MINIBUF_SIZE, "%d.%d.%d.%d", addr[0], addr[1], addr[2], addr[3]);	/* It's in network byte order */
	} else {
		strlcpy(thost, (char *)addr, MINIBUF_SIZE);
	}

	/*
	 * Read port number and convert to host byte order int
	 */
	r = read(cd, &port, 2);
	if (r != 2)
		goto bailout;

	i = 0;
	if (noproxy_match(thost)) {
		sd = host_connect(thost, ntohs(port));
		i = (sd >= 0);
	} else {
		snprintf(tport, MINIBUF_SIZE, "%d", ntohs(port));
		strlcat(thost, ":", MINIBUF_SIZE);
		strlcat(thost, tport, MINIBUF_SIZE);

		tcreds = new_auth();
		sd = proxy_connect(tcreds);
		if (sd >= 0)
			i = prepare_http_connect(sd, tcreds, thost);
	}

	/*
	 * Direct or proxy connect?
	 */
	if (!i) {
		/*
		 * Connect/tunnel failed, report
		 */
		bs[0] = 5;
		bs[1] = 1;			/* General failure */
		bs[2] = 0;
		bs[3] = 1;			/* Dummy IPv4 */
		memset(bs+4, 0, 6);
		(void) write_wrapper(cd, bs, 10);
		goto bailout;
	} else {
		/*
		 * All right
		 */
		bs[0] = 5;
		bs[1] = 0;			/* Success */
		bs[2] = 0;
		bs[3] = 1;			/* Dummy IPv4 */
		memset(bs+4, 0, 6);
		w = write_wrapper(cd, bs, 10);
		if (w != 10) {
			syslog(LOG_ERR, "SOCKS5: write() for reporting success for connect failed.\n");
		}
	}


	/*
	 * Let's give them bi-directional connection they asked for
	 */
	tunnel(cd, sd);

bailout:
	if (addr)
		free(addr);
	if (auths)
		free(auths);
	if (thost)
		free(thost);
	if (tport)
		free(tport);
	if (bs)
		free(bs);
	if (tcreds)
		free(tcreds);
	if (sd >= 0)
		close(sd);
	close(cd);

	return NULL;
}

int main(int argc, char **argv) {
	char *tmp;
	char *head;
	char *cpassword;
	char *cpassntlm2;
	char *cpassnt;
	char *cpasslm;
	char *cuser;
	char *cdomain;
	char *cworkstation;
	char *cuid;
	char *cpidfile;
	char *cauth;
	struct termios termold;
	struct termios termnew;
	pthread_attr_t pattr;
	pthread_t pthr;
	hlist_const_t list;
	int i;
	int w;

	int cd = 0;
	int help = 0;
	int nuid = 0;
	int ngid = 0;
	int gateway = 0;
	unsigned int tc = 0; ///< Total number of created threads
	unsigned int tj = 0; ///< Total number of joined threads
	int interactivepwd = 0;
	int interactivehash = 0;
	int tracefile = 0;
	int cflags = 0;
	int asdaemon = 1;
	char *myconfig = NULL;
	plist_t tunneld_list = NULL;
	plist_t proxyd_list = NULL;
	plist_t socksd_list = NULL;
	plist_t rules = NULL;
	config_t cf = NULL;
	char *magic_detect = NULL;
#ifdef ENABLE_PACPARSER
	int pac = 0;
	char *pac_file;

	pac_file = zmalloc(PATH_MAX);
#endif

	g_creds = new_auth();
	cuser = zmalloc(MINIBUF_SIZE);
	cdomain = zmalloc(MINIBUF_SIZE);
	cpassword = zmalloc(MINIBUF_SIZE);
	cpassntlm2 = zmalloc(MINIBUF_SIZE);
	cpassnt = zmalloc(MINIBUF_SIZE);
	cpasslm = zmalloc(MINIBUF_SIZE);
	cworkstation = zmalloc(MINIBUF_SIZE);
	cpidfile = zmalloc(MINIBUF_SIZE);
	cuid = zmalloc(MINIBUF_SIZE);
	cauth = zmalloc(MINIBUF_SIZE);

	openlog("cntlm", LOG_CONS, LOG_DAEMON);

#if config_endian == 0
	syslog(LOG_INFO, "Starting cntlm version " VERSION " for BIG endian\n");
#else
	syslog(LOG_INFO, "Starting cntlm version " VERSION " for LITTLE endian\n");
#endif

	while ((i = getopt(argc, argv, ":-:T:a:c:d:fghIl:p:r:su:vw:x:B:F:G:HL:M:N:O:P:R:S:U:X:q:")) != -1) {
		switch (i) {
			case 'a':
				strlcpy(cauth, optarg, MINIBUF_SIZE);
				break;
			case 'B':
				ntlmbasic = 1;
				break;
			case 'c':
				myconfig = strdup(optarg);
				break;
			case 'd':
				strlcpy(cdomain, optarg, MINIBUF_SIZE);
				break;
			case 'x':
#ifdef ENABLE_PACPARSER
				pac = 1;
				/*
				 * Resolve relative paths if necessary.
				 * Don't care if the named file does not exist (ENOENT) because
				 * later on we check the file's availability anyway.
				 */
				if (!realpath(optarg, pac_file) && errno != ENOENT) {
					syslog(LOG_ERR, "Resolving path to PAC file failed: %s\n", strerror(errno));
					myexit(1);
				}
#else
			fprintf(stderr, "-x specified but CNTLM was compiled without PAC support\n");
#endif
				break;
			case 'F':
				cflags = swap32(strtoul(optarg, &tmp, 0));
				break;
			case 'f':
				asdaemon = 0;
				break;
			case 'G':
				if (strlen(optarg)) {
					scanner_plugin = 1;
					if (!scanner_plugin_maxsize)
						scanner_plugin_maxsize = 1;
					i = strlen(optarg) + 3;
					tmp = zmalloc(i);
					snprintf(tmp, i, "*%s*", optarg);
					scanner_agent_list = plist_add(scanner_agent_list, 0, tmp);
				}
				break;
			case 'g':
				gateway = 1;
				break;
			case 'H':
				interactivehash = 1;
				break;
			case 'I':
				interactivepwd = 1;
				break;
			case 'L':
				/*
				 * Parse and validate the argument.
				 * Create a listening socket for tunneling.
				 */
				tunnel_add(&tunneld_list, optarg, gateway);
				break;
			case 'l':
				/*
				 * Create a listening socket for proxy function.
				 */
				listen_add("Proxy", &proxyd_list, optarg, gateway);
				break;
			case 'M':
				magic_detect = strdup(optarg);
				break;
			case 'N':
				noproxy_list = noproxy_add(noproxy_list, tmp=strdup(optarg));
				free(tmp);
				break;
			case 'O':
				listen_add("SOCKS5 proxy", &socksd_list, optarg, gateway);
				break;
			case 'P':
				strlcpy(cpidfile, optarg, MINIBUF_SIZE);
				break;
			case 'p':
				/*
				 * Overwrite the password parameter with '*'s to make it
				 * invisible in "ps", /proc, etc.
				 */
				strlcpy(cpassword, optarg, MINIBUF_SIZE);
				for (i = strlen(optarg)-1; i >= 0; --i)
					optarg[i] = '*';
				break;
			case 'R':
				tmp = strdup(optarg);
				head = strchr(tmp, ':');
				if (!head) {
					fprintf(stderr, "Invalid username:password format for -R: %s\n", tmp);
				} else {
					head[0] = 0;
					users_list = hlist_add(users_list, tmp, head+1,
						HLIST_ALLOC, HLIST_ALLOC);
				}
				break;
			case 'r':
				if (is_http_header(optarg))
					header_list = hlist_add(header_list,
						get_http_header_name(optarg),
						get_http_header_value(optarg),
						HLIST_NOALLOC, HLIST_NOALLOC);
				break;
			case 'S':
				scanner_plugin = 1;
				scanner_plugin_maxsize = atol(optarg);
				break;
			case 's':
				/*
				 * Do not use threads - for debugging purposes only
				 */
				serialize = 1;
				break;
			case 'T':
				debug = 1;
				asdaemon = 0;
				tracefile = open(optarg, O_CREAT | O_TRUNC | O_WRONLY, 0600);
				openlog("cntlm", LOG_CONS | LOG_PERROR, LOG_DAEMON);
				if (tracefile < 0) {
					fprintf(stderr, "Cannot create trace file.\n");
					myexit(1);
				} else {
					printf("Redirecting all output to %s\n", optarg);
					dup2(tracefile, 1);
					dup2(tracefile, 2);
				}
				break;
			case 'U':
				strlcpy(cuid, optarg, MINIBUF_SIZE);
				break;
			case 'u':
				i = strcspn(optarg, "@");
				if (i != strlen(optarg)) {
					strlcpy(cuser, optarg, MIN(MINIBUF_SIZE, i+1));
					strlcpy(cdomain, optarg+i+1, MINIBUF_SIZE);
				} else {
					strlcpy(cuser, optarg, MINIBUF_SIZE);
				}
				break;
			case 'v':
				debug = 1;
				asdaemon = 0;
				openlog("cntlm", LOG_CONS | LOG_PERROR, LOG_DAEMON);
				break;
			case 'w':
				strlcpy(cworkstation, optarg, MINIBUF_SIZE);
				break;
			case 'X':
#ifdef __CYGWIN__
				if (!sspi_set(strdup(optarg)))
				{
					fprintf(stderr, "SSPI initialize failed! Proceeding with SSPI disabled.\n");
				}
#else
				fprintf(stderr, "This feature is available under Windows only!\n");
				help = 1;
#endif
				break;
			case 'q':
				if (*optarg == '0') {
					request_logging_level = 0;
				}
				else if (*optarg == '1') {
					request_logging_level = 1;
				}
				else {
					fprintf(stderr, "Invalid argument for option -q, using default value of %d.\n", request_logging_level);
				}
				break;
			case 'h':
				help = 1;
				break;
			default:
				help = 2;
		}
	}

	/*
	 * Help requested?
	 */
	if (help > 0) {
		printf("CNTLM - Accelerating NTLM Authentication Proxy version " VERSION "\n");
		printf("Copyright (c) 2oo7-2o1o David Kubicek\n\n"
			"This program comes with NO WARRANTY, to the extent permitted by law. You\n"
			"may redistribute copies of it under the terms of the GNU GPL Version 2 or\n"
			"newer. For more information about these matters, see the file LICENSE.\n"
			"For copyright holders of included encryption routines see headers.\n\n");

		FILE * stream = stdout;
		int exit_code = 0;
		if (help > 1) {
			stream = stderr;
			exit_code = 1;
		}

		fprintf(stream, "Usage: %s [-AaBcDdFfgHhILlMPpSsTUuvw] <proxy_host>[:]<proxy_port> ...\n", argv[0]);
		fprintf(stream, "\t-a  ntlm | nt | lm\n"
				"\t    Authentication type - combined NTLM, just LM, or just NT. Default NTLM.\n"
#ifdef ENABLE_KERBEROS
				"\t    GSS activates kerberos auth: you need a cached credential.\n"
#endif
				"\t    It is the most versatile setting and likely to work for you.\n");
		fprintf(stream, "\t-B  Enable NTLM-to-basic authentication.\n");
		fprintf(stream, "\t-c  <config_file>\n"
				"\t    Configuration file. Other arguments can be used as well, overriding\n"
				"\t    config file settings.\n");
		fprintf(stream, "\t-d  <domain>\n"
				"\t    Domain/workgroup can be set separately.\n");
		fprintf(stream, "\t-f  Run in foreground, do not fork into daemon mode.\n");
		fprintf(stream, "\t-F  <flags>\n"
				"\t    NTLM authentication flags.\n");
		fprintf(stream, "\t-G  <pattern>\n"
				"\t    User-Agent matching for the trans-isa-scan plugin.\n");
		fprintf(stream, "\t-g  Gateway mode - listen on all interfaces, not only loopback.\n");
		fprintf(stream, "\t-H  Print password hashes for use in config file (NTLMv2 needs -u and -d).\n");
		fprintf(stream, "\t-h  Print this help info along with version number.\n");
		fprintf(stream, "\t-I  Prompt for the password interactively.\n");
		fprintf(stream, "\t-L  [<saddr>:]<lport>:<rhost>:<rport>\n"
				"\t    Forwarding/tunneling a la OpenSSH. Same syntax - listen on lport\n"
				"\t    and forward all connections through the proxy to rhost:rport.\n"
				"\t    Can be used for direct tunneling without corkscrew, etc.\n");
		fprintf(stream, "\t-l  [<saddr>:]<lport>\n"
				"\t    Main listening port for the NTLM proxy.\n");
		fprintf(stream, "\t-M  <testurl>\n"
				"\t    Magic autodetection of proxy's NTLM dialect.\n");
		fprintf(stream, "\t-N  \"<hostname_wildcard1>[, <hostname_wildcardN>\"\n"
				"\t    List of URL's to serve directly as stand-alone proxy (e.g. '*.local')\n");
		fprintf(stream, "\t-O  [<saddr>:]<lport>\n"
				"\t    Enable SOCKS5 proxy on port lport (binding to address saddr)\n");
		fprintf(stream, "\t-P  <pidfile>\n"
				"\t    Create a PID file upon successful start.\n");
		fprintf(stream, "\t-p  <password>\n"
				"\t    Account password. Will not be visible in \"ps\", /proc, etc.\n");
		fprintf(stream, "\t-q  <level>\n"
				"\t    Controls logging of requests like CONNECT/GET with URL.\n"
				"\t    level can be:\n"
				"\t    0 no requests are logged - default\n"
				"\t    1 requests are logged (old behavior)\n");
		fprintf(stream, "\t-r  \"HeaderName: value\"\n"
				"\t    Add a header substitution. All such headers will be added/replaced\n"
				"\t    in the client's requests.\n");
		fprintf(stream, "\t-S  <size_in_kb>\n"
				"\t    Enable automation of GFI WebMonitor ISA scanner for files < size_in_kb.\n");
		fprintf(stream, "\t-s  Do not use threads, serialize all requests - for debugging only.\n");
		fprintf(stream, "\t-T  <file.log>\n"
				"\t    Redirect all debug information into a trace file for support upload.\n"
				"\t    MUST be the first argument on the command line, implies -v.\n");
		fprintf(stream, "\t-U  <uid>\n"
				"\t    Run as uid. It is an important security measure not to run as root.\n");
		fprintf(stream, "\t-u  <user>[@<domain]\n"
				"\t    Domain/workgroup can be set separately.\n");
		fprintf(stream, "\t-v  Print debugging information.\n");
		fprintf(stderr, "\t-w  <workstation>\n"
				"\t    Some proxies require correct NetBIOS hostname.\n");
#ifdef ENABLE_PACPARSER
		fprintf(stderr, "\t-x  <PAC_file>\n"
				"\t    Specify a PAC file to load.\n");
#endif
		fprintf(stream, "\t-X  <sspi_handle_type>\n"
				"\t    Use SSPI with specified handle type. Works only under Windows.\n"
				"\t    Default is negotiate.\n");
		fprintf(stream, "\n");
		exit(exit_code);
	}

	if (debug) {
		printf("Cntlm debug trace, version " VERSION);
#ifdef __CYGWIN__
		printf(" windows/cygwin port");
#endif
		printf(".\nCommand line: ");
		for (i = 0; i < argc; ++i)
			printf("%s ", argv[i]);
		printf("\n");
	}

	if (myconfig) {
		if (!(cf = config_open(myconfig))) {
			syslog(LOG_ERR, "Cannot access specified config file: %s\n", myconfig);
			myexit(1);
		}
		free(myconfig);
	}

	/*
	 * More arguments on the command-line? Must be proxies.
	 */
	i = optind;
	while (i < argc) {
		tmp = strchr(argv[i], ':');
		parent_add(argv[i], !tmp && i+1 < argc ? atoi(argv[i+1]) : 0);
		i += (!tmp ? 2 : 1);
	}

	/*
	 * No configuration file yet? Load the default.
	 */
#ifdef SYSCONFDIR
	if (!cf) {
#ifdef __CYGWIN__
		tmp = getenv("PROGRAMFILES(X86)");
		if (tmp == NULL || strlen(tmp) == 0)
			tmp = getenv("PROGRAMFILES");
		if (tmp == NULL)
			tmp = "C:\\Program Files";

		head = zmalloc(MINIBUF_SIZE);
		strlcpy(head, tmp, MINIBUF_SIZE);
		strlcat(head, "\\Cntlm\\cntlm.ini", MINIBUF_SIZE);
		cf = config_open(head);
#else
		cf = config_open(SYSCONFDIR "/cntlm.conf");
#endif
		if (debug) {
			if (cf)
				printf("Default config file opened successfully\n");
			else
				syslog(LOG_ERR, "Could not open default config file\n");
		}
	}
#endif

#ifdef __CYGWIN__
	/*
	 * Still no configuration file found?
	 * Check if there is one in the same directory as the executable.
	 */
	if (!cf) {
		const unsigned int path_size = PATH_MAX;
		char path[path_size];

		if (GetModuleFileNameA(NULL, path, path_size) > 0) {
			path[path_size - 1] = '\0';
			// Remove the *.exe part at the end
			for(unsigned int index = strlen(path); index > 0; --index) {
				if(path[index - 1] == '\\') {
					path[index] = '\0';
					break;
				}
			}
			strlcat(path, "cntlm.ini", path_size);
			path[path_size - 1] = '\0';
			cf = config_open(path);
			if (cf) {
				syslog(LOG_INFO, "Using config file %s\n", path);
			}
		}
	}
#endif

	/*
	 * If any configuration file was successfully opened, parse it.
	 */
	if (cf) {

		/*
		 * Check if gateway mode is requested before actually binding any ports.
		 */
		tmp = zmalloc(MINIBUF_SIZE);
		CFG_DEFAULT(cf, "Gateway", tmp, MINIBUF_SIZE);
		if (!strcasecmp("yes", tmp))
			gateway = 1;
		free(tmp);

		/*
		 * Check for NTLM-to-basic settings
		 */
		tmp = zmalloc(MINIBUF_SIZE);
		CFG_DEFAULT(cf, "NTLMToBasic", tmp, MINIBUF_SIZE);
		if (!strcasecmp("yes", tmp))
			ntlmbasic = 1;
		free(tmp);

		/*
		 * Setup the rest of tunnels.
		 */
		while ((tmp = config_pop(cf, "Tunnel"))) {
			tunnel_add(&tunneld_list, tmp, gateway);
			free(tmp);
		}

		/*
		 * Bind the rest of proxy service ports.
		 */
		while ((tmp = config_pop(cf, "Listen"))) {
			listen_add("Proxy", &proxyd_list, tmp, gateway);
			free(tmp);
		}

		/*
		 * Bind the rest of SOCKS5 service ports.
		 */
		while ((tmp = config_pop(cf, "SOCKS5Proxy"))) {
			listen_add("SOCKS5 proxy", &socksd_list, tmp, gateway);
			free(tmp);
		}

		/*
		 * Accept only headers not specified on the command line.
		 * Command line has higher priority.
		 */
		while ((tmp = config_pop(cf, "Header"))) {
			if (is_http_header(tmp)) {
				head = get_http_header_name(tmp);
				if (!hlist_in(header_list, head))
					header_list = hlist_add(header_list, head, get_http_header_value(tmp),
						HLIST_ALLOC, HLIST_NOALLOC);
				free(head);
			} else
				syslog(LOG_ERR, "Invalid header format: %s\n", tmp);

			free(tmp);
		}

#ifdef ENABLE_PACPARSER
		/*
		 * Check if PAC mode is requested.
		 */
		tmp = zmalloc(MINIBUF_SIZE);
		CFG_DEFAULT(cf, "Pac", tmp, MINIBUF_SIZE);
		if (!strcasecmp("yes", tmp)) {
			pac = 1;
		}
		free(tmp);

		CFG_DEFAULT(cf, "PacFile", pac_file, MINIBUF_SIZE);
#endif

		/*
		 * Add the rest of parent proxies.
		 */
		while ((tmp = config_pop(cf, "Proxy"))) {
			parent_add(tmp, 0);
			free(tmp);
		}


		/*
		 * Single options.
		 */
		CFG_DEFAULT(cf, "Auth", cauth, MINIBUF_SIZE);
		CFG_DEFAULT(cf, "Domain", cdomain, MINIBUF_SIZE);
		CFG_DEFAULT(cf, "Password", cpassword, MINIBUF_SIZE);
		CFG_DEFAULT(cf, "PassNTLMv2", cpassntlm2, MINIBUF_SIZE);
		CFG_DEFAULT(cf, "PassNT", cpassnt, MINIBUF_SIZE);
		CFG_DEFAULT(cf, "PassLM", cpasslm, MINIBUF_SIZE);
		CFG_DEFAULT(cf, "Username", cuser, MINIBUF_SIZE);
		CFG_DEFAULT(cf, "Workstation", cworkstation, MINIBUF_SIZE);

#ifdef __CYGWIN__
		/*
		 * Check if SSPI is enabled and it's type.
		 */
		tmp = zmalloc(MINIBUF_SIZE);
		CFG_DEFAULT(cf, "SSPI", tmp, MINIBUF_SIZE);

		if (!sspi_enabled() && strlen(tmp))
		{
			if (!strcasecmp("NTLM", tmp) && !sspi_set(tmp)) // Only NTLM supported for now
			{
				fprintf(stderr, "SSPI initialize failed! Proceeding with SSPI disabled.\n");
			}
		}
		free(tmp);

#endif

		tmp = zmalloc(MINIBUF_SIZE);
		CFG_DEFAULT(cf, "Flags", tmp, MINIBUF_SIZE);
		if (!cflags)
			cflags = swap32(strtoul(tmp, NULL, 0));
		free(tmp);

		tmp = zmalloc(MINIBUF_SIZE);
		CFG_DEFAULT(cf, "ISAScannerSize", tmp, MINIBUF_SIZE);
		if (!scanner_plugin_maxsize && strlen(tmp)) {
			scanner_plugin = 1;
			scanner_plugin_maxsize = atoi(tmp);
		}
		free(tmp);

		while ((tmp = config_pop(cf, "NoProxy"))) {
			if (strlen(tmp)) {
				noproxy_list = noproxy_add(noproxy_list, tmp);
			}
			free(tmp);
		}

		while ((tmp = config_pop(cf, "SOCKS5Users"))) {
			head = strchr(tmp, ':');
			if (!head) {
				syslog(LOG_ERR, "Invalid username:password format for SOCKS5User: %s\n", tmp);
			} else {
				head[0] = 0;
				users_list = hlist_add(users_list, tmp, head+1, HLIST_ALLOC, HLIST_ALLOC);
			}
		}


		/*
		 * Add User-Agent matching patterns.
		 */
		while ((tmp = config_pop(cf, "ISAScannerAgent"))) {
			scanner_plugin = 1;
			if (!scanner_plugin_maxsize)
				scanner_plugin_maxsize = 1;

			if ((i = strlen(tmp))) {
				head = zmalloc(i + 3);
				snprintf(head, i+3, "*%s*", tmp);
				scanner_agent_list = plist_add(scanner_agent_list, 0, head);
			}
			free(tmp);
		}

		/*
		 * Print out unused/unknown options.
		 */
		list = cf->options;
		while (list) {
			syslog(LOG_INFO, "Ignoring config file option: %s\n", list->key);
			list = list->next;
		}
	}

	config_close(cf);


#ifdef ENABLE_PACPARSER
	/* Start Pacparser engine if pac_file available */
	/* TODO: pac file option in config file */
	if (pac) {
		/* Check if PAC file can be opened. */
		FILE *test_fd = NULL;
		if (!(test_fd = fopen(pac_file, "r"))) {
			syslog(LOG_ERR, "Cannot access specified PAC file: '%s'\n", pac_file);
			myexit(1);
		}
		fclose(test_fd);

		/* Initiailize Pacparser. */
		pacparser_init();
		pacparser_parse_pac(pac_file);
		if (debug)
			printf("Pacparser initialized with PAC file %s\n", pac_file);
		// TODO handle parsing errors from pacparser
		pacparser_initialized = 1;
	}

	if (!interactivehash && !parent_list && !pac)
#else
	if (!interactivehash && !parent_list)
#endif
		croak("Parent proxy address missing.\n", interactivepwd || magic_detect);

	if (!interactivehash && !magic_detect && !proxyd_list)
		croak("No proxy service ports were successfully opened.\n", interactivepwd);

	/*
	 * Set default value for the workstation. Hostname if possible.
	 */
	if (!strlen(cworkstation)) {
#if config_gethostname == 1
		gethostname(cworkstation, MINIBUF_SIZE);
#endif
		if (!strlen(cworkstation))
			strlcpy(cworkstation, "cntlm", MINIBUF_SIZE);

		syslog(LOG_INFO, "Workstation name used: %s\n", cworkstation);
	}

	/*
	 * Parse selected NTLM hash combination.
	 */
	if (strlen(cauth)) {
		if (!strcasecmp("ntlm", cauth)) {
			g_creds->hashnt = 1;
			g_creds->hashlm = 1;
			g_creds->hashntlm2 = 0;
		} else if (!strcasecmp("nt", cauth)) {
			g_creds->hashnt = 1;
			g_creds->hashlm = 0;
			g_creds->hashntlm2 = 0;
		} else if (!strcasecmp("lm", cauth)) {
			g_creds->hashnt = 0;
			g_creds->hashlm = 1;
			g_creds->hashntlm2 = 0;
		} else if (!strcasecmp("ntlmv2", cauth)) {
			g_creds->hashnt = 0;
			g_creds->hashlm = 0;
			g_creds->hashntlm2 = 1;
		} else if (!strcasecmp("ntlm2sr", cauth)) {
			g_creds->hashnt = 2;
			g_creds->hashlm = 0;
			g_creds->hashntlm2 = 0;
#ifdef ENABLE_KERBEROS
		} else if (!strcasecmp("gss", cauth)) {
			g_creds->haskrb = KRB_FORCE_USE_KRB;
			g_creds->hashnt = 0;
			g_creds->hashlm = 0;
			g_creds->hashntlm2 = 0;
			syslog(LOG_INFO, "Forcing GSS auth.\n");
#endif
		} else {
			syslog(LOG_ERR, "Unknown NTLM auth combination.\n");
			myexit(1);
		}
	}

	if (socksd_list && !users_list)
		syslog(LOG_WARNING, "SOCKS5 proxy will NOT require any authentication\n");

	if (!magic_detect)
		syslog(LOG_INFO, "Using following NTLM hashes: NTLMv2(%d) NT(%d) LM(%d)\n",
			g_creds->hashntlm2, g_creds->hashnt, g_creds->hashlm);

	if (cflags) {
		syslog(LOG_INFO, "Using manual NTLM flags: 0x%X\n", swap32(cflags));
		g_creds->flags = cflags;
	}

	/*
	 * Last chance to get password from the user
	 */
	if (interactivehash || magic_detect || (interactivepwd && !ntlmbasic)) {
		printf("Password: ");
		tcgetattr(0, &termold);
		termnew = termold;
		termnew.c_lflag &= ~(ISIG | ECHO);
		tcsetattr(0, TCSADRAIN, &termnew);
		tmp = fgets(cpassword, MINIBUF_SIZE, stdin);
		tcsetattr(0, TCSADRAIN, &termold);
		i = strlen(cpassword) - 1;
		if (cpassword[i] == '\n') {
			cpassword[i] = 0;
			if (cpassword[i - 1] == '\r')
				cpassword[i - 1] = 0;
		}
		printf("\n");
	}

	/*
	 * Convert optional PassNT, PassLM and PassNTLMv2 strings to hashes
	 * unless plaintext pass was used, which has higher priority.
	 *
	 * If plain password is present, calculate its NT and LM hashes
	 * and remove it from the memory.
	 */
	if (!strlen(cpassword)) {
		if (strlen(cpassntlm2)) {
			tmp = scanmem(cpassntlm2, 8);
			if (!tmp) {
				syslog(LOG_ERR, "Invalid PassNTLMv2 hash, terminating\n");
				exit(1);
			}
			auth_memcpy(g_creds, passntlm2, tmp, 16);
			free(tmp);
		}
		if (strlen(cpassnt)) {
			tmp = scanmem(cpassnt, 8);
			if (!tmp) {
				syslog(LOG_ERR, "Invalid PassNT hash, terminating\n");
				exit(1);
			}
			auth_memcpy(g_creds, passnt, tmp, 16);
			free(tmp);
		}
		if (strlen(cpasslm)) {
			tmp = scanmem(cpasslm, 8);
			if (!tmp) {
				syslog(LOG_ERR, "Invalid PassLM hash, terminating\n");
				exit(1);
			}
			auth_memcpy(g_creds, passlm, tmp, 16);
			free(tmp);
		}
	} else {
		if (g_creds->hashnt || magic_detect || interactivehash) {
			tmp = ntlm_hash_nt_password(cpassword);
			auth_memcpy(g_creds, passnt, tmp, 21);
			free(tmp);
		} if (g_creds->hashlm || magic_detect || interactivehash) {
			tmp = ntlm_hash_lm_password(cpassword);
			auth_memcpy(g_creds, passlm, tmp, 21);
			free(tmp);
		} if (g_creds->hashntlm2 || magic_detect || interactivehash) {
			tmp = ntlm2_hash_password(cuser, cdomain, cpassword);
			auth_memcpy(g_creds, passntlm2, tmp, 16);
			free(tmp);
		}
		memset(cpassword, 0, strlen(cpassword));
	}

#ifdef ENABLE_KERBEROS
	g_creds->haskrb |= check_credential();
	if(g_creds->haskrb & KRB_CREDENTIAL_AVAILABLE)
		syslog(LOG_INFO, "Using cached credential for GSS auth.\n");
#endif

	auth_strcpy(g_creds, user, cuser);
	auth_strcpy(g_creds, domain, cdomain);
	auth_strcpy(g_creds, workstation, cworkstation);

	free(cuser);
	free(cdomain);
	free(cworkstation);
	free(cpassword);
	free(cpassntlm2);
	free(cpassnt);
	free(cpasslm);
	free(cauth);

	/*
	 * Try known NTLM auth combinations and print which ones work.
	 * User can pick the best (most secure) one as his config.
	 */
	if (magic_detect) {
		magic_auth_detect(magic_detect);
		goto bailout;
	}

	if (interactivehash) {
		if (!is_memory_all_zero(g_creds->passlm, ARRAY_SIZE(g_creds->passlm))) {
			tmp = printmem(g_creds->passlm, 16, 8);
			printf("PassLM          %s\n", tmp);
			free(tmp);
		}

		if (!is_memory_all_zero(g_creds->passnt, ARRAY_SIZE(g_creds->passnt))) {
			tmp = printmem(g_creds->passnt, 16, 8);
			printf("PassNT          %s\n", tmp);
			free(tmp);
		}

		if (!is_memory_all_zero(g_creds->passntlm2, ARRAY_SIZE(g_creds->passntlm2))) {
			tmp = printmem(g_creds->passntlm2, 16, 8);
			printf("PassNTLMv2      %s    # Only for user '%s', domain '%s'\n",
				tmp, g_creds->user, g_creds->domain);
			free(tmp);
		}
		goto bailout;
	}

	/*
	 * If we're going to need a password, check we really have it.
	 */
	if (!ntlmbasic &&
#ifdef ENABLE_KERBEROS
			!g_creds->haskrb &&
#endif
			    ((g_creds->hashnt && is_memory_all_zero(g_creds->passnt, ARRAY_SIZE(g_creds->passnt)))
			 || (g_creds->hashlm && is_memory_all_zero(g_creds->passlm, ARRAY_SIZE(g_creds->passlm)))
			 || (g_creds->hashntlm2 &&  is_memory_all_zero(g_creds->passntlm2, ARRAY_SIZE(g_creds->passntlm2))))) {
		syslog(LOG_ERR, "Parent proxy account password (or required hashes) missing.\n");
		myexit(1);
	}

	/*
	 * Ok, we are ready to rock. If daemon mode was requested,
	 * fork and die. The child will not be group leader anymore
	 * and can thus create a new session for itself and detach
	 * from the controlling terminal.
	 */
	if (asdaemon) {
		if (debug)
			printf("Forking into background as requested.\n");

		i = fork();
		if (i == -1) {
			perror("Fork into background failed");		/* fork failed */
			myexit(1);
		} else if (i)
			myexit(0);					/* parent */

		setsid();
		umask(0);
		w = chdir("/");
		if(w != 0) {
			perror("chdir(\"/\") failed");
		}
		i = open("/dev/null", O_RDWR);
		if (i >= 0) {
			dup2(i, 0);
			dup2(i, 1);
			dup2(i, 2);
			if (i > 2)
				close(i);
		}
	}

	/*
	 * Reinit syslog logging to include our PID, after forking
	 * it is going to be OK
	 */
	if (asdaemon) {
		openlog("cntlm", LOG_CONS | LOG_PID, LOG_DAEMON);
		syslog(LOG_INFO, "Daemon ready");
	} else {
		openlog("cntlm", LOG_CONS | LOG_PID | LOG_PERROR, LOG_DAEMON);
		syslog(LOG_INFO, "Cntlm ready, staying in the foreground");
	}

	/*
	 * Check and change UID.
	 */
	if (strlen(cuid)) {
		if (getuid() && geteuid()) {
			syslog(LOG_WARNING, "No root privileges; keeping identity %d:%d\n", getuid(), getgid());
		} else {
			if (isdigit(cuid[0])) {
				nuid = atoi(cuid);
				ngid = nuid;
				if (nuid <= 0) {
					syslog(LOG_ERR, "Numerical uid parameter invalid\n");
					myexit(1);
				}
			} else {
				const struct passwd * const pw = getpwnam(cuid);
				if (!pw || !pw->pw_uid) {
					syslog(LOG_ERR, "Username %s in -U is invalid\n", cuid);
					myexit(1);
				}
				nuid = pw->pw_uid;
				ngid = pw->pw_gid;
			}
			if (setgid(ngid)) {
				syslog(LOG_ERR, "Setting group identity failed: %s\n", strerror(errno));
				syslog(LOG_ERR, "Terminating\n");
				myexit(1);
			}
			i = setuid(nuid);
			syslog(LOG_INFO, "Changing uid:gid to %d:%d - %s\n", nuid, ngid, strerror(errno));
			if (i) {
				syslog(LOG_ERR, "Terminating\n");
				myexit(1);
			}
		}
	}

	/*
	 * PID file requested? Try to create one (it must not exist).
	 * If we fail, exit with error.
	 */
	if (strlen(cpidfile)) {
		int len;

		umask(0);
		cd = open(cpidfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (cd < 0) {
			syslog(LOG_ERR, "Error creating a new PID file\n");
			myexit(1);
		}

		tmp = zmalloc(50);
		snprintf(tmp, 50, "%d\n", getpid());
		w = write_wrapper(cd, tmp, (len = strlen(tmp)));
		if (w != len) {
			syslog(LOG_ERR, "Error writing to the PID file\n");
			myexit(1);
		}
		free(tmp);
		close(cd);
	}

	/*
	 * Change the handler for signals recognized as clean shutdown.
	 * When the handler is called (termination request), it signals
	 * this news by adding 1 to the global quit variable.
	 */
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, &sighandler);
	signal(SIGTERM, &sighandler);
	signal(SIGHUP, &sighandler);

	/*
	 * Initialize the random number generator
	 */
	srandom(time(NULL));

	/*
	 * This loop iterates over every connection request on any of
	 * the listening ports. We keep the number of created threads.
	 *
	 * We also check the "finished threads" list, threads_list, here and
	 * free the memory of all inactive threads. Then, we update the
	 * number of finished threads.
	 *
	 * The loop ends, when we were "killed" and all threads created
	 * are finished, OR if we were killed more than once. This way,
	 * we have a "clean" shutdown (wait for all connections to finish
	 * after the first kill) and a "forced" one (user insists and
	 * killed us twice).
	 */
	while (quit == 0 || (tc != tj && quit < 2)) {
		struct thread_arg_s *data;
		struct sockaddr_in6 caddr;
		struct timeval tv;
		socklen_t clen;
		fd_set set;
		plist_t t;
		int tid = 0;

		FD_ZERO(&set);

		/*
		 * Watch for proxy ports.
		 */
		t = proxyd_list;
		while (t) {
			FD_SET(t->key, &set);
			t = t->next;
		}

		/*
		 * Watch for SOCKS5 ports.
		 */
		t = socksd_list;
		while (t) {
			FD_SET(t->key, &set);
			t = t->next;
		}

		/*
		 * Watch for tunneled ports.
		 */
		t = tunneld_list;
		while (t) {
			FD_SET(t->key, &set);
			t = t->next;
		}

		tv.tv_sec = 1;
		tv.tv_usec = 0;

		/*
		 * Wait here for data (connection request) on any of the listening
		 * sockets. When ready, establish the connection. For the main
		 * port, a new proxy_thread() thread is spawned to service the HTTP
		 * request. For tunneled ports, tunnel_thread() thread is created
		 * and for SOCKS port, socks5_thread() is created.
		 *
		 * All threads are defined in forward.c, except for local proxy_thread()
		 * which routes the request as forwarded or direct, depending on the
		 * URL host name and NoProxy settings.
		 */
		cd = select(FD_SETSIZE, &set, NULL, NULL, &tv);
		if (cd > 0) {
			for (i = 0; i < FD_SETSIZE; ++i) {
				if (!FD_ISSET(i, &set))
					continue;

				clen = sizeof(caddr);
				cd = accept(i, (struct sockaddr *)&caddr, (socklen_t *)&clen);

				if (cd < 0) {
					syslog(LOG_ERR, "Serious error during accept: %s\n", strerror(errno));
					continue;
				}

				pthread_attr_init(&pattr);
				pthread_attr_setstacksize(&pattr, MAX(STACK_SIZE, PTHREAD_STACK_MIN));
#ifndef __CYGWIN__
				pthread_attr_setguardsize(&pattr, 256);
#endif

				if (plist_in(proxyd_list, i)) {
					data = (struct thread_arg_s *)zmalloc(sizeof(struct thread_arg_s));
					data->fd = cd;
					data->addr = caddr;
					if (!serialize)
						tid = pthread_create(&pthr, &pattr, proxy_thread, (void *)data);
					else
						proxy_thread((void *)data);
				} else if (plist_in(socksd_list, i)) {
					data = (struct thread_arg_s *)zmalloc(sizeof(struct thread_arg_s));
					data->fd = cd;
					data->addr = caddr;
					tid = pthread_create(&pthr, &pattr, socks5_thread, (void *)data);
				} else {
					data = (struct thread_arg_s *)zmalloc(sizeof(struct thread_arg_s));
					data->fd = cd;
					data->addr = caddr;
					data->target = plist_get(tunneld_list, i);
					tid = pthread_create(&pthr, &pattr, tunnel_thread, (void *)data);
				}

				pthread_attr_destroy(&pattr);

				if (tid)
					syslog(LOG_ERR, "Serious error during pthread_create: %d\n", tid);
				else
					tc++;
			}
		} else if (cd < 0 && !quit)
			syslog(LOG_ERR, "Serious error during select: %s\n", strerror(errno));

		if (threads_list) {
			pthread_mutex_lock(&threads_mtx);
			t = threads_list;
			while (t) {
				plist_t tmp_next = t->next;
				tid = pthread_join((pthread_t)t->key, (void *)&i);

				if (!tid) {
					tj++;
					if (debug)
						printf("Joined thread %lu; rc: %d\n", t->key, i);
				} else
					syslog(LOG_ERR, "Serious error during pthread_join: %d\n", tid);

				free(t);
				t = tmp_next;
			}
			threads_list = NULL;
			pthread_mutex_unlock(&threads_mtx);
		}
	}

bailout:
	if (strlen(cpidfile))
		unlink(cpidfile);

	syslog(LOG_INFO, "Terminating with %u active threads\n", tc - tj);
	pthread_mutex_lock(&connection_mtx);
	plist_free(connection_list);
	pthread_mutex_unlock(&connection_mtx);

	hlist_free(header_list);
	plist_free(scanner_agent_list);
	plist_free(noproxy_list);
	plist_free(tunneld_list);
	plist_free(proxyd_list);
	plist_free(socksd_list);
	plist_free(rules);

#ifdef ENABLE_PACPARSER
	free(pac_file);
#endif

	free(cuid);
	free(cpidfile);
	free(magic_detect);
	free(g_creds);

	parentlist_free(parent_list);

	exit(0);
}
