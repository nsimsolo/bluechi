#include <arpa/inet.h>
#include <stdio.h>

#include "libhirte/bus/peer-bus.h"
#include "libhirte/bus/systemd-bus.h"
#include "libhirte/bus/user-bus.h"
#include "libhirte/common/common.h"
#include "libhirte/common/opt.h"
#include "libhirte/common/parse-util.h"
#include "libhirte/ini/config.h"
#include "libhirte/service/shutdown.h"

#include "agent.h"

struct JobTracker {
        char *job_object_path;
        job_tracker_callback callback;
        void *userdata;
        free_func_t free_userdata;
        LIST_FIELDS(JobTracker, tracked_jobs);
};

static bool
                agent_track_job(Agent *agent,
                                const char *job_object_path,
                                job_tracker_callback callback,
                                void *userdata,
                                free_func_t free_userdata);

SystemdRequest *systemd_request_ref(SystemdRequest *req) {
        req->ref_count++;
        return req;
}

void systemd_request_unref(SystemdRequest *req) {
        Agent *agent = req->agent;

        req->ref_count--;
        if (req->ref_count != 0) {
                return;
        }

        if (req->userdata && req->free_userdata) {
                req->free_userdata(req->userdata);
        }

        if (req->request_message) {
                sd_bus_message_unref(req->request_message);
        }
        if (req->slot) {
                sd_bus_slot_unref(req->slot);
        }
        if (req->message) {
                sd_bus_message_unref(req->message);
        }

        LIST_REMOVE(outstanding_requests, agent->outstanding_requests, req);
        agent_unref(req->agent);
        free(req);
}

void systemd_request_unrefp(SystemdRequest **reqp) {
        if (reqp && *reqp) {
                systemd_request_unref(*reqp);
        }
}

static SystemdRequest *agent_create_request(Agent *agent, sd_bus_message *request_message, const char *method) {
        _cleanup_systemd_request_ SystemdRequest *req = malloc0(sizeof(SystemdRequest));
        if (req == NULL) {
                return NULL;
        }

        req->ref_count = 1;
        req->agent = agent_ref(agent);
        LIST_INIT(outstanding_requests, req);
        req->request_message = sd_bus_message_ref(request_message);

        LIST_APPEND(outstanding_requests, agent->outstanding_requests, req);

        int r = sd_bus_message_new_method_call(
                        agent->systemd_dbus,
                        &req->message,
                        SYSTEMD_BUS_NAME,
                        SYSTEMD_OBJECT_PATH,
                        SYSTEMD_MANAGER_IFACE,
                        method);
        if (r < 0) {
                return NULL;
        }

        return steal_pointer(&req);
}

static void systemd_request_set_userdata(SystemdRequest *req, void *userdata, free_func_t free_userdata) {
        req->userdata = userdata;
        req->free_userdata = free_userdata;
}

static bool systemd_request_start(SystemdRequest *req, sd_bus_message_handler_t callback) {
        Agent *agent = req->agent;

        int r = sd_bus_call_async(
                        agent->systemd_dbus, &req->slot, req->message, callback, req, HIRTE_DEFAULT_DBUS_TIMEOUT);
        if (r < 0) {
                fprintf(stderr, "Failed to call async: %s\n", strerror(-r));
                return false;
        }

        systemd_request_ref(req); /* outstanding callback owns this ref */
        return true;
}


Agent *agent_new(void) {
        int r = 0;
        _cleanup_sd_event_ sd_event *event = NULL;
        r = sd_event_default(&event);
        if (r < 0) {
                fprintf(stderr, "Failed to create event loop: %s\n", strerror(-r));
                return NULL;
        }

        _cleanup_free_ char *service_name = strdup(HIRTE_AGENT_DBUS_NAME);
        if (service_name == NULL) {
                fprintf(stderr, "Out of memory\n");
                return NULL;
        }

        Agent *n = malloc0(sizeof(Agent));
        n->ref_count = 1;
        n->event = steal_pointer(&event);
        n->user_bus_service_name = steal_pointer(&service_name);
        n->port = HIRTE_DEFAULT_PORT;
        LIST_HEAD_INIT(n->outstanding_requests);
        LIST_HEAD_INIT(n->tracked_jobs);

        return n;
}

Agent *agent_ref(Agent *agent) {
        agent->ref_count++;
        return agent;
}

void agent_unref(Agent *agent) {
        agent->ref_count--;
        if (agent->ref_count != 0) {
                return;
        }

        free(agent->name);
        free(agent->host);
        free(agent->orch_addr);
        free(agent->user_bus_service_name);

        if (agent->event != NULL) {
                sd_event_unrefp(&agent->event);
        }

        if (agent->peer_dbus != NULL) {
                sd_bus_unrefp(&agent->peer_dbus);
        }
        if (agent->user_dbus != NULL) {
                sd_bus_unrefp(&agent->user_dbus);
        }
        if (agent->systemd_dbus != NULL) {
                sd_bus_unrefp(&agent->systemd_dbus);
        }

        free(agent);
}

void agent_unrefp(Agent **agentp) {
        if (agentp && *agentp) {
                agent_unref(*agentp);
                *agentp = NULL;
        }
}

bool agent_set_port(Agent *agent, const char *port_s) {
        uint16_t port = 0;

        if (!parse_port(port_s, &port)) {
                fprintf(stderr, "Invalid port format '%s'\n", port_s);
                return false;
        }
        agent->port = port;
        return true;
}

bool agent_set_host(Agent *agent, const char *host) {
        char *dup = strdup(host);
        if (dup == NULL) {
                fprintf(stderr, "Out of memory\n");
                return false;
        }
        free(agent->host);
        agent->host = dup;
        return true;
}

bool agent_set_name(Agent *agent, const char *name) {
        char *dup = strdup(name);
        if (dup == NULL) {
                fprintf(stderr, "Out of memory\n");
                return false;
        }

        free(agent->name);
        agent->name = dup;
        return true;
}

bool agent_parse_config(Agent *agent, const char *configfile) {
        _cleanup_config_ config *config = NULL;
        topic *topic = NULL;
        const char *name = NULL, *host = NULL, *port = NULL;

        config = parsing_ini_file(configfile);
        if (config == NULL) {
                return false;
        }

        topic = config_lookup_topic(config, "Node");
        if (topic == NULL) {
                return true;
        }

        name = topic_lookup(topic, "Name");
        if (name) {
                if (!agent_set_name(agent, name)) {
                        return false;
                }
        }

        host = topic_lookup(topic, "Host");
        if (host) {
                if (!agent_set_host(agent, host)) {
                        return false;
                }
        }

        port = topic_lookup(topic, "Port");
        if (port) {
                if (!agent_set_port(agent, port)) {
                        return false;
                }
        }

        return true;
}

static int list_units_callback(sd_bus_message *m, void *userdata, UNUSED sd_bus_error *ret_error) {
        _cleanup_systemd_request_ SystemdRequest *req = userdata;

        if (sd_bus_message_is_method_error(m, NULL)) {
                /* Forward error */
                return sd_bus_reply_method_error(req->request_message, sd_bus_message_get_error(m));
        }

        _cleanup_sd_bus_message_ sd_bus_message *reply = NULL;

        int r = sd_bus_message_new_method_return(req->request_message, &reply);
        if (r < 0) {
                return r;
        }

        r = sd_bus_message_copy(reply, m, true);
        if (r < 0) {
                return r;
        }

        return sd_bus_message_send(reply);
}

static int agent_method_list_units(UNUSED sd_bus_message *m, void *userdata, UNUSED sd_bus_error *ret_error) {
        Agent *agent = userdata;

        _cleanup_systemd_request_ SystemdRequest *req = agent_create_request(agent, m, "ListUnits");
        if (req == NULL) {
                return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_FAILED, "Internal error");
        }

        if (!systemd_request_start(req, list_units_callback)) {
                return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_FAILED, "Internal error");
        }

        return 1;
}

typedef struct {
        Agent *agent;
        uint32_t hirte_job_id;
} StartUnitOp;

static void start_unit_op_free(StartUnitOp *op) {
        free(op);
}

static void start_unit_job_done(UNUSED sd_bus_message *m, const char *result, void *userdata) {
        StartUnitOp *op = userdata;
        Agent *agent = op->agent;

        int r = sd_bus_emit_signal(
                        agent->peer_dbus,
                        INTERNAL_AGENT_OBJECT_PATH,
                        INTERNAL_AGENT_INTERFACE,
                        "JobDone",
                        "us",
                        op->hirte_job_id,
                        result);
        if (r < 0) {
                fprintf(stderr, "Failed to emit JobDone\n");
        }
}

static int start_unit_callback(sd_bus_message *m, void *userdata, UNUSED sd_bus_error *ret_error) {
        _cleanup_systemd_request_ SystemdRequest *req = userdata;
        Agent *agent = req->agent;
        const char *job_object_path = NULL;

        if (sd_bus_message_is_method_error(m, NULL)) {
                /* Forward error */
                return sd_bus_reply_method_error(req->request_message, sd_bus_message_get_error(m));
        }

        int r = sd_bus_message_read(m, "o", &job_object_path);
        if (r < 0) {
                return sd_bus_reply_method_errorf(req->request_message, SD_BUS_ERROR_FAILED, "Internal Error");
        }

        StartUnitOp *op = steal_pointer(&req->userdata);
        if (!agent_track_job(agent, job_object_path, start_unit_job_done, op, (free_func_t) start_unit_op_free)) {
                start_unit_op_free(op);
                return sd_bus_reply_method_errorf(req->request_message, SD_BUS_ERROR_FAILED, "Internal Error");
        }

        return sd_bus_reply_method_return(req->request_message, "");
}

static int agent_method_start_unit(UNUSED sd_bus_message *m, void *userdata, UNUSED sd_bus_error *ret_error) {
        Agent *agent = userdata;
        const char *name = NULL;
        const char *mode = NULL;
        uint32_t job_id = 0;

        int r = sd_bus_message_read(m, "ssu", &name, &mode, &job_id);
        if (r < 0) {
                return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid arguments");
        }


        _cleanup_systemd_request_ SystemdRequest *req = agent_create_request(agent, m, "StartUnit");
        if (req == NULL) {
                return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_FAILED, "Internal error");
        }

        StartUnitOp *op = malloc0(sizeof(StartUnitOp));
        if (op == NULL) {
                return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_FAILED, "Internal error");
        }
        op->agent = agent;
        op->hirte_job_id = job_id;
        systemd_request_set_userdata(req, op, (free_func_t) start_unit_op_free);

        r = sd_bus_message_append(req->message, "ss", name, mode);
        if (r < 0) {
                return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_FAILED, "Internal error");
        }

        if (!systemd_request_start(req, start_unit_callback)) {
                return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_FAILED, "Internal error");
        }

        return 1;
}

static const sd_bus_vtable internal_agent_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("ListUnits", "", "a(ssssssouso)", agent_method_list_units, 0),
        SD_BUS_METHOD("StartUnit", "ssu", "", agent_method_start_unit, 0),
        SD_BUS_SIGNAL_WITH_NAMES("JobDone", "us", SD_BUS_PARAM(id) SD_BUS_PARAM(result), 0),
        SD_BUS_VTABLE_END
};


static void job_tracker_free(JobTracker *track) {
        if (track->userdata && track->free_userdata) {
                track->free_userdata(track->userdata);
        }
        free(track->job_object_path);
        free(track);
}

static void job_tracker_freep(JobTracker **trackp) {
        if (trackp && *trackp) {
                job_tracker_free(*trackp);
                *trackp = NULL;
        }
}

#define _cleanup_job_tracker_ _cleanup_(job_tracker_freep)

static bool
                agent_track_job(Agent *agent,
                                const char *job_object_path,
                                job_tracker_callback callback,
                                void *userdata,
                                free_func_t free_userdata) {
        _cleanup_job_tracker_ JobTracker *track = malloc0(sizeof(JobTracker));
        if (track == NULL) {
                return false;
        }

        track->job_object_path = strdup(job_object_path);
        if (track->job_object_path == NULL) {
                return false;
        }

        track->callback = callback;
        track->userdata = userdata;
        track->free_userdata = free_userdata;
        LIST_INIT(tracked_jobs, track);

        LIST_PREPEND(tracked_jobs, agent->tracked_jobs, steal_pointer(&track));

        return true;
}

static int agent_match_job_removed(sd_bus_message *m, void *userdata, UNUSED sd_bus_error *error) {
        Agent *agent = userdata;
        const char *job_path = NULL;
        const char *unit = NULL;
        const char *result = NULL;
        JobTracker *track = NULL, *next_track = NULL;
        uint32_t id = 0;
        int r = 0;

        r = sd_bus_message_read(m, "uoss", &id, &job_path, &unit, &result);
        if (r < 0) {
                fprintf(stderr, "Can't parse job result\n");
                return r;
        }

        (void) sd_bus_message_rewind(m, true);

        LIST_FOREACH_SAFE(tracked_jobs, track, next_track, agent->tracked_jobs) {
                if (streq(track->job_object_path, job_path)) {
                        LIST_REMOVE(tracked_jobs, agent->tracked_jobs, track);
                        track->callback(m, result, track->userdata);
                        job_tracker_free(track);
                        break;
                }
        }

        return 0;
}


bool agent_start(Agent *agent) {
        struct sockaddr_in host;
        int r = 0;
        sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_sd_bus_message_ sd_bus_message *m = NULL;

        fprintf(stdout, "Starting Agent...\n");

        if (agent == NULL) {
                return false;
        }

        memset(&host, 0, sizeof(host));
        host.sin_family = AF_INET;
        host.sin_port = htons(agent->port);

        if (agent->name == NULL) {
                fprintf(stderr, "No name specified\n");
                return false;
        }

        if (agent->host == NULL) {
                fprintf(stderr, "No host specified\n");
                return false;
        }

        r = inet_pton(AF_INET, agent->host, &host.sin_addr);
        if (r < 1) {
                fprintf(stderr, "Invalid host option '%s'\n", optarg);
                return false;
        }

        agent->orch_addr = assemble_tcp_address(&host);
        if (agent->orch_addr == NULL) {
                return false;
        }

        agent->user_dbus = user_bus_open(agent->event);
        if (agent->user_dbus == NULL) {
                fprintf(stderr, "Failed to open user dbus\n");
                return false;
        }

        r = sd_bus_request_name(agent->user_dbus, agent->user_bus_service_name, SD_BUS_NAME_REPLACE_EXISTING);
        if (r < 0) {
                fprintf(stderr, "Failed to acquire service name on user dbus: %s\n", strerror(-r));
                return false;
        }

        agent->systemd_dbus = systemd_bus_open(agent->event);
        if (agent->systemd_dbus == NULL) {
                fprintf(stderr, "Failed to open systemd dbus\n");
                return false;
        }

        r = sd_bus_match_signal(
                        agent->systemd_dbus,
                        NULL,
                        SYSTEMD_BUS_NAME,
                        SYSTEMD_OBJECT_PATH,
                        SYSTEMD_MANAGER_IFACE,
                        "JobRemoved",
                        agent_match_job_removed,
                        agent);
        if (r < 0) {
                fprintf(stderr, "Failed to add job-removed peer bus match: %s\n", strerror(-r));
                return false;
        }

        agent->peer_dbus = peer_bus_open(agent->event, "peer-bus-to-orchestrator", agent->orch_addr);
        if (agent->peer_dbus == NULL) {
                fprintf(stderr, "Failed to open peer dbus\n");
                return false;
        }

        r = sd_bus_add_object_vtable(
                        agent->peer_dbus,
                        NULL,
                        INTERNAL_AGENT_OBJECT_PATH,
                        INTERNAL_AGENT_INTERFACE,
                        internal_agent_vtable,
                        agent);
        if (r < 0) {
                fprintf(stderr, "Failed to add manager vtable: %s\n", strerror(-r));
                return false;
        }

        r = sd_bus_call_method(
                        agent->peer_dbus,
                        HIRTE_DBUS_NAME,
                        INTERNAL_MANAGER_OBJECT_PATH,
                        INTERNAL_MANAGER_INTERFACE,
                        "Register",
                        &error,
                        &m,
                        "s",
                        agent->name);
        if (r < 0) {
                fprintf(stderr, "Failed to issue method call: %s\n", error.message);
                sd_bus_error_free(&error);
                return false;
        }

        r = sd_bus_message_read(m, "");
        if (r < 0) {
                fprintf(stderr, "Failed to parse response message: %s\n", strerror(-r));
                return false;
        }

        printf("Registered as '%s'\n", agent->name);

        r = shutdown_service_register(agent->user_dbus, agent->event);
        if (r < 0) {
                fprintf(stderr, "Failed to register shutdown service\n");
                return false;
        }

        r = event_loop_add_shutdown_signals(agent->event);
        if (r < 0) {
                fprintf(stderr, "Failed to add signals to agent event loop\n");
                return false;
        }

        r = sd_event_loop(agent->event);
        if (r < 0) {
                fprintf(stderr, "Starting event loop failed: %s\n", strerror(-r));
                return false;
        }

        return true;
}

bool agent_stop(Agent *agent) {
        fprintf(stdout, "Stopping Agent...\n");

        if (agent == NULL) {
                return false;
        }

        return true;
}