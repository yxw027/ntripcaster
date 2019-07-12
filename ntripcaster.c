#include <time.h>
#include "logh/log.h"
#include "wsocket/wsocket.h"
#include "queue.h"

#ifdef _WIN32
# include "evwrap.h"
#else
# include <ev.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stddef.h>
#include <signal.h>

enum ntrip_agent_type {
    NTRIP_PENDING_AGENT = 0,
    NTRIP_SOURCE_AGENT,
    NTRIP_CLIENT_AGENT,
    NTRIP_AGENT_SENTRY,
};

struct ntrip_caster;

struct ntrip_agent {
    ev_io io;
    wsocket socket;
    int  type;                  // agent type: ntrip_agent_type
    char mountpoint[64];        // mountpoint requested by agent
    char peeraddr[NI_MAXHOST];  // agent ip address
    char user_agent[64];        // agent ntrip user agent string
    time_t login_time;          // agent login time
    ev_tstamp last_activity;    // agent last IO time
    unsigned char pending_recv[1024];// pending agent socket recv buffer
    size_t pending_idx;              // pending agent socket recv buffer index

    //TODO: traffic status
    size_t in_bytes;    // in bound bytes
    size_t in_Bps;      // in bound Bps (bytes/s)
    size_t out_bytes;   // out bound bytes
    size_t out_Bps;     // out bound Bps (bytes/s)

    struct ntrip_caster* caster; // caster associate with agent(not change during whole agent lifetime)
    TAILQ_ENTRY(ntrip_agent) entries;  // agent list
};

// for authorization
struct ntrip_token {
    char token[64];         // token
    char read_mnt[64];      // associate mountpoint that token can read as client
    char write_mnt[64];     // associate mountpoint that token can write as source server
    TAILQ_ENTRY(ntrip_token) entries;   // token list
};

struct ntrip_caster_config {
    size_t MAX_PENDING_CNT; // pending agent count limit, 0 means no limit
    size_t MAX_CLIENT_CNT;  // client agent count limit, 0 means no limit
    size_t MAX_SOURCE_CNT;  // source agent count limit, 0 meas not limit
    // TODO: token & other config
    TAILQ_HEAD(, ntrip_token) token_head;  // token list
    char   bind_addr[64];   // caster bind address, "" means NULL
    char   bind_serv[16];   // caster bind port service
};

struct ntrip_caster {
    ev_io io;
    ev_timer timer; // timer for check agent alive
    wsocket socket;
    TAILQ_HEAD(, ntrip_agent) agents_head[NTRIP_AGENT_SENTRY]; // agent list for PENDING/CLIENT/SOURCE
    size_t agents_cnt[NTRIP_AGENT_SENTRY];   // agents count for PENDING/CLIENT/SOURCE
    struct ntrip_caster_config config;  // config
};


#define DEFAULT_MAX_PENDING_AGENT   20
#define DEFAULT_MAX_CLIENT_AGENT    100
#define DEFAULT_MAX_SOURCE_AGENT    20


#define NTRIP_RESPONSE_OK           "ICY 200 OK\r\n"
#define NTRIP_RESPONSE_UNAUTHORIZED "HTTP/1.0 401 Unauthorized\r\n"
#define NTRIP_RESPONSE_FORBIDDEN    "HTTP/1.0 403 Forbidden\r\n"
#define NTRIP_RESPONSE_ERROR_PASSED "ERROR - Bad Password\r\n"
#define NTRIP_RESPONSE_ERROR_MOUNTP "ERROR - Bad Mountpoint\r\n"

static wsocket listen_on(const char *addr, const char* service)
{
    wsocket sock = INVALID_WSOCKET;

    struct addrinfo hints = {0};
    hints.ai_family = PF_UNSPEC;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int rv = 0;
    struct addrinfo *ai = NULL;
    if ((rv = getaddrinfo(addr, service, &hints, &ai)) != 0) {
        LOG_ERROR("getaddrinfo() error, %s", gai_strerror(rv));
        return INVALID_WSOCKET;
    }
    for (const struct addrinfo *p = ai; p != NULL; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (wsocket_set_nonblocking(sock) == WSOCKET_ERROR) {
            LOG_ERROR("set nonblocking error, %s", wsocket_strerror(wsocket_errno));
            wsocket_close(sock);
            return INVALID_WSOCKET;
        }
        if (sock == INVALID_WSOCKET) {
            continue;
        }
        // enable addr resuse
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
        if (bind(sock, p->ai_addr, p->ai_addrlen) == WSOCKET_ERROR) {
            // bind error
            wsocket_close(sock);
            sock = INVALID_WSOCKET;
            continue;
        }
        // Got it!
        break;
    }

    if (sock == INVALID_WSOCKET) {
        LOG_ERROR("socket() or bind() error, %s", wsocket_strerror(wsocket_errno));
        freeaddrinfo(ai);
        ai = NULL;
        return INVALID_WSOCKET;
    }

    freeaddrinfo(ai);
    ai = NULL;

    if (listen(sock, 2) == WSOCKET_ERROR) {
        LOG_ERROR("listen() error, %s", wsocket_strerror(wsocket_errno));
        wsocket_close(sock);
        return INVALID_WSOCKET;
    }

    return sock;
}


static void close_agent(struct ntrip_agent *agent)
{
    LOG_INFO("close agent(%d) from %s", agent->socket, agent->peeraddr);
    TAILQ_REMOVE(&agent->caster->agents_head[agent->type], agent, entries);
    agent->caster->agents_cnt[agent->type] -= 1;
    wsocket_close(agent->socket);
    free(agent);
}

static void delay_close_once_cb(int revents, void* arg)
{
    if (revents & EV_TIMER) {
        struct ntrip_agent *agent = arg;
        close_agent(agent);
    }
}



static void agent_read_cb(EV_P_ ev_io *w, int revents)
{
    struct ntrip_agent *agent = (struct ntrip_agent *)w;

    if (agent->type == NTRIP_PENDING_AGENT) {
        // read and check what type agent
        int n = recv(agent->socket,
                     agent->pending_recv + agent->pending_idx,
                     sizeof(agent->pending_recv) - agent->pending_idx - 1,
                     0);

        if (n == WSOCKET_ERROR && wsocket_errno != WSOCKET_EWOULDBLOCK) {
            LOG_ERROR("agent(%d) recv error, %s", agent->socket, wsocket_strerror(wsocket_errno));
            ev_io_stop(EV_A_ &agent->io);
            close_agent(agent);
            return;
        }
        if (n == 0) {
            LOG_INFO("agent(%d) connection close", agent->socket);
            ev_io_stop(EV_A_ &agent->io);
            close_agent(agent);
            return;
        }
        if (n < 0) { // maybe -1 since WSOCKET_EWOULDBLOCK
            return;
        }
        agent->last_activity = ev_now(EV_A);
        agent->pending_idx =+ n;
        // check pending buffer overflow
        if (agent->pending_idx >= sizeof(agent->pending_recv) - 1) {
            LOG_ERROR("agent(%d) request buffer overflow", agent->socket);
            ev_io_stop(EV_A_ &agent->io);
            close_agent(agent);
            return;
        }
        agent->pending_recv[agent->pending_idx] = '\0';
        // test GET (ntrip client) or SOURCE (ntrip server)
        do {
            char* p = strstr(agent->pending_recv, "GET");
            if (p) {
                // ntrip client
                char *q, *ag;
                if (!(q = strstr(p, "\r\n")) || !(ag = strstr(q, "User-Agent:")) || !strstr(ag, "\r\n")) {
                    break;
                }
                ag += strlen("User-Agent:");
                // fill user agent
                sscanf(ag, "%63[^\n]", agent->user_agent);
                // test protocol
                char url[64], proto[64];
                url[0] = '\0';
                proto[0] = '\0';
                if (sscanf(p, "GET %63s %63s", url, proto) < 2 || strncmp(proto, "HTTP/1", strlen("HTTP/1")) != 0) {
                    LOG_ERROR("invalid ntrip proto=%s", proto);
                    break;
                }
                if ((p = strchr(url, '/'))) {
                    p += 1;
                }
                snprintf(agent->mountpoint, sizeof(agent->mountpoint), "%s", p);
                // TODO: check if mountpoint exist
                if (agent->mountpoint[0] == '\0' || strcmp(agent->mountpoint, "/") == 0) {
                    // TODO: send source table
                    ev_io_stop(EV_A_ &agent->io);
                    close_agent(agent);
                    return;
                }
                // TODO: check authentication
                // check if client agents max count
                if (agent->caster->config.MAX_CLIENT_CNT > 0 &&
                    agent->caster->config.MAX_CLIENT_CNT <= agent->caster->agents_cnt[NTRIP_CLIENT_AGENT]) {
                    LOG_WARN("too many client agents, now=%d, MAX=%d",
                             agent->caster->agents_cnt[NTRIP_CLIENT_AGENT],
                             agent->caster->config.MAX_CLIENT_CNT);
                    // send error message
                    send(agent->socket, NTRIP_RESPONSE_FORBIDDEN, strlen(NTRIP_RESPONSE_FORBIDDEN), 0);
                    LOG_INFO("kickoff pending agent(%d) from %s", agent->socket, agent->peeraddr);
                    ev_io_stop(EV_A_ &agent->io);
                    close_agent(agent);
                    return;
                }
                // send response
                send(agent->socket, NTRIP_RESPONSE_OK, strlen(NTRIP_RESPONSE_OK), 0);
                // move to clients list from pending
                agent->pending_idx = 0;
                TAILQ_REMOVE(&agent->caster->agents_head[agent->type], agent, entries);
                agent->caster->agents_cnt[agent->type] -= 1;
                agent->type = NTRIP_CLIENT_AGENT;
                TAILQ_INSERT_TAIL(&agent->caster->agents_head[agent->type], agent, entries);
                agent->caster->agents_cnt[agent->type] += 1;
                LOG_INFO("move agent(%d) into client agents", agent->socket);
                return;
            }
            p = strstr(agent->pending_recv, "SOURCE");
            if (p) {
                // ntrip server
                char *q, *ag;
                if (!(q = strstr(p, "\r\n")) || !(ag = strstr(q, "Source-Agent:")) || !strstr(ag, "\r\n")) {
                    break;
                }
                ag += strlen("Source-Agent:");
                // fill user agent
                sscanf(ag, "%63[^\n]", agent->user_agent);
                // get passwd and url
                char url[64], passwd[64];
                url[0] = '\0';
                passwd[0] = '\0';
                if (sscanf(p, "SOURCE %63s %63s", passwd, url) < 2) {
                    break;
                }
                snprintf(agent->mountpoint, sizeof(agent->mountpoint), "%s", url);

                // check if mountpoint exist
                if (agent->mountpoint[0] == '\0' || strcmp(agent->mountpoint, "/") == 0) {
                    send(agent->socket, NTRIP_RESPONSE_ERROR_MOUNTP, strlen(NTRIP_RESPONSE_ERROR_MOUNTP), 0);
                    ev_io_stop(EV_A_ &agent->io);
                    close_agent(agent);
                    return;
                }
                // TODO: check authentication
                // check if mountpoint source already exists
                // if so, then reject new agent
                struct ntrip_agent *server;
                TAILQ_FOREACH(server, &agent->caster->agents_head[NTRIP_SOURCE_AGENT], entries) {
                    if (strcasecmp(server->mountpoint, agent->mountpoint) == 0) {
                        // reject new agent
                        LOG_WARN("agent(%d) attempt source mountpoint(%s) which already has source agent(%d)",
                                 agent->socket, agent->mountpoint, server->socket);
                        send(agent->socket, NTRIP_RESPONSE_ERROR_MOUNTP, strlen(NTRIP_RESPONSE_ERROR_MOUNTP), 0);
                        ev_io_stop(EV_A_ &agent->io);
                        close_agent(agent);
                        return;
                    }
                }
                // check if source agents count max
                if (agent->caster->config.MAX_SOURCE_CNT > 0 &&
                    agent->caster->config.MAX_SOURCE_CNT <= agent->caster->agents_cnt[NTRIP_SOURCE_AGENT]) {
                    LOG_WARN("too many source agents, now=%d, MAX=%d",
                             agent->caster->agents_cnt[NTRIP_SOURCE_AGENT],
                             agent->caster->config.MAX_SOURCE_CNT);
                    // send error message
                    send(agent->socket, NTRIP_RESPONSE_ERROR_MOUNTP, strlen(NTRIP_RESPONSE_ERROR_MOUNTP), 0);
                    LOG_INFO("kickoff pending agent(%d) from %s", agent->socket, agent->peeraddr);
                    ev_io_stop(EV_A_ &agent->io);
                    close_agent(agent);
                    return;
                }
                // send response
                send(agent->socket, NTRIP_RESPONSE_OK, strlen(NTRIP_RESPONSE_OK), 0);
                // move to clients list from pending
                agent->pending_idx = 0;
                TAILQ_REMOVE(&agent->caster->agents_head[agent->type], agent, entries);
                agent->caster->agents_cnt[agent->type] -= 1;
                agent->type = NTRIP_SOURCE_AGENT;
                TAILQ_INSERT_TAIL(&agent->caster->agents_head[agent->type], agent, entries);
                agent->caster->agents_cnt[agent->type] += 1;
                LOG_INFO("move agent(%d) into source agents", agent->socket);
                return;
            }
            // not matching
        } while(0);
        // error occurs, stop and close agent
        LOG_INFO("agent(%d) request error", agent->socket);
        ev_io_stop(EV_A_ &agent->io);
        close_agent(agent);
    } else if (agent->type == NTRIP_CLIENT_AGENT) {
        // ntrip client read
        // now will read and discard client gga message
        char buf[512];
        int n = recv(agent->socket, buf, sizeof(buf) - 1, 0);
        if (n == WSOCKET_ERROR && wsocket_errno != WSOCKET_EWOULDBLOCK) {
            LOG_ERROR("agent(%d) recv error, %s", agent->socket, wsocket_strerror(wsocket_errno));
            ev_io_stop(EV_A_ &agent->io);
            close_agent(agent);
            return;
        }
        if (n == 0) {
            LOG_INFO("agent(%d) connection close", agent->socket);
            ev_io_stop(EV_A_ &agent->io);
            close_agent(agent);
            return;
        }
        if (n < 0) { // maybe -1 since WSOCKET_EWOULDBLOCK
            return;
        }

        agent->last_activity = ev_now(EV_A);
        // discard client data
    } else if (agent->type == NTRIP_SOURCE_AGENT) {
        // ntrip server read
        char buf[512];
        int n = recv(agent->socket, buf, sizeof(buf) - 1, 0);
        if (n == WSOCKET_ERROR && wsocket_errno != WSOCKET_EWOULDBLOCK) {
            LOG_ERROR("agent(%d) recv error, %s", agent->socket, wsocket_strerror(wsocket_errno));
            ev_io_stop(EV_A_ &agent->io);
            close_agent(agent);
            return;
        }
        if (n == 0) {
            LOG_INFO("agent(%d) connection close", agent->socket);
            ev_io_stop(EV_A_ &agent->io);
            close_agent(agent);
            return;
        }
        if (n < 0) { // maybe -1 since WSOCKET_EWOULDBLOCK
            return;
        }

        agent->last_activity = ev_now(EV_A);
        // send data to every match mountpoint clients
        struct ntrip_agent *client, *temp;
        TAILQ_FOREACH_SAFE(client, &agent->caster->agents_head[NTRIP_CLIENT_AGENT], entries, temp) {
            if (strcasecmp(agent->mountpoint, client->mountpoint) == 0) {
                if (send(client->socket, buf, n, 0) != WSOCKET_ERROR) {
                    client->last_activity = ev_now(EV_A);
                }
            }
        }
    } else {
        LOG_ERROR("close error type agent(%d, type=%d) from %s",
                  agent->socket, agent->type, agent->peeraddr);
        ev_io_stop(EV_A_ &agent->io);
        wsocket_close(agent->socket);
        free(agent);
    }
}


static void caster_accept_cb(EV_P_ ev_io *w, int revents)
{
    struct ntrip_caster* caster = (struct ntrip_caster *)w;
    wsocket agent_socket = INVALID_WSOCKET;
    struct sockaddr_storage agent_addr = {0};
    socklen_t agent_addrlen = sizeof(agent_addr);

    struct ntrip_agent *agent = NULL;

    if (EV_ERROR & revents) {
        LOG_ERROR("invalid ev event with error");
        return;
    }

    agent_socket = accept(caster->socket, (struct sockaddr *)&agent_addr, &agent_addrlen);
    if (agent_socket == INVALID_WSOCKET) {
        LOG_ERROR("accept() error, %s", wsocket_strerror(wsocket_errno));
        return;
    }
    // set nonblocking
    wsocket_set_nonblocking(agent_socket);
    // print connect info
    char addrbuf[NI_MAXHOST] = {0};
    char servbuf[NI_MAXSERV] = {0};
    int rv = 0;
    if ((rv = getnameinfo((struct sockaddr *)&agent_addr, agent_addrlen,
                          addrbuf, sizeof(addrbuf),
                          servbuf, sizeof(servbuf),
                          NI_NUMERICHOST | NI_NUMERICSERV)) == 0) {
        LOG_INFO("accept agent(%d) from %s:%s", agent_socket, addrbuf, servbuf);

    } else {
        LOG_ERROR("getnameinfo() error, %s", gai_strerror(rv));
    }
    // check if pending agents max count
    if (caster->config.MAX_PENDING_CNT > 0 &&
        caster->config.MAX_PENDING_CNT <= caster->agents_cnt[NTRIP_PENDING_AGENT]) {
        LOG_WARN("too many pending agents, now=%d, MAX=%d",
                 caster->agents_cnt[NTRIP_PENDING_AGENT],
                 caster->config.MAX_PENDING_CNT);
        LOG_INFO("reject agent(%d) from %s:%s", agent_socket, addrbuf, servbuf);
        wsocket_close(agent_socket);
        return;
    }
    agent = calloc(1, sizeof(*agent));
    if (agent == NULL) {
        LOG_ERROR("malloc() error, %s", strerror(errno));
        return;
    }
    agent->socket = agent_socket;
    agent->type = NTRIP_PENDING_AGENT;
    agent->mountpoint[0] = '\0';
    snprintf(agent->peeraddr, sizeof(agent->peeraddr), "%s", addrbuf);
    agent->user_agent[0] = '\0';
    agent->login_time = time(NULL);
    agent->last_activity = ev_now(EV_A);
    agent->pending_idx = 0;
    agent->caster = caster;

    ev_io_init(&agent->io, agent_read_cb, WSOCKET_GET_FD(agent_socket), EV_READ);
    TAILQ_INSERT_TAIL(&caster->agents_head[NTRIP_PENDING_AGENT], agent, entries);
    caster->agents_cnt[NTRIP_PENDING_AGENT] += 1;

    ev_io_start(EV_A_  &agent->io);
}

static void caster_timeout_cb(EV_P_ ev_timer *w, int revents)
{
    // check and remove non-active agent
    struct ntrip_caster *caster = (struct ntrip_caster *)((char *)w - offsetof(struct ntrip_caster, timer));
    ev_tstamp now = ev_now(EV_A);
    struct ntrip_agent *agent, *temp;
    for (int i = 0; i < NTRIP_AGENT_SENTRY; i++) {
        TAILQ_FOREACH_SAFE(agent, &caster->agents_head[i], entries, temp) {
            if (now - agent->last_activity >= 5.0) {
                LOG_INFO("timeout agent(%d) from %s", agent->socket, agent->peeraddr);
                ev_io_stop(EV_A_ &agent->io);
                close_agent(agent);
            }
        }
    }
}

static void caster_init_config(struct ntrip_caster_config *config)
{
    // init default
    config->MAX_PENDING_CNT = DEFAULT_MAX_PENDING_AGENT;
    config->MAX_CLIENT_CNT  = DEFAULT_MAX_CLIENT_AGENT;
    config->MAX_SOURCE_CNT  = DEFAULT_MAX_SOURCE_AGENT;
    TAILQ_INIT(&config->token_head);
    // TODO: read from config file
}

int main(int argc, const char *argv[])
{
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    WSOCKET_INIT();
    struct ev_loop* loop = EV_DEFAULT;
    struct ntrip_caster caster = {0};
    // init caster config
    caster_init_config(&caster.config);
    for (int i = 0; i < NTRIP_AGENT_SENTRY; i++) {
        caster.agents_cnt[i] = 0;
        TAILQ_INIT(&caster.agents_head[i]);
    }

    wsocket sock = listen_on("0.0.0.0", "2101");
    if (sock == INVALID_WSOCKET) {
        LOG_ERROR("setup server error.");
        return 1;
    } else {
        LOG_INFO("setup server on 2101 OK.");
    }

    caster.socket = sock;
    ev_io_init(&caster.io, caster_accept_cb, WSOCKET_GET_FD(sock), EV_READ);
    ev_io_start(EV_A_ &caster.io);
    ev_timer_init(&caster.timer, caster_timeout_cb, 5, 3);
    ev_timer_start(EV_A_ &caster.timer);

    while (1) {
        ev_loop(loop, 0);
    }

    WSOCKET_CLEANUP();
    return 0;
}
