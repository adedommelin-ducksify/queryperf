/***
 ***	DNS Query Performance Testing Tool  (queryperf.c)
 ***
 ***	Version $Id: queryperf.c,v 1.1 2001/07/12 02:02:09 gson Exp $
 ***
 ***	Copyright (C) 2000, 2001  Nominum, Inc.  All Rights Reserved.
 ***
 ***	Stephen Jacob <sj@nominum.com>
 ***/

#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <math.h>
#include <errno.h>

/*
 * Configuration defaults
 */

#define DEF_MAX_QUERIES_OUTSTANDING	20
#define DEF_QUERY_TIMEOUT		5		/* in seconds */
#define DEF_SERVER_TO_QUERY		"localhost"
#define DEF_SERVER_PORT			53
#define DEF_BUFFER_SIZE			32		/* in k */

/*
 * Other constants / definitions
 */

#define COMMENT_CHAR			';'
#define CONFIG_CHAR			'#'
#define MAX_PORT			65535
#define MAX_INPUT_LEN			512
#define MAX_DOMAIN_LEN			255
#define MAX_BUFFER_LEN			8192		/* in bytes */
#define HARD_TIMEOUT_EXTRA		5		/* in seconds */
#define RESPONSE_BLOCKING_WAIT_TIME	0.1		/* in seconds */

#define FALSE				0
#define TRUE				1

#define WHITESPACE			" \t\n"

enum directives_enum	{ V_SERVER, V_PORT, V_MAXQUERIES, V_MAXWAIT };
#define DIRECTIVES	{ "server", "port", "maxqueries", "maxwait" }
#define DIR_VALUES	{ V_SERVER, V_PORT, V_MAXQUERIES, V_MAXWAIT }

#define QTYPE_STRINGS { \
	"A", "NS", "MD", "MF", "CNAME", "SOA", "MB", "MG", \
	"MR", "NULL", "WKS", "PTR", "HINFO", "MINFO", "MX", "TXT", \
	"AAAA", "AXFR", "MAILB", "MAILA", "*", "ANY" \
}

#define QTYPE_CODES { \
	1, 2, 3, 4, 5, 6, 7, 8, \
	9, 10, 11, 12, 13, 14, 15, 16, \
	28, 252, 253, 254, 255, 255 \
}

/*
 * Data type definitions
 */

#define QUERY_STATUS_MAGIC	0x51535441U	/* QSTA */
#define VALID_QUERY_STATUS(q)	((q) != NULL && \
				 (q)->magic == QUERY_STATUS_MAGIC)

struct query_status {
	unsigned int magic;
	int in_use;
	unsigned short int id;
	struct timeval sent_timestamp;
};

/*
 * Configuration options (global)
 */

unsigned int max_queries_outstanding;			/* init 0 */
unsigned int query_timeout = DEF_QUERY_TIMEOUT;
int ignore_config_changes = FALSE;
unsigned int socket_bufsize = DEF_BUFFER_SIZE;

int use_stdin = TRUE;
char *datafile_name;					/* init NULL */

char *server_to_query;					/* init NULL */
unsigned int server_port = DEF_SERVER_PORT;

int run_only_once = FALSE;
int use_timelimit = FALSE;
unsigned int run_timelimit;				/* init 0 */

int serverset = FALSE, portset = FALSE;
int queriesset = FALSE, timeoutset = FALSE;

/*
 * Other global stuff
 */

int setup_phase = TRUE;

FILE *datafile_ptr;					/* init NULL */
unsigned int runs_through_file;				/* init 0 */

unsigned int num_queries_sent;				/* init 0 */
unsigned int num_queries_outstanding;			/* init 0 */
unsigned int num_queries_timed_out;			/* init 0 */

struct timeval time_of_program_start;
struct timeval time_of_first_query;
struct timeval time_of_end_of_run;

struct query_status *status;				/* init NULL */
unsigned int query_status_allocated;			/* init 0 */

int query_socket;					/* init 0 */
struct sockaddr_in qaddr;

/*
 * get_uint16:
 *   Get an unsigned short integer from a buffer (in network order)
 */
static unsigned short
get_uint16(unsigned char *buf) {
	unsigned short ret;

	ret = buf[0] * 256 + buf[1];

	return (ret);
}

/*
 * show_startup_info:
 *   Show name/version
 */
void
show_startup_info(void) {
	printf("\n"
"DNS Query Performance Testing Tool\n"
"Version: $Id: queryperf.c,v 1.1 2001/07/12 02:02:09 gson Exp $\n"
"\n");
}

/*
 * show_usage:
 *   Print out usage/syntax information
 */
void
show_usage(void) {
	fprintf(stderr,
"\n"
"Usage: queryperf [-d datafile] [-s server_addr] [-p port] [-q num_queries]\n"
"                 [-b bufsize] [-t timeout] [-n] [-l limit] [-1]\n"
"  -d specifies the input data file (default: stdin)\n"
"  -s sets the server to query (default: %s)\n"
"  -p sets the port on which to query the server (default: %u)\n"
"  -q specifies the maximum number of queries outstanding (default: %d)\n"
"  -t specifies the timeout for query completion in seconds (default: %d)\n"
"  -n causes configuration changes to be ignored\n"
"  -l specifies how a limit for how long to run tests in seconds (no default)\n"
"  -1 run through input only once (default: multiple iff limit given)\n"
"  -b set input/output buffer size in kilobytes (default: %d k)\n"
"\n",
	        DEF_SERVER_TO_QUERY, DEF_SERVER_PORT,
	        DEF_MAX_QUERIES_OUTSTANDING, DEF_QUERY_TIMEOUT,
		DEF_BUFFER_SIZE);
}

/*
 * set_datafile:
 *   Set the datafile to read
 *
 *   Return -1 on failure
 *   Return a non-negative integer otherwise
 */
int
set_datafile(char *new_file) {
	char *dfname_tmp;

	if ((new_file == NULL) || (new_file[0] == '\0')) {
		fprintf(stderr, "Error: null datafile name\n");
		return (-1);
	}

	if ((dfname_tmp = malloc(strlen(new_file) + 1)) == NULL) {
		fprintf(stderr, "Error allocating memory for datafile name: "
		        "%s\n", new_file);
		return (-1);
	}

	free(datafile_name);
	datafile_name = dfname_tmp;

	strcpy(datafile_name, new_file);
	use_stdin = FALSE;

	return (0);
}

/*
 * set_input_stdin:
 *   Set the input to be stdin (instead of a datafile)
 */
void
set_input_stdin(void) {
	use_stdin = TRUE;
	free(datafile_name);
	datafile_name = NULL;
}

/*
 * set_server:
 *   Set the server to be queried
 *
 *   Return -1 on failure
 *   Return a non-negative integer otherwise
 */
int
set_server(char *new_name) {
	static struct hostent *server_he;

	/* If no change in server name, don't do anything... */
	if ((server_to_query != NULL) && (new_name != NULL))
		if (strcmp(new_name, server_to_query) == 0)
			return (0);

	if ((new_name == NULL) || (new_name[0] == '\0')) {
		fprintf(stderr, "Error: null server name\n");
		return (-1);
	}

	free(server_to_query);
	server_to_query = NULL;

	if ((server_to_query = malloc(strlen(new_name) + 1)) == NULL) {
		fprintf(stderr, "Error allocating memory for server name: "
		        "%s\n", new_name);
		return (-1);
	}

	if ((server_he = gethostbyname(new_name)) == NULL) {
		fprintf(stderr, "Error: gethostbyname(\"%s\") failed\n",
		        new_name);
		return (-1);
	}

	strcpy(server_to_query, new_name);
	qaddr.sin_addr = *((struct in_addr *)server_he->h_addr);

	return (0);
}

/*
 * set_server_port:
 *   Set the port on which to contact the server
 *
 *   Return -1 if port is invalid
 *   Return a non-negative integer otherwise
 */
int
set_server_port(unsigned int new_port) {
	if (new_port > MAX_PORT)
		return (-1);
	else {
		server_port = new_port;
		qaddr.sin_port = htons(server_port);
		return (0);
	}
}

/*
 * is_digit:
 *   Tests if a character is a digit
 *
 *   Return TRUE if it is
 *   Return FALSE if it is not
 */
int
is_digit(char d) {
	if (d < '0' || d > '9')
		return (FALSE);
	else
		return (TRUE);
}

/*
 * is_uint:
 *   Tests if a string, test_int, is a valid unsigned integer
 *
 *   Sets *result to be the unsigned integer if it is valid
 *
 *   Return TRUE if it is
 *   Return FALSE if it is not
 */
int
is_uint(char *test_int, unsigned int *result) {
	unsigned long int value;
	char *end;

	if (test_int == NULL)
		return (FALSE);

	if (is_digit(test_int[0]) == FALSE)
		return (FALSE);

	value = strtoul(test_int, &end, 10);

	if ((errno == ERANGE) || (*end != '\0') || (value > UINT_MAX))
		return (FALSE);

	*result = (unsigned int)value;
	return (TRUE);
}

/*
 * set_max_queries:
 *   Set the maximum number of outstanding queries
 *
 *   Returns -1 on failure
 *   Returns a non-negative integer otherwise
 */
int
set_max_queries(unsigned int new_max) {
	static unsigned int size_qs = sizeof(struct query_status);
	struct query_status *temp_stat;
	unsigned int count;

	if (new_max < 0) {
		fprintf(stderr, "Unable to change max outstanding queries: "
		        "must be positive and non-zero: %u\n", new_max);
		return (-1);
	}

	if (new_max > query_status_allocated) {
		temp_stat = realloc(status, new_max * size_qs);

		if (temp_stat == NULL) {
			fprintf(stderr, "Error resizing query_status\n");
			return (-1);
		} else {
			status = temp_stat;

			/*
			 * Be careful to only initialise between above
			 * the previously allocated space. Note that the
			 * allocation may be larger than the current
			 * max_queries_outstanding. We don't want to
			 * "forget" any outstanding queries! We might
			 * still have some above the bounds of the max.
			 */
			count = query_status_allocated;
			for (; count < new_max; count++) {
				status[count].in_use = FALSE;
				status[count].magic = QUERY_STATUS_MAGIC;
			}

			query_status_allocated = new_max;
		}
	}

	max_queries_outstanding = new_max;

	return (0);
}

/*
 * parse_args:
 *   Parse program arguments and set configuration options
 *
 *   Return -1 on failure
 *   Return a non-negative integer otherwise
 */
int
parse_args(int argc, char **argv) {
	int c;
	unsigned int uint_arg_val;

	while ((c = getopt(argc, argv, "q:t:nd:s:p:1l:b:")) != -1) {
		switch (c) {
		case 'q':
			if (is_uint(optarg, &uint_arg_val) == TRUE) {
				set_max_queries(uint_arg_val);
				queriesset = TRUE;
			} else {
				fprintf(stderr, "Option requires a positive "
				        "integer value: -%c %s\n",
					c, optarg);
				return (-1);
			}
			break;

		case 't':
			if (is_uint(optarg, &uint_arg_val) == TRUE) {
				query_timeout = uint_arg_val;
				timeoutset = TRUE;
			} else {
				fprintf(stderr, "Option requires a positive "
				        "integer value: -%c %s\n",
				        c, optarg);
				return (-1);
			}
			break;

		case 'n':
			ignore_config_changes = TRUE;
			break;

		case 'd':
			if (set_datafile(optarg) == -1) {
				fprintf(stderr, "Error setting datafile "
					"name: %s\n", optarg);
				return (-1);
			}
			break;

		case 's':
			if (set_server(optarg) == -1) {
				fprintf(stderr, "Error setting server "
					"name: %s\n", optarg);
				return (-1);
			}
			serverset = TRUE;
			break;
			
		case 'p':
			if (is_uint(optarg, &uint_arg_val) == TRUE &&
			    uint_arg_val < MAX_PORT)
			{
				set_server_port(uint_arg_val);
				portset = TRUE;
			} else {
				fprintf(stderr, "Option requires a positive "
				        "integer between 0 and %d: -%c %s\n",
					MAX_PORT - 1, c, optarg);
				return (-1);
			}
			break;

		case '1':
			run_only_once = TRUE;
			break;

		case 'l':
			if (is_uint(optarg, &uint_arg_val) == TRUE) {
				use_timelimit = TRUE;
				run_timelimit = uint_arg_val;
			} else {
				fprintf(stderr, "Option requires a positive "
				        "integer: -%c %s\n",
					c, optarg);
				return (-1);
			}
			break;

		case 'b':
			if (is_uint(optarg, &uint_arg_val) == TRUE) {
				socket_bufsize = uint_arg_val;
			} else {
				fprintf(stderr, "Option requires a positive "
					"integer: -%c %s\n",
					c, optarg);
				return (-1);
			}
			break;

		default:
			fprintf(stderr, "Invalid option: -%c\n", optopt);
			return (-1);
		}
	}

	if (run_only_once == FALSE && use_timelimit == FALSE)
		run_only_once = TRUE;

	return (0);
}

/*
 * open_datafile:
 *   Open the data file ready for reading
 *
 *   Return -1 on failure
 *   Return non-negative integer on success
 */
int
open_datafile(void) {
	if (use_stdin == TRUE) {
		datafile_ptr = stdin;
		return (0);
	} else {
		if ((datafile_ptr = fopen(datafile_name, "r")) == NULL) {
			fprintf(stderr, "Error: unable to open datafile: %s\n",
			        datafile_name);
			return (-1);
		} else
			return (0);
	}
}

/*
 * close_datafile:
 *   Close the data file if any is open
 *
 *   Return -1 on failure
 *   Return non-negative integer on success, including if not needed
 */
int
close_datafile(void) {
	if ((use_stdin == FALSE) && (datafile_ptr != NULL)) {
		if (fclose(datafile_ptr) != 0) {
			fprintf(stderr, "Error: unable to close datafile\n");
			return (-1);
		}
	}

	return (0);
}

/*
 * open_socket:
 *   Open a socket for the queries
 *
 *   Return -1 on failure
 *   Return a non-negative integer otherwise
 */
int
open_socket(void) {
	int sock;
	struct protoent *proto;
	struct sockaddr_in bind_addr;
	int ret;
	int bufsize;

	bind_addr.sin_family = AF_INET;
	bind_addr.sin_port = htons(0); /* Have bind allocate a random port */
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bzero(&(bind_addr.sin_zero), 8);

	if ((proto = getprotobyname("udp")) == NULL) {
		fprintf(stderr, "Error: getprotobyname call failed");
		return (-1);
	}

	if ((sock = socket(PF_INET, SOCK_DGRAM, proto->p_proto)) == -1) {
		fprintf(stderr, "Error: socket call failed");
		return (-1);
	}

	if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(struct sockaddr))
	    == -1) {
		fprintf(stderr, "Error: bind call failed");
		return (-1);
	}

	bufsize = 1024 * socket_bufsize;

	ret = setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
			 &bufsize, sizeof(bufsize));
	if (ret < 0)
		fprintf(stderr, "Warning:  setsockbuf(SO_RCVBUF) failed\n");

	ret = setsockopt(sock, SOL_SOCKET, SO_SNDBUF,
			 &bufsize, sizeof(bufsize));
	if (ret < 0)
		fprintf(stderr, "Warning:  setsockbuf(SO_SNDBUF) failed\n");

	query_socket = sock;
	
	return (0);
}

/*
 * close_socket:
 *   Close the query socket
 *
 *   Return -1 on failure
 *   Return a non-negative integer otherwise
 */
int
close_socket(void) {
	if (query_socket != 0) {
		if (close(query_socket) != 0) {
			fprintf(stderr, "Error: unable to close socket\n");
			return (-1);
		}
	}

	query_socket = 0;

	return (0);
}

/*
 * setup:
 *   Set configuration options from command line arguments
 *   Open datafile ready for reading
 *
 *   Return -1 on failure
 *   Return non-negative integer on success
 */
int
setup(int argc, char **argv) {
	qaddr.sin_family = AF_INET;
	qaddr.sin_port = htons(0);
	qaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	bzero(&(qaddr.sin_zero), 8);

	set_input_stdin();

	if (set_max_queries(DEF_MAX_QUERIES_OUTSTANDING) == -1) {
		fprintf(stderr, "%s: Unable to set default max outstanding "
		        "queries\n", argv[0]);
		return (-1);
	}

	if (set_server(DEF_SERVER_TO_QUERY) == -1) {
		fprintf(stderr, "%s: Error setting default server name\n",
		        argv[0]);
		return (-1);
	}

	if (set_server_port(DEF_SERVER_PORT) == -1) {
		fprintf(stderr, "%s: Error setting default server port\n",
		        argv[0]);
		return (-1);
	}

	if (parse_args(argc, argv) == -1) {
		show_usage();
		return (-1);
	}

	if (open_datafile() == -1)
		return (-1);

	if (open_socket() == -1)
		return (-1);

	return (0);
}

/*
 * set_timenow:
 *   Set a timeval struct to indicate the current time
 */
void
set_timenow(struct timeval *tv) {
	if (gettimeofday(tv, NULL) == -1) {
		fprintf(stderr, "Error in gettimeofday(). Using inaccurate "
		        "time() instead\n");
		tv->tv_sec = time(NULL);
		tv->tv_usec = 0;
	}
}

/*
 * difftv:
 *   Find the difference in seconds between two timeval structs.
 *
 *   Return the difference between tv1 and tv2 in seconds in a double.
 */
double
difftv(struct timeval tv1, struct timeval tv2) {
	long diff_sec, diff_usec;
	double diff;

	diff_sec = tv1.tv_sec - tv2.tv_sec;
	diff_usec = tv1.tv_usec - tv2.tv_usec;

	diff = (double)diff_sec + ((double)diff_usec / 1000000.0);

	return diff;
}

/*
 * timelimit_reached:
 *   Have we reached the time limit (if any)?
 *
 *   Returns FALSE if there is no time limit or if we have not reached it
 *   Returns TRUE otherwise
 */
int
timelimit_reached(void) {
	struct timeval time_now;

	set_timenow(&time_now);

	if (use_timelimit == FALSE)
		return (FALSE);

	if (setup_phase == TRUE) {
		if (difftv(time_now, time_of_program_start)
		    < (double)(run_timelimit + HARD_TIMEOUT_EXTRA))
			return (FALSE);
		else
			return (TRUE);
	} else {
		if (difftv(time_now, time_of_first_query)
		    < (double)run_timelimit)
			return (FALSE);
		else
			return (TRUE);
	}
}

/*
 * keep_sending:
 *   Should we keep sending queries or stop here?
 *
 *   Return TRUE if we should keep on sending queries
 *   Return FALSE if we should stop
 *
 *   Side effects:
 *   Rewinds the input and clears reached_end_input if we have reached the
 *   end of the input, but we are meant to run through it multiple times
 *   and have not hit the time limit yet (if any is set).
 */
int
keep_sending(int *reached_end_input) {
	static int stop = FALSE;

	if (stop == TRUE)
		return (FALSE);

	if ((*reached_end_input == FALSE) && (timelimit_reached() == FALSE))
		return (TRUE);
	else if ((*reached_end_input == TRUE) && (run_only_once == FALSE)
	         && (timelimit_reached() == FALSE)) {
		rewind(datafile_ptr);
		*reached_end_input = FALSE;
		runs_through_file++;
		return (TRUE);
	} else {
		if (*reached_end_input == TRUE)
			runs_through_file++;
		stop = TRUE;
		return (FALSE);
	}
}

/*
 * queries_outstanding:
 *   How many queries are outstanding?
 *
 *   Returns the number of outstanding queries
 */
unsigned int
queries_outstanding(void) {
	return (num_queries_outstanding);
}

/*
 * next_input_line:
 *   Get the next non-comment line from the input file
 *
 *   Put text in line, up to max of n chars. Skip comment lines.
 *   Skip empty lines.
 *
 *   Return line length on success
 *   Return 0 if cannot read a non-comment line (EOF or error)
 */
int
next_input_line(char *line, int n) {
	char *result;

	do {
		result = fgets(line, n, datafile_ptr);
	} while ((result != NULL) &&
	         ((line[0] == COMMENT_CHAR) || (line[0] == '\n')));

	if (result == NULL)
		return (0);
	else
		return (strlen(line));
}

/*
 * identify_directive:
 *   Gives us a numerical value equivelant for a directive string
 *
 *   Returns the value for the directive
 *   Returns -1 if not a valid directive
 */
int
identify_directive(char *dir) {
	static char *directives[] = DIRECTIVES;
	static int dir_values[] = DIR_VALUES;
	unsigned int index, num_directives;

	num_directives = sizeof(directives) / sizeof(directives[0]);

	if (num_directives > (sizeof(dir_values) / sizeof(int)))
		num_directives = sizeof(dir_values) / sizeof(int);

	for (index = 0; index < num_directives; index++) {
		if (strcmp(dir, directives[index]) == 0)
			return (dir_values[index]);
	}

	return (-1);
}

/*
 * update_config:
 *   Update configuration options from a line from the input file
 */
void
update_config(char *config_change_desc) {
	char *directive, *config_value, *trailing_garbage;
	char conf_copy[MAX_INPUT_LEN + 1];
	unsigned int uint_val;
	int directive_number;
	int check;

	if (ignore_config_changes == TRUE) {
		fprintf(stderr, "Ignoring configuration change: %s",
		        config_change_desc);
		return;
	}

	strcpy(conf_copy, config_change_desc);

	++config_change_desc;

	if (*config_change_desc == '\n' || *config_change_desc == '\0') {
		fprintf(stderr, "Invalid config: No directive present: %s",
		        conf_copy);
		return;
	}

	if (index(WHITESPACE, *config_change_desc) != NULL) {
		fprintf(stderr, "Invalid config: Space before directive or "
		        "no directive present: %s", conf_copy);
		return;
	}

	directive = strtok(config_change_desc, WHITESPACE);
	config_value = strtok(NULL, WHITESPACE);
	trailing_garbage = strtok(NULL, WHITESPACE);

	if ((directive_number = identify_directive(directive)) == -1) {
		fprintf(stderr, "Invalid config: Bad directive: %s",
		        conf_copy);
		return;
	}

	if (config_value == NULL) {
		fprintf(stderr, "Invalid config: No value present: %s",
		        conf_copy);
		return;
	}

	if (trailing_garbage != NULL) {
		fprintf(stderr, "Config warning: "
		        "trailing garbage: %s", conf_copy);
	}

	switch(directive_number) {

	case V_SERVER:
		if (serverset && (setup_phase == TRUE)) {
			fprintf(stderr, "Config change overriden by command "
			        "line: %s\n", directive);
			return;
		}

		if (set_server(config_value) == -1)
			fprintf(stderr, "Set server error: unable to change "
			        "the server name to '%s'\n", config_value);
		break;

	case V_PORT:
		if (portset && (setup_phase == TRUE)) {
			fprintf(stderr, "Config change overriden by command "
			        "line: %s\n", directive);
			return;
		}

		check = is_uint(config_value, &uint_val);

		if ((check == TRUE) && (uint_val > 0)) {
			if (set_server_port(uint_val) == -1) {
				fprintf(stderr, "Invalid config: Bad value for"
				        " %s: %s\n", directive, config_value);
			}
		} else
			fprintf(stderr, "Invalid config: Bad value for "
			        "%s: %s\n", directive, config_value);
		break;

	case V_MAXQUERIES:
		if (queriesset && (setup_phase == TRUE)) {
			fprintf(stderr, "Config change overriden by command "
			        "line: %s\n", directive);
			return;
		}

		check = is_uint(config_value, &uint_val);

		if ((check == TRUE) && (uint_val > 0)) {
			set_max_queries(uint_val);
		} else
			fprintf(stderr, "Invalid config: Bad value for "
			        "%s: %s\n", directive, config_value);
		break;

	case V_MAXWAIT:
		if (timeoutset && (setup_phase == TRUE)) {
			fprintf(stderr, "Config change overriden by command "
			        "line: %s\n", directive);
			return;
		}

		check = is_uint(config_value, &uint_val);

		if ((check == TRUE) && (uint_val > 0)) {
			query_timeout = uint_val;
		} else
			fprintf(stderr, "Invalid config: Bad value for "
			        "%s: %s\n", directive, config_value);
		break;

	default:
		fprintf(stderr, "Invalid config: Bad directive: %s\n",
		        directive);
		break;
	}
}

/*
 * parse_query:
 *   Parse a query line from the input file
 *
 *   Set qname to be the domain to query (up to a max of qnlen chars)
 *   Set qtype to be the type of the query
 *
 *   Return -1 on failure
 *   Return a non-negative integer otherwise
 */
int
parse_query(char *input, char *qname, int qnlen, int *qtype) {
	static char *qtype_strings[] = QTYPE_STRINGS;
	static int qtype_codes[] = QTYPE_CODES;
	int num_types, index;
	int found = FALSE;
	char incopy[MAX_INPUT_LEN + 1];
	char *domain_str, *type_str;

	num_types = sizeof(qtype_strings) / sizeof(qtype_strings[0]);
	if (num_types > (sizeof(qtype_codes) / sizeof(int)))
		num_types = sizeof(qtype_codes) / sizeof(int);

	strcpy(incopy, input);

	domain_str = strtok(incopy, WHITESPACE);
	type_str = strtok(NULL, WHITESPACE);

	if ((domain_str == NULL) || (type_str == NULL)) {
		fprintf(stderr, "Invalid query input format: %s", input);
		return (-1);
	}

	if (strlen(domain_str) > qnlen) {
		fprintf(stderr, "Query domain too long: %s\n", domain_str);
		return (-1);
	}

	for (index = 0; (index < num_types) && (found == FALSE); index++) {
		if (strcmp(type_str, qtype_strings[index]) == 0) {
			*qtype = qtype_codes[index];
			found = TRUE;
		}
	}

	if (found == FALSE) {
		fprintf(stderr, "Query type not understood: %s\n", type_str);
		return (-1);
	}

	strcpy(qname, domain_str);

	return (0);
}

/*
 * dispatch_query:
 *   Send the query packet for the entry 
 *
 *   Return -1 on failure
 *   Return a non-negative integer otherwise
 */
int
dispatch_query(unsigned short int id, char *dom, int qt) {
	static u_char packet_buffer[PACKETSZ + 1];
	static socklen_t sockaddrlen = sizeof(struct sockaddr);
	int buffer_len = PACKETSZ;
	int bytes_sent;
	unsigned short int net_id = htons(id);
	char *id_ptr = (char *)&net_id;

	buffer_len = res_mkquery(QUERY, dom, C_IN, qt, NULL, 0,
				 NULL, packet_buffer, PACKETSZ);
	if (buffer_len == -1) {
		fprintf(stderr, "Failed to create query packet: %s %d\n",
		        dom, qt);
		return (-1);
	}

	packet_buffer[0] = id_ptr[0];
	packet_buffer[1] = id_ptr[1];

	bytes_sent = sendto(query_socket, packet_buffer, buffer_len, 0,
			    (struct sockaddr *)&qaddr, sockaddrlen);
	if (bytes_sent == -1) {
		fprintf(stderr, "Failed to send query packet: %s %d\n",
		        dom, qt);
		return (-1);
	}

	if (bytes_sent != buffer_len)
		fprintf(stderr, "Warning: incomplete packet sent: %s %d\n",
		        dom, qt);

	return (0);
}

/*
 * send_query:
 *   Send a query based on a line of input
 */
void
send_query(char *query_desc) {
	static unsigned short int use_query_id = 0;
	static int qname_len = MAX_DOMAIN_LEN;
	static char domain[MAX_DOMAIN_LEN + 1];
	int query_type;
	unsigned int count;

	use_query_id++;

	if (parse_query(query_desc, domain, qname_len, &query_type) == -1) {
		fprintf(stderr, "Error parsing query: %s", query_desc);
		return;
	}

	if (dispatch_query(use_query_id, domain, query_type) == -1) {
		fprintf(stderr, "Error sending query: %s", query_desc);
		return;
	}

	if (setup_phase == TRUE) {
		set_timenow(&time_of_first_query);
		setup_phase = FALSE;
		printf("[Status] Sending queries\n");
	}

	/* Find the first slot in status[] that is not in use */
	for(count = 0; (status[count].in_use == TRUE)
	    && (count < max_queries_outstanding); count++);

	if (status[count].in_use == TRUE) {
		fprintf(stderr, "Unexpected error: We have run out of "
		        "status[] space!\n");
		return;
	}

	/* Register the query in status[] */
	status[count].in_use = TRUE;
	status[count].id = use_query_id;
	set_timenow(&status[count].sent_timestamp);

	num_queries_sent++;
	num_queries_outstanding++;
}

/*
 * data_available:
 *   Is there data available on the given file descriptor?
 *
 *   Return TRUE if there is
 *   Return FALSE otherwise
 */
int
data_available(int fd, double wait) {
	fd_set read_fds;
	struct timeval tv;
	int retval;

	/* Set list of file descriptors */
	FD_ZERO(&read_fds);
	FD_SET(fd, &read_fds);

	if ((wait > 0.0) && (wait < (double)LONG_MAX)) {
		tv.tv_sec = (long)floor(wait);
		tv.tv_usec = (long)(1000000.0 * (wait - floor(wait)));
	} else {
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	}

	retval = select(fd + 1, &read_fds, NULL, NULL, &tv);

	if (FD_ISSET(fd, &read_fds))
		return (TRUE);
	else
		return (FALSE);
}

/*
 * register_response:
 *   Register receipt of a query
 *
 *   Removes (sets in_use = FALSE) the record for the given query id in
 *   status[] if any exists.
 */
void
register_response(unsigned short int id) {
	unsigned int ct = 0;
	int found = FALSE;

	for(; (ct < query_status_allocated) && (found == FALSE); ct++) {
		if ((status[ct].in_use == TRUE) && (status[ct].id == id)) {
			status[ct].in_use = FALSE;
			num_queries_outstanding--;
			found = TRUE;
		}
	}

	if (found == FALSE)
		fprintf(stderr, "Warning: Received a response with an "
		        "unexpected (maybe timed out) id: %u\n", id);
}

/*
 * process_single_response:
 *   Receive from the given socket & process an invididual response packet.
 *   Remove it from the list of open queries (status[]) and decrement the
 *   number of outstanding queries if it matches an open query.
 */
void
process_single_response(int sockfd) {
	static struct sockaddr_in from_addr;
	static unsigned char in_buf[MAX_BUFFER_LEN];
	int numbytes, addr_len, resp_id;

	addr_len = sizeof(struct sockaddr);

	if ((numbytes = recvfrom(sockfd, in_buf, MAX_BUFFER_LEN,
	     0, (struct sockaddr *)&from_addr, &addr_len)) == -1) {
		fprintf(stderr, "Error receiving datagram\n");
		return;
	}

	resp_id = get_uint16(in_buf);

	register_response(resp_id);
}

/*
 * process_responses:
 *   Go through any/all received responses and remove them from the list of
 *   open queries (set in_use = FALSE for their entry in status[]), also
 *   decrementing the number of outstanding queries.
 */
void
process_responses(void) {
	double first_packet_wait = RESPONSE_BLOCKING_WAIT_TIME;
	unsigned int outstanding = queries_outstanding();

	/*
	 * Don't block waiting for packets at all if we aren't looking for
	 * any responses or if we are now able to send new queries.
	 */
	if ((outstanding == 0) || (outstanding < max_queries_outstanding)) {
		first_packet_wait = 0.0;
	}

	if (data_available(query_socket, first_packet_wait) == TRUE) {
		process_single_response(query_socket);

		while (data_available(query_socket, 0.0) == TRUE)
			process_single_response(query_socket);
	}
}

/*
 * retire_old_queries:
 *   Go through the list of open queries (status[]) and remove any queries
 *   (i.e. set in_use = FALSE) which are older than the timeout, decrementing
 *   the number of queries outstanding for each one removed.
 */
void
retire_old_queries(void) {
	unsigned int count = 0;
	struct timeval curr_time;

	set_timenow(&curr_time);

	for(; count < query_status_allocated; count++) {

		if ((status[count].in_use == TRUE)
		    && (difftv(curr_time, status[count].sent_timestamp)
		    >= (double)query_timeout)) {

			status[count].in_use = FALSE;
			num_queries_outstanding--;
			num_queries_timed_out++;
			printf("[Timeout] Query timed out: msg id %u\n",
			       status[count].id);
		}
	}
}

/*
 * print_statistics:
 *   Print out statistics based on the results of the test
 */
void
print_statistics(void) {
	unsigned int num_queries_completed;
	double per_lost, per_completed;
	double run_time, queries_per_sec;
	struct timeval start_time;

	num_queries_completed = num_queries_sent - num_queries_timed_out;

	if (num_queries_completed == 0) {
		per_lost = 0.0;
		per_completed = 0.0;
	} else {
		per_lost = 100.0 * (double)num_queries_timed_out
		           / (double)num_queries_sent;
		per_completed = 100.0 - per_lost;
	}

	if (num_queries_sent == 0) {
		start_time.tv_sec = time_of_program_start.tv_sec;
		start_time.tv_usec = time_of_program_start.tv_usec;
		run_time = 0.0;
		queries_per_sec = 0.0;
	} else {
		start_time.tv_sec = time_of_first_query.tv_sec;
		start_time.tv_usec = time_of_first_query.tv_usec;
		run_time = difftv(time_of_end_of_run, time_of_first_query);
		queries_per_sec = (double)num_queries_completed / run_time;
	}

	printf("\n");

	printf("Statistics:\n");

	printf("\n");

	printf("  Parse input file:     %s\n",
	       ((run_only_once == TRUE) ? "once" : "multiple times"));
	if (use_timelimit)
		printf("  Run time limit:       %u seconds\n", run_timelimit);
	if (run_only_once == FALSE)
		printf("  Ran through file:     %u times\n",
		       runs_through_file);
	else
		printf("  Ended due to:         reaching %s\n",
		       ((runs_through_file == 0) ? "time limit"
		       : "end of file"));

	printf("\n");

	printf("  Queries sent:         %u queries\n", num_queries_sent);
	printf("  Queries completed:    %u queries\n", num_queries_completed);
	printf("  Queries lost:         %u queries\n", num_queries_timed_out);

	printf("\n");

	printf("  Percentage completed: %6.2lf%%\n", per_completed);
	printf("  Percentage lost:      %6.2lf%%\n", per_lost);

	printf("\n");

	printf("  Started at:           %s", ctime(&start_time.tv_sec));
	printf("  Finished at:          %s",
	       ctime(&time_of_end_of_run.tv_sec));
	printf("  Ran for:              %.6lf seconds\n", run_time);

	printf("\n");

	printf("  Queries per second:   %.6lf qps\n", queries_per_sec);

	printf("\n");
}

/*
 * dnsqtest Program Mainline
 */
int
main(int argc, char **argv) {
	int got_eof = FALSE;
	int input_length = MAX_INPUT_LEN;
	char input_line[MAX_INPUT_LEN + 1];

	set_timenow(&time_of_program_start);
	time_of_first_query.tv_sec = 0;
	time_of_first_query.tv_usec = 0;
	time_of_end_of_run.tv_sec = 0;
	time_of_end_of_run.tv_usec = 0;

	input_line[0] = '\0';

	show_startup_info();

	if (setup(argc, argv) == -1)
		return (-1);

	printf("[Status] Processing input data\n");

	while (keep_sending(&got_eof) == TRUE || queries_outstanding() > 0) {
		while (keep_sending(&got_eof) == TRUE
		       && queries_outstanding() < max_queries_outstanding) {

			if (next_input_line(input_line, input_length) == 0) {
				got_eof = TRUE;
			} else {
				/*
				 * TODO: Should test if we got a whole line
				 * and flush to the next \n in input if not
				 * here... Add this later. Only do the next
				 * few lines if we got a whole line, else
				 * print a warning. Alternative: Make the
				 * max line size really big. BAD! :)
				 */

				if (input_line[0] == CONFIG_CHAR)
					update_config(input_line);
				else {
					send_query(input_line);
				}
			}
		}

		retire_old_queries();
		process_responses();
	}

	set_timenow(&time_of_end_of_run);

	printf("[Status] Testing complete\n");

	close_socket();
	close_datafile();

	print_statistics();

	return (0);
}
