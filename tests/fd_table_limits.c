#include <darwintest.h>
#include <mach/mach.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <darwintest_multiprocess.h>
#include <spawn.h>
#include <spawn_private.h>
#include <libproc_internal.h>
#include <signal.h>
#include <string.h>

#include <err.h>
#include <stdio.h>
#include <sysexits.h>
#include <stdbool.h>

#include "rnServer.h"         // generated by MIG from rnserver.defs

#include <servers/bootstrap.h>
#include <libproc_internal.h>       // proc*cpumon*()

T_GLOBAL_META(
	T_META_NAMESPACE("xnu.fd"),
	T_META_RUN_CONCURRENTLY(TRUE),
	T_META_RADAR_COMPONENT_NAME("xnu"),
	T_META_RADAR_COMPONENT_VERSION("file descriptors"));

#define MAX_ARGV 5

extern char **environ;
static mach_port_t resource_notify_port = MACH_PORT_NULL;

T_DECL(test_fd_table_set_soft_limit, "Allocate fds upto soft limit", T_META_IGNORECRASHES(".*fd_table_limits_client.*"), T_META_CHECK_LEAKS(false))
{
	char *test_prog_name = "./fd_table_limits_client";
	char *child_args[MAX_ARGV];
	int child_pid;
	posix_spawnattr_t       attrs;
	int err;

#if __arm__ || TARGET_OS_BRIDGE
	T_SKIP("Not running on target platforms");
#endif /* __arm__ || TARGET_OS_BRIDGE */

	/* Initialize posix_spawn attributes */
	posix_spawnattr_init(&attrs);

	err = posix_spawnattr_set_filedesclimit_ext(&attrs, 200, 0);
	T_EXPECT_POSIX_SUCCESS(err, "posix_spawnattr_set_filedesclimit_ext");

	child_args[0] = test_prog_name;
	child_args[1] = "200"; //soft limit
	child_args[2] = "0"; //hard limit
	child_args[3] = "1"; //test num
	child_args[4] = NULL;

	err = posix_spawn(&child_pid, child_args[0], NULL, &attrs, child_args, environ);
	T_EXPECT_POSIX_SUCCESS(err, "posix_spawn fd_table_limits_client");

	int child_status;
	/* Wait for child and check for exception */

	if (-1 == waitpid(child_pid, &child_status, 0)) {
		T_FAIL("waitpid: child mia");
	}

	if (WIFSIGNALED(child_status)) {
		T_FAIL("Child exited with signal = %d", WTERMSIG(child_status));
	}

	T_ASSERT_EQ(WIFEXITED(child_status), 1, "Child exited normally with exit value %d", WEXITSTATUS(child_status));
}

T_DECL(test_fd_table_set_hard_limit, "Allocate fds upto hard limit", T_META_IGNORECRASHES(".*fd_table_limits_client.*"), T_META_CHECK_LEAKS(false))
{
	char *test_prog_name = "./fd_table_limits_client";
	char *child_args[MAX_ARGV];
	int child_pid;
	posix_spawnattr_t       attrs;
	int err;

#if __arm__ || TARGET_OS_BRIDGE
	T_SKIP("Not running on target platforms");
#endif /* __arm__ || TARGET_OS_BRIDGE */

	/* Initialize posix_spawn attributes */
	posix_spawnattr_init(&attrs);

	err = posix_spawnattr_set_filedesclimit_ext(&attrs, 0, 500);
	T_EXPECT_POSIX_SUCCESS(err, "posix_spawnattr_set_filedesclimit_ext");

	child_args[0] = test_prog_name;
	child_args[1] = "0"; //soft limit
	child_args[2] = "500"; //hard limit
	child_args[3] = "1"; //test num
	child_args[4] = NULL;

	err = posix_spawn(&child_pid, child_args[0], NULL, &attrs, child_args, environ);
	T_EXPECT_POSIX_SUCCESS(err, "posix_spawn fd_table_limits_client");

	int child_status;
	/* Wait for child and check for exception */

	if (-1 == waitpid(child_pid, &child_status, 0)) {
		T_FAIL("waitpid: child mia");
	}

	T_ASSERT_EQ(WIFEXITED(child_status), 0, "Child did not exit normally");

	if (WIFSIGNALED(child_status)) {
		T_ASSERT_EQ(child_status, 9, "Child exited with status = %x", child_status);
	}
}

T_DECL(test_fd_table_setting_limits, "Allocate fds - both soft & hard limit", T_META_IGNORECRASHES(".*fd_table_limits_client.*"), T_META_CHECK_LEAKS(false))
{
	char *test_prog_name = "./fd_table_limits_client";
	char *child_args[MAX_ARGV];
	int child_pid;
	posix_spawnattr_t       attrs;
	int err;

#if __arm__ || TARGET_OS_BRIDGE
	T_SKIP("Not running on target platforms");
#endif /* __arm__ || TARGET_OS_BRIDGE */

	/* Initialize posix_spawn attributes */
	posix_spawnattr_init(&attrs);

	err = posix_spawnattr_set_filedesclimit_ext(&attrs, 400, 800);
	T_EXPECT_POSIX_SUCCESS(err, "posix_spawnattr_set_filedesclimit_ext");

	child_args[0] = test_prog_name;
	child_args[1] = "400"; //soft limit
	child_args[2] = "800"; //hard limit
	child_args[3] = "1"; //test num
	child_args[4] = NULL;

	err = posix_spawn(&child_pid, child_args[0], NULL, &attrs, child_args, environ);
	T_EXPECT_POSIX_SUCCESS(err, "posix_spawn fd_table_limits_client");

	int child_status;
	/* Wait for child and check for exception */

	if (-1 == waitpid(child_pid, &child_status, 0)) {
		T_FAIL("waitpid: child mia");
	}

	T_ASSERT_EQ(WIFEXITED(child_status), 0, "Child did not exit normally");

	if (WIFSIGNALED(child_status)) {
		T_ASSERT_EQ(child_status, 9, "Child exited with status = %x", child_status);
	}
}

typedef struct {
	mach_msg_header_t   header;
	mach_msg_body_t     body;
	mach_msg_port_descriptor_t port_descriptor;
	mach_msg_trailer_t  trailer;            // subtract this when sending
} ipc_complex_message;

struct args {
	const char *progname;
	int verbose;
	int voucher;
	int num_msgs;
	const char *server_port_name;
	mach_port_t server_port;
	mach_port_t reply_port;
	int request_msg_size;
	void *request_msg;
	int reply_msg_size;
	void *reply_msg;
	uint32_t persona_id;
	long client_pid;
};

void parse_args(struct args *args);
void server_setup(struct args* args);
void* exception_server_thread(void *arg);
mach_port_t create_exception_port(void);
static mach_port_t create_resource_notify_port(void);
static ipc_complex_message icm_request = {};
static ipc_complex_message icm_reply = {};

#define TEST_TIMEOUT    10

void
parse_args(struct args *args)
{
	args->server_port_name = "TEST_FD_TABLE_LIMITS";
	args->server_port = MACH_PORT_NULL;
	args->reply_msg_size = sizeof(ipc_complex_message) - sizeof(mach_msg_trailer_t);
	args->request_msg_size = sizeof(ipc_complex_message) - sizeof(mach_msg_trailer_t);
	args->reply_msg_size = sizeof(ipc_complex_message) - sizeof(mach_msg_trailer_t);
	args->request_msg = &icm_request;
	args->reply_msg = &icm_reply;
}

/* Create a mach IPC listener which will respond to the client's message */
void
server_setup(struct args *args)
{
	kern_return_t ret;
	mach_port_t bsport;

	ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	    &args->server_port);
	T_ASSERT_MACH_SUCCESS(ret, "server: mach_port_allocate()");

	ret = mach_port_insert_right(mach_task_self(), args->server_port, args->server_port,
	    MACH_MSG_TYPE_MAKE_SEND);
	T_ASSERT_MACH_SUCCESS(ret, "server: mach_port_insert_right()");

	ret = task_get_bootstrap_port(mach_task_self(), &bsport);
	T_ASSERT_MACH_SUCCESS(ret, "server: task_get_bootstrap_port()");

	ret = bootstrap_register(bsport, (const char *)args->server_port_name, args->server_port);
	T_ASSERT_MACH_SUCCESS(ret, "server: bootstrap_register()");

	T_LOG("server: waiting for IPC messages from client on port '%s'.\n",
	    args->server_port_name);

	/* Make the server port as the resource notify port */
	resource_notify_port = args->server_port;
}

T_DECL(test_fd_table_hard_limit_with_resource_notify_port, "Allocate ports upto hard limit and trigger notification", T_META_IGNORECRASHES(".*fd_table_limits_client.*"), T_META_CHECK_LEAKS(false))
{
	char *test_prog_name = "./fd_table_limits_client";
	char *child_args[MAX_ARGV];
	int child_pid;
	posix_spawnattr_t       attrs;
	int err;
	kern_return_t kr;
	struct args*            server_args = (struct args*)malloc(sizeof(struct args));

#if __arm__ || TARGET_OS_BRIDGE
	T_SKIP("Not running on target platforms");
#endif /* __arm__ || TARGET_OS_BRIDGE */

	/* Create the bootstrap port */
	parse_args(server_args);
	server_setup(server_args);

/*
 *       dt_helper_t helpers[] = {
 *               dt_launchd_helper_domain("com.apple.xnu.test.mach_port.plist",
 *           "right_dedup_server", NULL, LAUNCH_SYSTEM_DOMAIN),
 *               dt_fork_helper("right_dedup_client"),
 *       };
 *       dt_run_helpers(helpers, 2, 600);
 */

	/* Initialize posix_spawn attributes */
	posix_spawnattr_init(&attrs);
	err = posix_spawnattr_set_filedesclimit_ext(&attrs, 0, 500);
	T_ASSERT_POSIX_SUCCESS(err, "posix_spawnattr_set_filedesclimit_ext");

	child_args[0] = test_prog_name;
	child_args[1] = "0"; // soft limit
	child_args[2] = "500"; // hard limit
	child_args[3] = "2"; // test num
	child_args[4] = NULL;

	err = posix_spawn(&child_pid, child_args[0], NULL, &attrs, &child_args[0], environ);
	T_ASSERT_POSIX_SUCCESS(err, "posix_spawn fd_table_limits_client");

	server_args->client_pid = child_pid;

	T_LOG("server: Let's see if we can catch some fd leak");
	/*
	 * Recover the service port because the port must have been destroyed and sent the notification by now
	 */
	kr = mach_msg_server_once(resource_notify_server, 4096, resource_notify_port, 0);
	T_ASSERT_MACH_SUCCESS(kr, "mach_msg_server_once resource_notify_port");
}

// MIG's resource_notify_server() expects receive_cpu_usage_trigger()
// This must match the definition in xnu's resource_notify.defs
kern_return_t
receive_cpu_usage_violation(__unused mach_port_t receiver,
    __unused proc_name_t procname,
    __unused pid_t pid,
    __unused posix_path_t killed_proc_path,
    __unused mach_timespec_t timestamp,
    __unused int64_t observed_cpu_nsecs,
    __unused int64_t observation_nsecs,
    __unused int64_t cpu_nsecs_allowed,
    __unused int64_t limit_window_nsecs,
    __unused resource_notify_flags_t flags)
{
	return KERN_FAILURE;
}

kern_return_t
receive_cpu_wakes_violation(__unused mach_port_t receiver,
    __unused proc_name_t procname,
    __unused pid_t pid,
    __unused posix_path_t killed_proc_path,
    __unused mach_timespec_t timestamp,
    __unused int64_t observed_cpu_wakes,
    __unused int64_t observation_nsecs,
    __unused int64_t cpu_wakes_allowed,
    __unused int64_t limit_window_nsecs,
    __unused resource_notify_flags_t flags)
{
	return KERN_FAILURE;
}

kern_return_t
receive_disk_writes_violation(__unused mach_port_t receiver,
    __unused proc_name_t procname,
    __unused pid_t pid,
    __unused posix_path_t killed_proc_path,
    __unused mach_timespec_t timestamp,
    __unused int64_t observed_bytes_dirtied,
    __unused int64_t observation_nsecs,
    __unused int64_t bytes_dirtied_allowed,
    __unused int64_t limit_window_nsecs,
    __unused resource_notify_flags_t flags)
{
	return KERN_FAILURE;
}

kern_return_t
receive_port_space_violation(__unused mach_port_t receiver,
    __unused proc_name_t procname,
    __unused pid_t pid,
    __unused mach_timespec_t timestamp,
    __unused int64_t observed_ports,
    __unused int64_t ports_allowed,
    __unused mach_port_t fatal_port,
    __unused resource_notify_flags_t flags)
{
	return KERN_FAILURE;
}

kern_return_t
receive_file_descriptors_violation(__unused mach_port_t receiver,
    __unused proc_name_t procname,
    __unused pid_t pid,
    __unused mach_timespec_t timestamp,
    __unused int64_t observed_filedesc,
    __unused int64_t filedesc_allowed,
    __unused mach_port_t fatal_port,
    __unused resource_notify_flags_t flags)
{
	T_LOG("Received a notification on the resource notify port");
	T_LOG("filedesc_allowed = %lld, observed_filedesc = %lld", filedesc_allowed, observed_filedesc);
	if (fatal_port) {
		mach_port_deallocate(mach_task_self(), fatal_port);
	}

	return KERN_SUCCESS;
}