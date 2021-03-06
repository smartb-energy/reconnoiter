/*
 * Copyright (c) 2011, OmniTI Computer Consulting, Inc.
 * Copyright (c) 2015, Circonus, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name OmniTI Computer Consulting, Inc. nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

#include <mtev_defines.h>

#include <poll.h>
#include <unistd.h>
#include <errno.h>

#include <mtev_dso.h>
#include <eventer/eventer.h>
#include <mtev_log.h>
#include <stratcon_iep.h>
#include <mtev_conf.h>

#include "librabbitmq/amqp.h"
#include "librabbitmq/amqp_framing.h"
#include "rabbitmq_driver.xmlh"

#define MAX_CONCURRENCY 16
#define MAX_HOSTS 10
#define DEFAULT_SNDBUF (1 << 20)
#define DEFAULT_RCVBUF (1 << 20)

static socklen_t desired_sndbuf = DEFAULT_SNDBUF;
static socklen_t desired_rcvbuf = DEFAULT_RCVBUF;

static pthread_mutex_t driver_lock;
struct amqp_driver {
  pthread_t owner;
  amqp_connection_state_t connection;
  char exchange[128];
  char routingkey[256];
  char username[80];
  char password[80];
  char vhost[256];
  int sockfd;
  int heartbeat;
  int nhosts;
  int nconnects;
  int hostidx;
  char hostname[10][256];
  int port;
  struct timeval last_hb;
  int has_error; /* out of band */
};

static struct {
  mtev_atomic64_t basic_returns;
  mtev_atomic64_t connects;
  mtev_atomic64_t inbound_methods;
  mtev_atomic64_t inbound_heartbeats;
  mtev_atomic64_t publications;
  mtev_atomic64_t concurrency;
  struct amqp_driver thread_states[MAX_CONCURRENCY];
} stats;
#define BUMPSTAT(a) mtev_atomic_inc64(&stats.a)

static iep_thread_driver_t *noit_rabbimq_allocate(mtev_conf_section_t conf) {
  char *hostname = NULL, *cp, *brk;
  struct amqp_driver *dr = NULL;
  int i;

  pthread_mutex_lock(&driver_lock);
  for(i=0; i<MAX_HOSTS; i++) {
    if(stats.thread_states[i].owner == (pthread_t)(intptr_t)NULL) {
      stats.thread_states[i].owner = pthread_self();
      dr = &stats.thread_states[i];
      break;
    }
  }
  pthread_mutex_unlock(&driver_lock);
  if(!dr) return NULL;
  dr->nconnects = rand();
#define GETCONFSTR(w) mtev_conf_get_stringbuf(conf, #w, dr->w, sizeof(dr->w))
  GETCONFSTR(exchange);
  if(!GETCONFSTR(routingkey))
    dr->routingkey[0] = '\0';
  GETCONFSTR(username);
  GETCONFSTR(password);
  if(!GETCONFSTR(vhost)) { dr->vhost[0] = '/'; dr->vhost[1] = '\0'; }
  if(!mtev_conf_get_int(conf, "heartbeat", &dr->heartbeat))
    dr->heartbeat = 5000;
  dr->heartbeat = (dr->heartbeat + 999) / 1000;

  (void)mtev_conf_get_string(conf, "hostname", &hostname);
  if(!hostname) hostname = strdup("127.0.0.1");
  for(cp = hostname; cp; cp = strchr(cp+1, ',')) dr->nhosts++;
  if(dr->nhosts > MAX_HOSTS) dr->nhosts = MAX_HOSTS;
  for(i = 0, cp = strtok_r(hostname, ",", &brk);
      cp; cp = strtok_r(NULL, ",", &brk), i++)
    strlcpy(dr->hostname[i], cp, sizeof(dr->hostname[i]));
  free(hostname);

  if(!mtev_conf_get_int(conf, "port", &dr->port))
    dr->port = 5672;
  mtev_atomic_inc64(&stats.concurrency);
  return (iep_thread_driver_t *)dr;
}
static int noit_rabbimq_disconnect(iep_thread_driver_t *d) {
  struct amqp_driver *dr = (struct amqp_driver *)d;
  if(dr->connection) {
    amqp_destroy_connection(dr->connection);
    if(dr->sockfd >= 0) close(dr->sockfd);
    dr->sockfd = -1;
    dr->connection = NULL;
    return 0;
  }
  return -1;
}
static void noit_rabbimq_deallocate(iep_thread_driver_t *d) {
  struct amqp_driver *dr = (struct amqp_driver *)d;
  noit_rabbimq_disconnect(d);
  pthread_mutex_lock(&driver_lock);
  memset(dr, 0, sizeof(*dr));
  pthread_mutex_unlock(&driver_lock);
  mtev_atomic_dec64(&stats.concurrency);
  free(dr);
}
static void noit_rabbitmq_set_filters(mq_command_t *command, int count) {
  /* NOT CURRENTLY IMPLEMENTED */
}
static void noit_rabbitmq_read_frame(struct amqp_driver *dr) {
  struct pollfd p;
  if(!dr->connection) return;
  while(1) {
    memset(&p, 0, sizeof(p));
    p.fd = dr->sockfd;
    p.events = POLLIN;
    if(poll(&p, 1, 0)) {
      int rv;
      amqp_frame_t f;
      rv = amqp_simple_wait_frame(dr->connection, &f);
      if(rv > 0) {
        if(f.frame_type == AMQP_FRAME_HEARTBEAT) {
          BUMPSTAT(inbound_heartbeats);
          mtevL(mtev_debug, "amqp <- hearbeat\n");
        }
        else if(f.frame_type == AMQP_FRAME_METHOD) {
          BUMPSTAT(inbound_methods);
          mtevL(mtev_error, "amqp <- method [%s]\n", amqp_method_name(f.payload.method.id));
          dr->has_error = 1;
          switch(f.payload.method.id) {
            case AMQP_CHANNEL_CLOSE_METHOD: {
                amqp_channel_close_t *m = (amqp_channel_close_t *) f.payload.method.decoded;
                mtevL(mtev_error, "AMQP channel close error %d: %s\n",
                      m->reply_code, (char *)m->reply_text.bytes);
              }
              break;
            case AMQP_CONNECTION_CLOSE_METHOD: {
                amqp_connection_close_t *m = (amqp_connection_close_t *) f.payload.method.decoded;
                mtevL(mtev_error, "AMQP connection close error %d: %s\n",
                      m->reply_code, (char *)m->reply_text.bytes);
              }
              break;
          }
        }
        else {
          mtevL(mtev_error, "amqp <- frame [%d]\n", f.frame_type);
        }
      }
      else break;
    }
    else break;
  }
}
static void noit_rabbitmq_heartbeat(struct amqp_driver *dr) {
  struct timeval n, d;
  if(!dr->connection) return;
  mtev_gettimeofday(&n, NULL);
  sub_timeval(n, dr->last_hb, &d);
  if(d.tv_sec >= dr->heartbeat) {
    amqp_frame_t f;
    f.frame_type = AMQP_FRAME_HEARTBEAT;
    f.channel = 0;
    amqp_send_frame(dr->connection, &f);
    mtevL(mtev_debug, "amqp -> hearbeat\n");
    memcpy(&dr->last_hb, &n, sizeof(n));
  }
}
static void
noit_rabbitmq_brcb(amqp_channel_t channel, amqp_basic_return_t *m, void *closure) {
  BUMPSTAT(basic_returns);
  mtevL(mtev_debug, "AMQP return [%d:%.*s]\n", m->reply_code,
        (int)m->reply_text.len, (char *)m->reply_text.bytes);
}
static int noit_rabbimq_connect(iep_thread_driver_t *dr) {
  struct amqp_driver *driver = (struct amqp_driver *)dr;

  if(!driver->connection) {
    int sidx = driver->nconnects++ % driver->nhosts;
    struct timeval timeout;
    amqp_rpc_reply_t r, *rptr;

    mtevL(mtev_error, "AMQP connect: %s:%d\n",
          driver->hostname[sidx], driver->port);
    BUMPSTAT(connects);
    driver->hostidx = sidx;
    timeout.tv_sec = driver->heartbeat;
    timeout.tv_usec = 0;
    driver->sockfd = amqp_open_socket(driver->hostname[sidx], driver->port, &timeout);
    if(driver->sockfd < 0) {
      mtevL(mtev_error, "AMQP connect failed: %s:%d\n",
            driver->hostname[sidx], driver->port);
      return -1;
    }
    if(setsockopt(driver->sockfd, SOL_SOCKET, SO_SNDBUF, &desired_sndbuf, sizeof(desired_sndbuf)) < 0)
      mtevL(mtev_debug, "rabbitmq: setsockopt(SO_SNDBUF, %ld) -> %s\n", (long int)desired_sndbuf, strerror(errno));
    if(setsockopt(driver->sockfd, SOL_SOCKET, SO_RCVBUF, &desired_rcvbuf, sizeof(desired_rcvbuf)) < 0)
      mtevL(mtev_debug, "rabbitmq: setsockopt(SO_RCVBUF, %ld) -> %s\n", (long int)desired_rcvbuf, strerror(errno));
    driver->has_error = 0;
    driver->connection = amqp_new_connection();
    amqp_set_basic_return_cb(driver->connection, noit_rabbitmq_brcb, driver);
    amqp_set_sockfd(driver->connection, driver->sockfd);
    r = amqp_login(driver->connection,
                   driver->vhost, 0, 131072, driver->heartbeat,
                   AMQP_SASL_METHOD_PLAIN,
                   driver->username, driver->password);
    if(r.reply_type != AMQP_RESPONSE_NORMAL) {
      mtevL(mtev_error, "AMQP login failed\n");
      amqp_connection_close(driver->connection, AMQP_REPLY_SUCCESS);
      amqp_destroy_connection(driver->connection);
      if(driver->sockfd >= 0) close(driver->sockfd);
      driver->sockfd = -1;
      driver->connection = NULL;
      return -1;
    }

    amqp_channel_open(driver->connection, 1);
    rptr = amqp_get_rpc_reply();
    if(rptr->reply_type != AMQP_RESPONSE_NORMAL) {
      mtevL(mtev_error, "AMQP channe_open failed\n");
      amqp_connection_close(driver->connection, AMQP_REPLY_SUCCESS);
      amqp_destroy_connection(driver->connection);
      if(driver->sockfd >= 0) close(driver->sockfd);
      driver->sockfd = -1;
      driver->connection = NULL;
      return -1;
    }
    mtev_gettimeofday(&driver->last_hb, NULL);
    return 0;
  }
  /* 1 means already connected */
  return 1;
}

/* This is very specific to an internal implementation somewhere...
 * and thus unlikely to be useful unless people name their checks:
 * c_<accountid>_<checknumber>::<rest of name>
 * This code should likley be made generic, perhaps with named
 * pcre captures.  However, I'm worried about performance.
 * For now, leave it and understand it is limited usefulness.
 */
static int extract_uuid_from_jlog(const char *payload, size_t payloadlen,
                                  int *account_id, int *check_id, char *dst) {
  int i = 0;
  const char *atab = payload, *u = NULL;

  if(account_id) *account_id = 0;
  if(check_id) *check_id = 0;

#define advance_past_tab do { \
  atab = memchr(atab, '\t', payloadlen - (atab - payload)); \
  if(!atab) return 0; \
  atab++; \
} while(0)

  /* Tab -> M|S|C */
  advance_past_tab;
  /* Tab -> noit IP */
  advance_past_tab;
  /* Tab -> timestamp */
  advance_past_tab;
  /* Tab -> uuid */
  u = atab;
  advance_past_tab;
  /* Tab -> metric_name */
  atab--;
  if(atab - u < UUID_STR_LEN) return 0;
  if(atab - u > UUID_STR_LEN) {
    const char *f;
    f = memchr(u, '`', payloadlen - (u - payload));
    if(f) {
      f = memchr(f+1, '`', payloadlen - (f + 1 - payload));
      if(f) {
        f++;
        if(memcmp(f, "c_", 2) == 0) {
          f += 2;
          if(account_id) *account_id = atoi(f);
          f = memchr(f, '_', payloadlen - (f - payload));
          if(f) {
            f++;
            if(check_id) *check_id = atoi(f);
          }
        }
      }
    }
  }
  u = atab - UUID_STR_LEN;
  while(i<32 && u < atab) {
    if((*u >= 'a' && *u <= 'f') ||
       (*u >= '0' && *u <= '9')) {
      dst[i*2] = '.';
      dst[i*2 + 1] = *u;
      i++;
    }
    else if(*u != '-') return 0;
    u++;
  }
  dst[i*2] = '\0';
  return 1;
}
static int
noit_rabbimq_submit(iep_thread_driver_t *dr,
                    const char *payload, size_t payloadlen) {
  int rv;
  amqp_bytes_t body;
  struct amqp_driver *driver = (struct amqp_driver *)dr;
  const char *routingkey = driver->routingkey;

  body.len = payloadlen;
  body.bytes = (char *)payload;
  if(*payload == 'M' ||
     *payload == 'S' ||
     *payload == 'C' ||
     (*payload == 'H' && payload[1] == '1') ||
     (*payload == 'F' && payload[1] == '1') ||
     (*payload == 'B' && (payload[1] == '1' || payload[1] == '2'))) {
    char uuid_str[32 * 2 + 1];
    int account_id, check_id;
    if(extract_uuid_from_jlog(payload, payloadlen,
                              &account_id, &check_id, uuid_str)) {
      if(*routingkey) {
        char *replace;
        int newlen = strlen(driver->routingkey) + 1 + sizeof(uuid_str) + 2 * 32;
        replace = alloca(newlen);
        snprintf(replace, newlen, "%s.%x.%x.%d.%d%s", driver->routingkey,
                 account_id%16, (account_id/16)%16, account_id,
                 check_id, uuid_str);
        routingkey = replace;
      }
    }
  }
  rv = amqp_basic_publish(driver->connection, 1,
                          amqp_cstring_bytes(driver->exchange),
                          amqp_cstring_bytes(routingkey),
                          1, 0, NULL, body);
  if(rv < 0) {
    mtevL(mtev_error, "AMQP publish failed, disconnecting\n");
    amqp_connection_close(driver->connection, AMQP_REPLY_SUCCESS);
    amqp_destroy_connection(driver->connection);
    if(driver->sockfd >= 0) close(driver->sockfd);
    driver->sockfd = -1;
    driver->connection = NULL;
    return -1;
  }
  BUMPSTAT(publications);
  noit_rabbitmq_heartbeat(driver);
  noit_rabbitmq_read_frame(driver);
  amqp_maybe_release_buffers(driver->connection);
  if(driver->has_error) {
    amqp_connection_close(driver->connection, AMQP_REPLY_SUCCESS);
    amqp_destroy_connection(driver->connection);
    if(driver->sockfd >= 0) close(driver->sockfd);
    driver->sockfd = -1;
    driver->connection = NULL;
    return -1;
  }
  return 0;
}

mq_driver_t mq_driver_rabbitmq = {
  noit_rabbimq_allocate,
  noit_rabbimq_connect,
  noit_rabbimq_submit,
  noit_rabbimq_disconnect,
  noit_rabbimq_deallocate,
  noit_rabbitmq_set_filters
};

static int noit_rabbimq_driver_config(mtev_dso_generic_t *self, mtev_hash_table *o) {
  const char *intstr;
  if(mtev_hash_retr_str(o, "sndbuf", strlen("sndbuf"), &intstr))
    desired_sndbuf = atoi(intstr);
  if(mtev_hash_retr_str(o, "rcvbuf", strlen("rcvbuf"), &intstr))
    desired_rcvbuf = atoi(intstr);
  return 0;
}
static int noit_rabbimq_driver_onload(mtev_image_t *self) {
  return 0;
}

static int
noit_console_show_rabbitmq(mtev_console_closure_t ncct,
                           int argc, char **argv,
                           mtev_console_state_t *dstate,
                           void *closure) {
  int i;
  nc_printf(ncct, " == RabbitMQ ==\n");
  nc_printf(ncct, " Concurrency:           %llu\n", stats.concurrency);
  nc_printf(ncct, " Connects:              %llu\n", stats.connects);
  nc_printf(ncct, " AMQP basic returns:    %llu\n", stats.basic_returns);
  nc_printf(ncct, " AMQP methods (in):     %llu\n", stats.inbound_methods);
  nc_printf(ncct, " AMQP heartbeats (in):  %llu\n", stats.inbound_heartbeats);
  nc_printf(ncct, " AMQP basic publish:    %llu\n", stats.publications);
  pthread_mutex_lock(&driver_lock);
  for(i=0;i<MAX_HOSTS;i++) {
    struct amqp_driver *dr;
    if(!stats.thread_states[i].owner) continue;
    dr = &stats.thread_states[i];
    nc_printf(ncct, "   == connection: %p ==\n", (void *)(intptr_t)dr->owner);
    if(dr->connection)
      nc_printf(ncct, "     %s@%s:%d (vhost: %s, exchange: %s)\n",
                dr->username, dr->hostname[dr->hostidx], dr->port, dr->vhost,
                dr->exchange);
    else
      nc_printf(ncct, "     not connected\n");
  }
  pthread_mutex_unlock(&driver_lock);
  return 1;
}

static void
register_console_rabbitmq_commands() {
  mtev_console_state_t *tl;
  cmd_info_t *showcmd;

  tl = mtev_console_state_initial();
  showcmd = mtev_console_state_get_cmd(tl, "show");
  mtevAssert(showcmd && showcmd->dstate);
  mtev_console_state_add_cmd(showcmd->dstate,
    NCSCMD("rabbitmq", noit_console_show_rabbitmq, NULL, NULL, NULL));
}

static int noit_rabbimq_driver_init(mtev_dso_generic_t *self) {
  pthread_mutex_init(&driver_lock, NULL);
  memset(&stats, 0, sizeof(stats));
  stratcon_iep_mq_driver_register("rabbitmq", &mq_driver_rabbitmq);
  register_console_rabbitmq_commands();
  return 0;
}

mtev_dso_generic_t rabbitmq_driver = {
  {
    .magic = MTEV_GENERIC_MAGIC,
    .version = MTEV_GENERIC_ABI_VERSION,
    .name = "rabbitmq_driver",
    .description = "AMQP driver for IEP MQ submission",
    .xml_description = rabbitmq_driver_xml_description,
    .onload = noit_rabbimq_driver_onload
  },
  noit_rabbimq_driver_config,
  noit_rabbimq_driver_init
};

