/*
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/types.h>
#include <regex.h>
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "strtab.h"
#include "misc.h"
#include "trace.h"
#include "tcp.h"
#include "http.h"
#include "cfg.h"
#include "htsmsg_json.h"
#include "talloc.h"

static void *http_server;

typedef struct http_path {
  LIST_ENTRY(http_path) hp_link;
  char *hp_path;
  void *hp_opaque;
  http_callback_t *hp_callback;
  int hp_len;
} http_path_t;


static LIST_HEAD(, http_path) http_paths;


typedef struct http_route {
  LIST_ENTRY(http_route) hr_link;
  int hr_flags;
  char *hr_path;
  regex_t hr_reg;
  int hr_depth;
  http_callback2_t *hr_callback;
} http_route_t;


static LIST_HEAD(, http_route) http_routes;

#define HTTP_ERROR_DISCONNECT -1

static struct strtab HTTP_cmdtab[] = {
  { "GET",        HTTP_CMD_GET },
  { "HEAD",       HTTP_CMD_HEAD },
  { "POST",       HTTP_CMD_POST },
  { "PUT",        HTTP_CMD_PUT },
  { "DELETE",     HTTP_CMD_DELETE },
  { "DESCRIBE",   RTSP_CMD_DESCRIBE },
  { "OPTIONS",    RTSP_CMD_OPTIONS },
  { "SETUP",      RTSP_CMD_SETUP },
  { "PLAY",       RTSP_CMD_PLAY },
  { "TEARDOWN",   RTSP_CMD_TEARDOWN },
  { "PAUSE",      RTSP_CMD_PAUSE },
};



static struct strtab HTTP_versiontab[] = {
  { "HTTP/1.0",        HTTP_VERSION_1_0 },
  { "HTTP/1.1",        HTTP_VERSION_1_1 },
  { "RTSP/1.0",        RTSP_VERSION_1_0 },
};

static void http_parse_query_args(http_connection_t *hc, char *args);


/**
 *
 */
static int
http_resolve_path(http_connection_t *hc)
{
  http_path_t *hp;
  char *v;
  const char *remain = NULL;

  LIST_FOREACH(hp, &http_paths, hp_link) {
    if(!strncmp(hc->hc_path, hp->hp_path, hp->hp_len)) {
      if(hc->hc_path[hp->hp_len] == 0 || hc->hc_path[hp->hp_len] == '/' ||
	 hc->hc_path[hp->hp_len] == '?')
	break;
    }
  }

  if(hp == NULL)
    return 404;

  v = hc->hc_path + hp->hp_len;


  switch(*v) {
  case 0:
    break;

  case '/':
    if(v[1])
      remain = v + 1;
    break;

  default:
    return 404;
  }


  return hp->hp_callback(hc, remain, hp->hp_opaque);
}


#define MAX_ROUTE_MATCHES 32

/**
 *
 */
static int
http_resolve_route(http_connection_t *hc, int cont)
{
  http_route_t *hr;
  regmatch_t match[MAX_ROUTE_MATCHES];
  char *argv[MAX_ROUTE_MATCHES];
  int argc;

  LIST_FOREACH(hr, &http_routes, hr_link)
    if(!regexec(&hr->hr_reg, hc->hc_path, 32, match, 0))
      break;

  if(hr == NULL)
    return 404;

  if(cont && !(hr->hr_flags & HTTP_ROUTE_HANDLE_100_CONTINUE))
    return 100;

  for(argc = 0; argc < MAX_ROUTE_MATCHES; argc++) {
    if(match[argc].rm_so == -1)
      break;
    int len = match[argc].rm_eo - match[argc].rm_so;
    char *s = argv[argc] = alloca(len + 1);
    s[len] = 0;
    memcpy(s, hc->hc_path + match[argc].rm_so, len);
  }

  return hr->hr_callback(hc, argc, argv,
                         cont ? HTTP_ROUTE_HANDLE_100_CONTINUE : 0);
}



/*
 * HTTP status code to string
 */

static const char *
http_rc2str(int code)
{
  switch(code) {
  case HTTP_STATUS_OK:              return "OK";
  case HTTP_STATUS_PARTIAL_CONTENT: return "Partial Content";
  case HTTP_STATUS_NOT_FOUND:       return "Not found";
  case HTTP_STATUS_UNAUTHORIZED:    return "Unauthorized";
  case HTTP_STATUS_BAD_REQUEST:     return "Bad request";
  case HTTP_STATUS_FOUND:           return "Found";
  case HTTP_STATUS_NOT_MODIFIED:    return "Not modified";
  case HTTP_STATUS_TEMPORARY_REDIRECT: return "Temporary redirect";
  case HTTP_STATUS_ISE: return "Internal Server Error";
  default:
    return "Unknown returncode";
    break;
  }
}

static const char *cachedays[7] = {
  "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *cachemonths[12] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};


/**
 *
 */
int
http_send_100_continue(http_connection_t *hc)
{
  htsbuf_queue_t q;
  htsbuf_queue_init(&q, 0);

  htsbuf_qprintf(&q, "%s 100 Continue\r\n\r\n",
		 val2str(hc->hc_version, HTTP_versiontab));
  return tcp_write_queue(hc->hc_ts, &q);
}



/**
 * Transmit a HTTP reply
 */
int
http_send_header(http_connection_t *hc, int rc, const char *content, 
		 int64_t contentlen,
		 const char *encoding, const char *location, 
		 int maxage, const char *range,
		 const char *disposition, const char *transfer_encoding)
{
  struct tm tm0, *tm;
  htsbuf_queue_t hdrs;
  time_t t;

  htsbuf_queue_init(&hdrs, 0);

  htsbuf_qprintf(&hdrs, "%s %d %s\r\n", 
		 val2str(hc->hc_version, HTTP_versiontab),
		 rc, http_rc2str(rc));

  htsbuf_qprintf(&hdrs, "Server: doozer2\r\n");

  if(maxage == 0) {
    htsbuf_qprintf(&hdrs, "Cache-Control: no-cache\r\n");
  } else {
    time(&t);

    tm = gmtime_r(&t, &tm0);
    htsbuf_qprintf(&hdrs, 
		"Last-Modified: %s, %02d %s %d %02d:%02d:%02d GMT\r\n",
		cachedays[tm->tm_wday],	tm->tm_year + 1900,
		cachemonths[tm->tm_mon], tm->tm_mday,
		tm->tm_hour, tm->tm_min, tm->tm_sec);

    t += maxage;

    tm = gmtime_r(&t, &tm0);
    htsbuf_qprintf(&hdrs, 
		"Expires: %s, %02d %s %d %02d:%02d:%02d GMT\r\n",
		cachedays[tm->tm_wday],	tm->tm_year + 1900,
		cachemonths[tm->tm_mon], tm->tm_mday,
		tm->tm_hour, tm->tm_min, tm->tm_sec);
      
    htsbuf_qprintf(&hdrs, "Cache-Control: max-age=%d\r\n", maxage);
  }

  if(rc == HTTP_STATUS_UNAUTHORIZED)
    htsbuf_qprintf(&hdrs, "WWW-Authenticate: Basic realm=\"doozer\"\r\n");

  if(contentlen > 0)
    htsbuf_qprintf(&hdrs, "Content-Length: %"PRId64"\r\n", contentlen);
  else
    hc->hc_keep_alive = 0;

  htsbuf_qprintf(&hdrs, "Connection: %s\r\n", 
	      hc->hc_keep_alive ? "Keep-Alive" : "Close");

  if(encoding != NULL)
    htsbuf_qprintf(&hdrs, "Content-Encoding: %s\r\n", encoding);

  if(transfer_encoding != NULL)
    htsbuf_qprintf(&hdrs, "Transfer-Encoding: %s\r\n", transfer_encoding);

  if(location != NULL)
    htsbuf_qprintf(&hdrs, "Location: %s\r\n", location);

  if(content != NULL)
    htsbuf_qprintf(&hdrs, "Content-Type: %s\r\n", content);


  if(range) {
    htsbuf_qprintf(&hdrs, "Accept-Ranges: %s\r\n", "bytes");
    htsbuf_qprintf(&hdrs, "Content-Range: %s\r\n", range);
  }

  if(disposition != NULL)
    htsbuf_qprintf(&hdrs, "Content-Disposition: %s\r\n", disposition);

  http_arg_t *ra;
  TAILQ_FOREACH(ra, &hc->hc_response_headers, link)
    htsbuf_qprintf(&hdrs, "%s: %s\r\n", ra->key, ra->val);

  htsbuf_qprintf(&hdrs, "\r\n");
  //  fprintf(stderr, "-- OUTPUT ------------------\n");
  //  htsbuf_dump_raw_stderr(&hdrs);
  //  fprintf(stderr, "----------------------------\n");

  return tcp_write_queue(hc->hc_ts, &hdrs);
}



/**
 * Transmit a HTTP reply
 */
static int
http_send_reply(http_connection_t *hc, int rc, const char *content, 
		const char *encoding, const char *location, int maxage)
{
  if(http_send_header(hc, rc, content, hc->hc_reply.hq_size,
                      encoding, location, maxage, 0, NULL, NULL))
    return -1;

  if(hc->hc_no_output)
    return 0;

  return tcp_write_queue(hc->hc_ts, &hc->hc_reply);
}


/**
 * Send HTTP error back
 */
int
http_err(http_connection_t *hc, int error, const char *str)
{
  const char *errtxt = http_rc2str(error);
  htsbuf_queue_flush(&hc->hc_reply);

  htsbuf_qprintf(&hc->hc_reply, 
		 "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
		 "<HTML><HEAD>\r\n"
		 "<TITLE>%d %s</TITLE>\r\n"
		 "</HEAD><BODY>\r\n"
		 "<H1>%d %s</H1>\r\n",
		 error, errtxt, error, errtxt);

  if(str != NULL)
    htsbuf_qprintf(&hc->hc_reply, "<p>%s</p>\r\n", str);

  htsbuf_qprintf(&hc->hc_reply, "</BODY></HTML>\r\n");

  http_send_reply(hc, error, "text/html", NULL, NULL, 0);
  return 0;
}


/**
 * Send HTTP error back
 */
void
http_error(http_connection_t *hc, int error)
{
  http_err(hc, error, NULL);
}


/**
 * Send an HTTP OK, simple version for text/html
 */
int
http_output_html(http_connection_t *hc)
{
  return http_send_reply(hc, HTTP_STATUS_OK, "text/html; charset=UTF-8",
			 NULL, NULL, 0);
}

/**
 * Send an HTTP OK, simple version for text/html
 */
int
http_output_content(http_connection_t *hc, const char *content)
{
  return http_send_reply(hc, HTTP_STATUS_OK, content, NULL, NULL, 0);
}



/**
 * Send an HTTP REDIRECT
 */
void
http_redirect(http_connection_t *hc, const char *location, int status)
{
  htsbuf_queue_flush(&hc->hc_reply);

  htsbuf_qprintf(&hc->hc_reply,
		 "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
		 "<HTML><HEAD>\r\n"
		 "<TITLE>Redirect</TITLE>\r\n"
		 "</HEAD><BODY>\r\n"
		 "Please follow <a href=\"%s\">%s</a>\r\n"
		 "</BODY></HTML>\r\n",
		 location, location);

  http_send_reply(hc, status, "text/html", NULL, location, 0);
}


/**
 * Resolve URL and invoke handler
 *
 * If 'cont' is set we are pre-resolving for a 'Expect: 100-continue'
 * request
 *
 * If this function returns non-zero the conncetion will be terminated
 *
 * Normal errors are supposed to be handled without having to disconnection
 * the connection and thus they are sent inside here using http_error()
 *
 */
static int
http_resolve(http_connection_t *hc)
{
  int err;

  err = http_resolve_route(hc, 0);

  if(err == 404)
    err = http_resolve_path(hc);

  if(err == HTTP_ERROR_DISCONNECT)
    return 1;

  if(err)
    http_error(hc, err);
  return 0;
}





/**
 * Initial processing of HTTP POST
 *
 * Return non-zero if we should disconnect
 */
static int
http_cmd_post(http_connection_t *hc)
{
  char *v, *argv[2];
  int n;

  v = http_arg_get(&hc->hc_args, "Content-Length");
  if(v == NULL) {
    /* No content length in POST, make us disconnect */
    return HTTP_ERROR_DISCONNECT;
  }
  hc->hc_post_len = atoi(v);

  v = http_arg_get(&hc->hc_args, "Expect");
  if(v != NULL && !strcasecmp(v, "100-continue")) {
    int err = http_resolve_route(hc, 1);

    if(err == 100) {

      if(http_send_100_continue(hc))
        return 1;

    } else if(err != 0) {
      http_error(hc, err);
      return 0;
    }
  }


  if(hc->hc_post_len > 1024 * 1024 * 1024) {
    /* Bail out if POST data > 1 GB */
    hc->hc_keep_alive = 0;
    return HTTP_ERROR_DISCONNECT;
  }

  /* Allocate space for data, we add a terminating null char to ease
     string processing on the content */

  hc->hc_post_data = malloc(hc->hc_post_len + 1);
  hc->hc_post_data[hc->hc_post_len] = 0;

  if(tcp_read_data(hc->hc_ts, hc->hc_post_data, hc->hc_post_len) < 0)
    return HTTP_ERROR_DISCONNECT;

  /* Parse content-type */
  v = http_arg_get(&hc->hc_args, "Content-Type");
  if(v == NULL) {
    http_error(hc, HTTP_STATUS_BAD_REQUEST);
    return 0;
  }
  v = mystrdupa(v);
  n = str_tokenize(v, argv, 2, ';');
  if(n == 0) {
    http_error(hc, HTTP_STATUS_BAD_REQUEST);
    return 0;
  }

  hc->hc_content_type = argv[0];

  if(!strcmp(argv[0], "application/x-www-form-urlencoded"))
    http_parse_query_args(hc, hc->hc_post_data);

  assert(hc->hc_post_message == NULL);
  if(!strcmp(argv[0], "application/json")) {
    char errbuf[256];
    hc->hc_post_message = htsmsg_json_deserialize(hc->hc_post_data,
                                                  errbuf, sizeof(errbuf));

    if(hc->hc_post_message == NULL)
      return 400;
  }

  return http_resolve(hc);
}


/**
 * Process a HTTP request
 */
static int
http_process_request(http_connection_t *hc)
{
  // Split of query args

  char *args = strchr(hc->hc_path, '?');
  if(args != NULL) {
    *args = 0;
    http_parse_query_args(hc, args + 1);
  }


  switch(hc->hc_cmd) {
  default:
    http_error(hc, HTTP_STATUS_BAD_REQUEST);
    return 0;
  case HTTP_CMD_GET:
  case HTTP_CMD_DELETE:
    return http_resolve(hc);
  case HTTP_CMD_HEAD:
    hc->hc_no_output = 1;
    return http_resolve(hc);
  case HTTP_CMD_POST:
  case HTTP_CMD_PUT:
    return http_cmd_post(hc);
  }
}

/**
 * Process a request, extract info from headers, dispatch command and
 * clean up
 */
static int
process_request(http_connection_t *hc)
{
  char *v, *argv[2];
  int n, rval = -1;
  uint8_t authbuf[150];
  
  hc->hc_path_orig = strdup(hc->hc_path);

  /* Set keep-alive status */
  v = http_arg_get(&hc->hc_args, "connection");

  switch(hc->hc_version) {
  case RTSP_VERSION_1_0:
    hc->hc_keep_alive = 1;
    break;

  case HTTP_VERSION_1_0:
    /* Keep-alive is default off, but can be enabled */
    hc->hc_keep_alive = v != NULL && !strcasecmp(v, "keep-alive");
    break;
    
  case HTTP_VERSION_1_1:
    /* Keep-alive is default on, but can be disabled */
    hc->hc_keep_alive = !(v != NULL && !strcasecmp(v, "close"));
    break;
  }

  /* Extract authorization */
  if((v = http_arg_get(&hc->hc_args, "Authorization")) != NULL) {
    v = mystrdupa(v);
    if((n = str_tokenize(v, argv, 2, -1)) == 2) {

      if(!strcasecmp(argv[0], "basic")) {
        n = base64_decode(authbuf, argv[1], sizeof(authbuf) - 1);
        authbuf[n] = 0;
        if((n = str_tokenize((char *)authbuf, argv, 2, ':')) == 2) {
          hc->hc_username = strdup(argv[0]);
          hc->hc_password = strdup(argv[1]);
        }
      }
    }
  }

  if(hc->hc_username != NULL) {
    hc->hc_representative = strdup(hc->hc_username);
  } else {
    hc->hc_representative = malloc(30);
    /* Not threadsafe ? */
    snprintf(hc->hc_representative, 30,
	     "%s", inet_ntoa(hc->hc_peer->sin_addr));

  }

  switch(hc->hc_version) {
  case RTSP_VERSION_1_0:
    break;

  case HTTP_VERSION_1_0:
  case HTTP_VERSION_1_1:
    rval = http_process_request(hc);
    break;
  }
  free(hc->hc_representative);
  free(hc->hc_path_orig);
  return rval;
}




/*
 * Delete all arguments associated with a connection
 */
void
http_arg_flush(struct http_arg_list *list)
{
  http_arg_t *ra;
  while((ra = TAILQ_FIRST(list)) != NULL) {
    TAILQ_REMOVE(list, ra, link);
    free(ra->key);
    free(ra->val);
    free(ra);
  }
}


/**
 * Find an argument associated with a connection
 */
char *
http_arg_get(struct http_arg_list *list, const char *name)
{
  http_arg_t *ra;
  TAILQ_FOREACH(ra, list, link)
    if(!strcasecmp(ra->key, name))
      return ra->val;
  return NULL;
}


/**
 *
 */
int
http_arg_get_int(struct http_arg_list *list, const char *name,
                 int def)
{
  const char *arg = http_arg_get(list, name);
  return arg ? atoi(arg) : def;
}


/**
 * Set an argument associated with a connection
 */
void
http_arg_set(struct http_arg_list *list, const char *key,
             const char *val)
{
  http_arg_t *ra;

  ra = malloc(sizeof(http_arg_t));
  TAILQ_INSERT_TAIL(list, ra, link);
  ra->key = strdup(key);
  ra->val = strdup(val);
}


/**
 *
 */
static int route_cmp(const http_route_t *a, const http_route_t *b)
{
  return a->hr_depth - b->hr_depth;
}

/**
 * Add a regexp'ed route
 */
void
http_route_add(const char *path, http_callback2_t *callback, int flags)
{
  http_route_t *hr = malloc(sizeof(http_route_t));

  hr->hr_flags = flags;
  int len = strlen(path);
  hr->hr_depth = 0;
  for(int i = 0; i < len; i++)
    if(path[i] == '/')
      hr->hr_depth++;

  char *p = malloc(len + 2);
  p[0] = '^';
  strcpy(p+1, path);

  int rval = regcomp(&hr->hr_reg, p, REG_ICASE | REG_EXTENDED);
  free(p);
  if(rval) {
    char errbuf[256];
    regerror(rval, &hr->hr_reg, errbuf, sizeof(errbuf));
    trace(LOG_ALERT, "Failed to compile regex for HTTP route %s -- %s",
          path, errbuf);
    exit(1);
  }

  hr->hr_path     = strdup(path);
  hr->hr_callback = callback;
  LIST_INSERT_SORTED(&http_routes, hr, hr_link, route_cmp);
}


/**
 * Add a callback for a given "virtual path" on our HTTP server
 */
void
http_path_add(const char *path, void *opaque, http_callback_t *callback)
{
  http_path_t *hp = malloc(sizeof(http_path_t));

  hp->hp_len      = strlen(path);
  hp->hp_path     = strdup(path);
  hp->hp_opaque   = opaque;
  hp->hp_callback = callback;
  LIST_INSERT_HEAD(&http_paths, hp, hp_link);
}


/**
 * De-escape HTTP URL
 */
void
http_deescape(char *s)
{
  char v, *d = s;

  while(*s) {
    if(*s == '+') {
      *d++ = ' ';
      s++;
    } else if(*s == '%') {
      s++;
      switch(*s) {
      case '0' ... '9':
	v = (*s - '0') << 4;
	break;
      case 'a' ... 'f':
	v = (*s - 'a' + 10) << 4;
	break;
      case 'A' ... 'F':
	v = (*s - 'A' + 10) << 4;
	break;
      default:
	*d = 0;
	return;
      }
      s++;
      switch(*s) {
      case '0' ... '9':
	v |= (*s - '0');
	break;
      case 'a' ... 'f':
	v |= (*s - 'a' + 10);
	break;
      case 'A' ... 'F':
	v |= (*s - 'A' + 10);
	break;
      default:
	*d = 0;
	return;
      }
      s++;

      *d++ = v;
    } else {
      *d++ = *s++;
    }
  }
  *d = 0;
}


/**
 * Parse arguments of a HTTP GET url, not perfect, but works for us
 */
static void
http_parse_query_args(http_connection_t *hc, char *args)
{
  char *k, *v;

  while(args) {
    k = args;
    if((args = strchr(args, '=')) == NULL)
      break;
    *args++ = 0;
    v = args;
    args = strchr(args, '&');

    if(args != NULL)
      *args++ = 0;

    http_deescape(k);
    http_deescape(v);
    http_arg_set(&hc->hc_req_args, k, v);
  }
}


/**
 *
 */
static void
http_serve_requests(http_connection_t *hc)
{
  char cmdline[1024];
  char hdrline[1024];
  char *argv[3], *c;
  int n;


  htsbuf_queue_init(&hc->hc_reply, 0);

  do {
    talloc_cleanup();

    cfg_root(cr);
    int tracehttp = cfg_get_int(cr, CFG("http", "trace"), 0);

    hc->hc_no_output  = 0;

    if(tcp_read_line(hc->hc_ts, cmdline, sizeof(cmdline)) < 0) {
      return;
    }

    if(tracehttp)
      trace(LOG_DEBUG, "HTTP: %s", cmdline);

    if((n = str_tokenize(cmdline, argv, 3, -1)) != 3) {
      return;
    }

    if((hc->hc_cmd = str2val(argv[0], HTTP_cmdtab)) == -1) {
      return;
    }

    hc->hc_path = argv[1];
    if((hc->hc_version = str2val(argv[2], HTTP_versiontab)) == -1) {
      return;
    }

    /* parse header */
    while(1) {
      if(tcp_read_line(hc->hc_ts, hdrline, sizeof(hdrline)) < 0) {
	return;
      }

      if(tracehttp)
	trace(LOG_DEBUG, "HTTP: %s", hdrline);

      if(hdrline[0] == 0) {
	break; /* header complete */
      }

      if((n = str_tokenize(hdrline, argv, 2, -1)) < 2)
	continue;

      if((c = strrchr(argv[0], ':')) == NULL)
	return;

      *c = 0;
      http_arg_set(&hc->hc_args, argv[0], argv[1]);
    }

    if(process_request(hc)) {
      break;
    }


    if(hc->hc_post_message != NULL) {
      htsmsg_destroy(hc->hc_post_message);
      hc->hc_post_message = NULL;
    }

    free(hc->hc_post_data);
    hc->hc_post_data = NULL;

    http_arg_flush(&hc->hc_args);
    http_arg_flush(&hc->hc_req_args);
    http_arg_flush(&hc->hc_response_headers);

    htsbuf_queue_flush(&hc->hc_reply);

    free(hc->hc_username);
    hc->hc_username = NULL;

    free(hc->hc_password);
    hc->hc_password = NULL;

  } while(hc->hc_keep_alive);
  
}


/**
 *
 */
static void
http_serve(tcp_stream_t *ts, void *opaque, struct sockaddr_in *peer, 
	   struct sockaddr_in *self)
{
  http_connection_t hc;
  
  memset(&hc, 0, sizeof(http_connection_t));

  TAILQ_INIT(&hc.hc_args);
  TAILQ_INIT(&hc.hc_req_args);
  TAILQ_INIT(&hc.hc_response_headers);

  hc.hc_ts = ts;
  hc.hc_peer = peer;
  hc.hc_self = self;

  http_serve_requests(&hc);

  free(hc.hc_post_data);
  free(hc.hc_username);
  free(hc.hc_password);

  if(hc.hc_post_message != NULL)
    htsmsg_destroy(hc.hc_post_message);

  http_arg_flush(&hc.hc_args);
  http_arg_flush(&hc.hc_req_args);
  http_arg_flush(&hc.hc_response_headers);
  if(hc.hc_ts != NULL)
    tcp_close(hc.hc_ts);
}


/**
 *  Fire up HTTP server
 */
int
http_server_init(int port, const char *bindaddr)
{
  http_server = tcp_server_create(port, bindaddr, http_serve, NULL);
  if(http_server == NULL)
    return errno;
  return 0;
}
