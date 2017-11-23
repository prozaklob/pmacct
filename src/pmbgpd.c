/*  
    pmacct (Promiscuous mode IP Accounting package)
    pmacct is Copyright (C) 2003-2017 by Paolo Lucente
*/

/*
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/* defines */
#define __PMBGPD_C

/* includes */
#include "pmacct.h"
#include "bgp/bgp.h"
#include "plugin_hooks.h"
#include "pmbgpd.h"
#include "pretag_handlers.h"
#include "pmacct-data.h"
#include "pkt_handlers.h"
#include "ip_flow.h"
#include "classifier.h"
#include "net_aggr.h"
#include "thread_pool.h"

/* global var */
struct channels_list_entry channels_list[MAX_N_PLUGINS]; /* communication channels: core <-> plugins */
thread_pool_t *bgp_lg_pool;

/* Functions */
void usage_daemon(char *prog_name)
{
  printf("%s %s (%s)\n", PMBGPD_USAGE_HEADER, PMACCT_VERSION, PMACCT_BUILD);
  printf("Usage: %s [ -D | -d ] [ -L IP address ] [ -l port ] ]\n", prog_name);
  printf("       %s [ -f config_file ]\n", prog_name);
  printf("       %s [ -h ]\n", prog_name);
  printf("\nGeneral options:\n");
  printf("  -h  \tShow this page\n");
  printf("  -V  \tShow version and compile-time options and exit\n");
  printf("  -L  \tBind to the specified IP address\n");
  printf("  -l  \tListen on the specified TCP port\n");
  printf("  -f  \tLoad configuration from the specified file\n");
  printf("  -D  \tDaemonize\n");
  printf("  -d  \tEnable debug\n");
  printf("  -S  \t[ auth | mail | daemon | kern | user | local[0-7] ] \n\tLog to the specified syslog facility\n");
  printf("  -F  \tWrite Core Process PID into the specified file\n");
  printf("  -o  \tOutput file to log real-time BGP messages\n");
  printf("  -O  \tOutput file to dump generated RIBs at regular time intervals\n");
  printf("  -i  \tInterval, in secs, to write to the dump output file (supplied by -O)\n");
  printf("\n");
  printf("For examples, see:\n");
  printf("  https://github.com/pmacct/pmacct/blob/master/QUICKSTART or\n");
  printf("  https://github.com/pmacct/pmacct/wiki\n");
  printf("\n");
  printf("For suggestions, critics, bugs, contact me: %s.\n", MANTAINER);
}

int main(int argc,char **argv, char **envp)
{
  struct plugins_list_entry *list;
  char config_file[SRVBUFLEN];
  int logf;

  /* getopt() stuff */
  extern char *optarg;
  extern int optind, opterr, optopt;
  int errflag, cp;

#if defined HAVE_MALLOPT
  mallopt(M_CHECK_ACTION, 0);
#endif

  umask(077);

  memset(cfg_cmdline, 0, sizeof(cfg_cmdline));
  memset(&config, 0, sizeof(struct configuration));
  memset(&config_file, 0, sizeof(config_file));

  log_notifications_init(&log_notifications);
  config.acct_type = ACCT_PMBGP;

  find_id_func = NULL;
  plugins_list = NULL;
  errflag = 0;
  rows = 0;

  /* getting commandline values */
  while (!errflag && ((cp = getopt(argc, argv, ARGS_PMBGPD)) != -1)) {
    cfg_cmdline[rows] = malloc(SRVBUFLEN);
    switch (cp) {
    case 'L':
      strlcpy(cfg_cmdline[rows], "bgp_daemon_ip: ", SRVBUFLEN);
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'l':
      strlcpy(cfg_cmdline[rows], "bgp_daemon_port: ", SRVBUFLEN);
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'D':
      strlcpy(cfg_cmdline[rows], "daemonize: true", SRVBUFLEN);
      rows++;
      break;
    case 'd':
      debug = TRUE;
      strlcpy(cfg_cmdline[rows], "debug: true", SRVBUFLEN);
      rows++;
      break;
    case 'f':
      strlcpy(config_file, optarg, sizeof(config_file));
      break;
    case 'F':
      strlcpy(cfg_cmdline[rows], "pidfile: ", SRVBUFLEN);
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'S':
      strlcpy(cfg_cmdline[rows], "syslog: ", SRVBUFLEN);
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'o':
      strlcpy(cfg_cmdline[rows], "bgp_daemon_msglog_file: ", SRVBUFLEN);
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'O':
      strlcpy(cfg_cmdline[rows], "bgp_table_dump_file: ", SRVBUFLEN);
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'i':
      strlcpy(cfg_cmdline[rows], "bgp_table_dump_refresh_time: ", SRVBUFLEN);
      strncat(cfg_cmdline[rows], optarg, CFG_LINE_LEN(cfg_cmdline[rows]));
      rows++;
      break;
    case 'g':
      strlcpy(cfg_cmdline[rows], "bgp_daemon_lg: true", SRVBUFLEN);
      rows++;
      break;
    case 'h':
      usage_daemon(argv[0]);
      exit(0);
      break;
    case 'V':
      version_daemon(PMBGPD_USAGE_HEADER);
      exit(0);
      break;
    default:
      usage_daemon(argv[0]);
      exit(1);
      break;
    }
  }


  /* post-checks and resolving conflicts */
  if (strlen(config_file)) {
    if (parse_configuration_file(config_file) != SUCCESS)
      exit(1);
  }
  else {
    if (parse_configuration_file(NULL) != SUCCESS)
      exit(1);
  }

  list = plugins_list;
  while (list) {
    list->cfg.acct_type = ACCT_PMBGP;
    set_default_preferences(&list->cfg);
    if (!strcmp(list->type.string, "core")) {
      memcpy(&config, &list->cfg, sizeof(struct configuration));
      config.name = list->name;
      config.type = list->type.string;
    }
    list = list->next;
  }

  if (config.files_umask) umask(config.files_umask);

  if (config.daemon) {
    if (debug || config.debug)
      printf("WARN ( %s/core ): debug is enabled; forking in background. Logging to standard error (stderr) will get lost.\n", config.name);
    daemonize();
  }

  initsetproctitle(argc, argv, envp);
  if (config.syslog) {
    logf = parse_log_facility(config.syslog);
    if (logf == ERR) {
      config.syslog = NULL;
      printf("WARN ( %s/core ): specified syslog facility is not supported. Logging to standard error (stderr).\n", config.name);
    }
    else openlog(NULL, LOG_PID, logf);
    Log(LOG_INFO, "INFO ( %s/core ): Start logging ...\n", config.name);
  }

  if (config.logfile)
  {
    config.logfile_fd = open_output_file(config.logfile, "a", FALSE);
    while (list) {
      list->cfg.logfile_fd = config.logfile_fd ;
      list = list->next;
    }
  }

  if (config.proc_priority) {
    int ret;

    ret = setpriority(PRIO_PROCESS, 0, config.proc_priority);
    if (ret) Log(LOG_WARNING, "WARN ( %s/core ): proc_priority failed (errno: %d)\n", config.name, errno);
    else Log(LOG_INFO, "INFO ( %s/core ): proc_priority set to %d\n", config.name, getpriority(PRIO_PROCESS, 0));
  }

  if (strlen(config_file)) {
    char canonical_path[PATH_MAX], *canonical_path_ptr;

    canonical_path_ptr = realpath(config_file, canonical_path);
    if (canonical_path_ptr) Log(LOG_INFO, "INFO ( %s/core ): Reading configuration file '%s'.\n", config.name, canonical_path);
  }
  else Log(LOG_INFO, "INFO ( %s/core ): Reading configuration from cmdline.\n", config.name);

  pm_setproctitle("%s [%s]", "Core Process", config.proc_name);
  if (config.pidfile) write_pid_file(config.pidfile);

  /* signal handling we want to inherit to plugins (when not re-defined elsewhere) */
  signal(SIGCHLD, startup_handle_falling_child); /* takes note of plugins failed during startup phase */
  signal(SIGHUP, reload); /* handles reopening of syslog channel */
  signal(SIGUSR1, SIG_IGN);
  signal(SIGUSR2, reload_maps); /* sets to true the reload_maps flag */
  signal(SIGPIPE, SIG_IGN); /* we want to exit gracefully when a pipe is broken */
  signal(SIGINT, my_sigint_handler);
  signal(SIGTERM, my_sigint_handler);

  if (!config.nfacctd_bgp) config.nfacctd_bgp = BGP_DAEMON_ONLINE;
  if (!config.nfacctd_bgp_port) config.nfacctd_bgp_port = BGP_TCP_PORT;

#if defined WITH_ZMQ
  if (config.bgp_lg) bgp_lg_wrapper();
#endif

  bgp_prepare_daemon();
  skinny_bgp_daemon();
}

#if defined WITH_ZMQ
#if defined ENABLE_THREADS
void bgp_lg_wrapper()
{
  /* initialize variables */
  if (!config.bgp_lg_ip) config.bgp_lg_ip = bgp_lg_default_ip;
  if (!config.bgp_lg_port) config.bgp_lg_port = BGP_LG_DEFAULT_TCP_PORT;
  if (!config.bgp_lg_threads) config.bgp_lg_threads = BGP_LG_DEFAULT_THREADS;

  /* initialize threads pool */
  bgp_lg_pool = allocate_thread_pool(1);
  assert(bgp_lg_pool);
  Log(LOG_DEBUG, "DEBUG ( %s/core/lg ): pmbgpd Looking Glass thread initialized\n", config.name, 1);

  /* giving a kick to the BGP thread */
  send_to_pool(bgp_lg_pool, bgp_lg_daemon, NULL);
}
#else
void bgp_lg_wrapper()
{
}
#endif /* ENABLE_THREADS */

void bgp_lg_daemon()
{
  char inproc_str[] = "inproc://lg_host_backend", log_id[SHORTBUFLEN];
  struct p_zmq_host lg_host;
  int idx;

  memset(&lg_host, 0, sizeof(lg_host));

  snprintf(log_id, sizeof(log_id), "%s/core/lg", config.name);
  p_zmq_set_log_id(&lg_host, log_id);

  p_zmq_router_setup(&lg_host, config.bgp_lg_ip, config.bgp_lg_port);
  Log(LOG_INFO, "INFO ( %s/core/lg ): Looking Glass listening on %s:%u\n", config.name, config.bgp_lg_ip, config.bgp_lg_port);

  lg_host.router_worker_func = &bgp_lg_daemon_worker;
  p_zmq_router_backend_setup(&lg_host, config.bgp_lg_threads, inproc_str);
}

void bgp_lg_daemon_worker(void *zh, void *zs)
{
  struct p_zmq_host *lg_host = (struct p_zmq_host *) zh;
  struct p_zmq_sock *sock = zs;
  struct bgp_lg_req req;
  struct bgp_lg_rep rep;
  int ret;

  if (!lg_host || !sock) {
    Log(LOG_ERR, "ERROR ( %s/core/lg ): bgp_lg_daemon_worker no lg_host or sock\nExiting.\n", config.name);
    exit(1);
  }

  memset(&req, 0, sizeof(req));
  memset(&rep, 0, sizeof(rep));

  for (;;) {
    ret = bgp_lg_daemon_decode_query_type(sock, &req);

    switch(req.type) {
    case BGP_LG_QT_IP_LOOKUP:
      {
        struct bgp_lg_req_ipl_data query_data;

        req.data = &query_data;
	memset(req.data, 0, sizeof(struct bgp_lg_req_ipl_data));
        ret = bgp_lg_daemon_decode_query_ip_lookup(sock, req.data);

        bgp_lg_rep_init(&rep);
        if (!ret) bgp_lg_daemon_ip_lookup(req.data, &rep, FUNC_TYPE_BGP); 

        bgp_lg_daemon_encode_reply_ip_lookup(sock, &rep);
      }
      break;
    case BGP_LG_QT_UNKNOWN:
    default:
      bgp_lg_daemon_encode_reply_unknown(sock);
      break;
    }
  }
}

int bgp_lg_daemon_decode_query_type(struct p_zmq_sock *sock, struct bgp_lg_req *req) 
{
  json_error_t req_err;
  json_t *req_obj, *query_type_json;
  char *req_str;
  int ret = SUCCESS;

  if (!sock || !req) return ERR;

  req_str = p_zmq_recv_str(sock);
  req_obj = json_loads(req_str, 0, &req_err);
  free(req_str);

  if (req_obj) {
    if (!json_is_object(req_obj)) {
      Log(LOG_WARNING, "WARN ( %s/core/lg ): bgp_lg_daemon_decode_query_type(): json_is_object() failed.\n", config.name);
      ret = ERR;
      goto exit_lane;
    }
    else {
      query_type_json = json_object_get(req_obj, "query_type");
      if (query_type_json == NULL) {
        Log(LOG_WARNING, "WARN ( %s/core/lg ): bgp_lg_daemon_decode_query_type(): no 'query_type' element.\n", config.name);
        ret = ERR;
        goto exit_lane;
      }
      else {
	req->type = json_integer_value(query_type_json);
	json_decref(query_type_json);
      }
    }

    exit_lane:
    json_decref(req_obj);
  }
  else {
    Log(LOG_WARNING, "WARN ( %s/core/lg ): bgp_lg_daemon_decode_query_type(): invalid request received: %s.\n", config.name, req_err.text);
    ret = ERR;
  }

  return ret;
}

int bgp_lg_daemon_decode_query_ip_lookup(struct p_zmq_sock *sock, struct bgp_lg_req_ipl_data *req) 
{
  json_error_t req_err;
  json_t *req_obj, *peer_ip_src_json, *ip_address_json;
  const char *peer_ip_src_str, *ip_address_str;
  char *req_str;
  int ret = SUCCESS;

  if (!sock || !req) return ERR;

  req_str = p_zmq_recv_str(sock);
  req_obj = json_loads(req_str, 0, &req_err);
  free(req_str);

  if (req_obj) {
    if (!json_is_object(req_obj)) {
      Log(LOG_WARNING, "WARN ( %s/core/lg ): bgp_lg_daemon_decode_query(): json_is_object() failed.\n", config.name);
      ret = ERR;
      goto exit_lane;
    }
    else {
      peer_ip_src_json = json_object_get(req_obj, "peer_ip_src");
      if (peer_ip_src_json == NULL) {
	Log(LOG_WARNING, "WARN ( %s/core/lg ): bgp_lg_daemon_decode_query(): no 'peer_ip_src' element.\n", config.name);
	ret = ERR;
	goto exit_lane;
      }
      else {
	struct host_addr peer_ip_src_ha;

	peer_ip_src_str = json_string_value(peer_ip_src_json);
	str_to_addr(peer_ip_src_str, &peer_ip_src_ha);
	addr_to_sa(&req->peer, &peer_ip_src_ha, FALSE);
	if (!req->peer.sa_family) {
	  Log(LOG_WARNING, "WARN ( %s/core/lg ): bgp_lg_daemon_decode_query(): bogus 'peer_ip_src' element.\n", config.name);
	  ret = ERR;
	  goto exit_lane;
	}

        json_decref(peer_ip_src_json);
      }

      ip_address_json = json_object_get(req_obj, "ip_address");
      if (ip_address_json == NULL) {
	Log(LOG_WARNING, "WARN ( %s/core/lg ): bgp_lg_daemon_decode_query: no 'ip_address' element.\n", config.name);
	ret = ERR;
	goto exit_lane;
      }
      else {
	ip_address_str = json_string_value(ip_address_json);
	str2prefix(ip_address_str, &req->pref);
	if (!req->pref.family) {
	  Log(LOG_WARNING, "WARN ( %s/core/lg ): bgp_lg_daemon_decode_query(): bogus 'ip_address' element.\n", config.name);
	  ret = ERR;
	  goto exit_lane;
	}

        json_decref(ip_address_json);
      }

      // XXX: decode Route Distinguisher, if any
    }

    exit_lane:
    json_decref(req_obj);
  }
  else {
    Log(LOG_WARNING, "WARN ( %s/core/lg ): bgp_lg_daemon_decode_query(): invalid request received: %s.\n", config.name, req_err.text);
    ret = ERR;
  }

  return ret;
}

void bgp_lg_daemon_encode_reply_ip_lookup(struct p_zmq_sock *sock, struct bgp_lg_rep *rep) 
{
  json_t *rep_results_obj;
  char *rep_results_str;

  if (!sock || !rep) return;

  rep_results_obj = json_object();
  json_object_set_new_nocheck(rep_results_obj, "results", json_integer(rep->results));
  json_object_set_new_nocheck(rep_results_obj, "query_type", json_integer(BGP_LG_QT_IP_LOOKUP));

  rep_results_str = json_dumps(rep_results_obj, JSON_PRESERVE_ORDER);
  json_decref(rep_results_obj);
  
  if (!rep->results) p_zmq_send_str(sock, rep_results_str);
  else {
    struct bgp_lg_rep_ipl_data *data;
    json_t *rep_data_obj;
    char *rep_data_str;
    u_int32_t idx;

    p_zmq_sendmore_str(sock, rep_results_str);

    for (idx = 0, data = rep->data; idx < rep->results; idx++) {
      rep_data_str = bgp_lg_daemon_encode_reply_ip_lookup_data(data);

      if (rep_data_str) {
	if (idx == (rep->results - 1)) p_zmq_send_str(sock, rep_data_str); 
        else p_zmq_sendmore_str(sock, rep_data_str);

	free(rep_data_str);
      }

      data = data->next;
    }
  }

  if (rep_results_str) free(rep_results_str);
}

char *bgp_lg_daemon_encode_reply_ip_lookup_data(struct bgp_lg_rep_ipl_data *rep_data)
{
  struct bgp_node dummy_node;
  char event_type[] = "lglass", *data_str = NULL;

  if (rep_data && rep_data->pref) {
    memset(&dummy_node, 0, sizeof(dummy_node));
    memcpy(&dummy_node.p, rep_data->pref, sizeof(struct prefix)); 

    bgp_peer_log_msg(&dummy_node, rep_data->info, rep_data->afi, rep_data->safi, event_type,
		     PRINT_OUTPUT_JSON /* XXX: allow for different encodings */, &data_str,
		     BGP_LOG_TYPE_MISC);
  }

  return data_str;
}

void bgp_lg_daemon_encode_reply_unknown(struct p_zmq_sock *sock)
{
  json_t *rep_results_obj;
  char *rep_results_str;

  if (!sock) return;

  rep_results_obj = json_object();
  json_object_set_new_nocheck(rep_results_obj, "results", json_integer(FALSE));
  json_object_set_new_nocheck(rep_results_obj, "query_type", json_integer(BGP_LG_QT_UNKNOWN));
  // XXX: add some error string?

  rep_results_str = json_dumps(rep_results_obj, JSON_PRESERVE_ORDER);
  json_decref(rep_results_obj);
}
#endif /* WITH_ZMQ */ 
