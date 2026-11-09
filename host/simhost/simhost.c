// simhost: starts launchd_sim under Darling the way CoreSimulator and the host
// launchd do on a Mac.
//
//   simhost <launchd_sim> <args...>
//
// - launchd_sim's bootstrap port is a port served here. Its first request
//   (MIG id 708) asks for the receive right of the port it will serve the
//   simulator domain on; we create one, keep a send right, and move it over.
// - XPC_SIMULATOR_HOLDING_TANK_HACK names a child launchd_sim reaps with
//   waitpid(), so it is forked before exec and exits at once.
// Other requests are logged and dropped.
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>

#define SIM_GET_SERVICE_PORT 708

struct service_port_reply
{
	mach_msg_header_t header;
	mach_msg_body_t body;
	mach_msg_port_descriptor_t port;
};

static mach_port_t sim_service_port = MACH_PORT_NULL; // send right to launchd_sim's domain port

static void* wait_child(void* arg)
{
	pid_t pid = (pid_t)(intptr_t) arg;
	int status = 0;

	while (waitpid(pid, &status, 0) < 0)
		;
	if (WIFEXITED(status))
		fprintf(stderr, "simhost: launchd_sim exited with %d\n", WEXITSTATUS(status));
	else
		fprintf(stderr, "simhost: launchd_sim killed by signal %d\n", WTERMSIG(status));
	exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}

static void reply_service_port(mach_msg_header_t* request)
{
	struct service_port_reply reply = {};
	mach_port_t port;
	kern_return_t kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
	if (kr == KERN_SUCCESS)
		kr = mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS)
	{
		fprintf(stderr, "simhost: cannot make the service port: %d\n", kr);
		return;
	}
	sim_service_port = port;

	reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(request->msgh_bits), 0) | MACH_MSGH_BITS_COMPLEX;
	reply.header.msgh_size = sizeof(reply);
	reply.header.msgh_remote_port = request->msgh_remote_port;
	reply.header.msgh_id = request->msgh_id + 100;
	reply.body.msgh_descriptor_count = 1;
	reply.port.name = port;
	reply.port.disposition = MACH_MSG_TYPE_MOVE_RECEIVE;
	reply.port.type = MACH_MSG_PORT_DESCRIPTOR;

	kr = mach_msg(&reply.header, MACH_SEND_MSG, sizeof(reply), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	fprintf(stderr, "simhost: sent service port 0x%x: %d\n", port, kr);
}

int main(int argc, char** argv)
{
	mach_port_t bootstrap;
	pid_t pid;
	pthread_t waiter;

	if (argc < 2)
	{
		fprintf(stderr, "usage: simhost <launchd_sim> <args...>\n");
		return 2;
	}

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &bootstrap) != KERN_SUCCESS
		|| mach_port_insert_right(mach_task_self(), bootstrap, bootstrap, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS)
	{
		fprintf(stderr, "simhost: cannot make the bootstrap port\n");
		return 1;
	}

	// Port names do not survive fork(), special ports do: set ours for the child to inherit
	kern_return_t kr = task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, bootstrap);
	if (kr != KERN_SUCCESS)
	{
		fprintf(stderr, "simhost: cannot set the bootstrap port: 0x%x\n", kr);
		return 1;
	}

	pid = fork();
	if (pid == 0)
	{
		char tank_pid[16];
		pid_t tank = fork();

		if (tank == 0)
			_exit(0);
		snprintf(tank_pid, sizeof(tank_pid), "%d", tank);
		setenv("XPC_SIMULATOR_HOLDING_TANK_HACK", tank_pid, 1);

		execv(argv[1], argv + 1);
		perror("simhost: exec");
		_exit(127);
	}
	if (pid < 0)
	{
		perror("simhost: fork");
		return 1;
	}
	pthread_create(&waiter, NULL, wait_child, (void*)(intptr_t) pid);

	for (;;)
	{
		union {
			mach_msg_header_t header;
			char data[8192];
		} msg;
		kern_return_t kr = mach_msg(&msg.header, MACH_RCV_MSG | MACH_RCV_LARGE, 0, sizeof(msg), bootstrap,
			MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

		if (kr != KERN_SUCCESS)
		{
			fprintf(stderr, "simhost: receive failed: 0x%x\n", kr);
			continue;
		}
		if (msg.header.msgh_id == SIM_GET_SERVICE_PORT)
			reply_service_port(&msg.header);
		else
		{
			fprintf(stderr, "simhost: unhandled request id %d size %u\n", msg.header.msgh_id, msg.header.msgh_size);
			mach_msg_destroy(&msg.header);
		}
	}
}
