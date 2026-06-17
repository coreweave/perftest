/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies Ltd.  All rights reserved.
 * Copyright (c) 2009 HNR Consulting.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * $Id$
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "perftest_parameters.h"
#include "perftest_resources.h"
#include "perftest_communication.h"

/******************************************************************************
 ******************************************************************************/
/* run_job (FMT-1267): tear down + recreate the QP and re-establish the
 * connection between in-process jobs, keeping the warm device/PD/MR/CQ/CUDA
 * (ibv_reg_mr is NOT called again). Mirrors the initial connect sequence in
 * main(); plain RC path only (no DC/XRC/event special-casing for the spike). */
static int g_agent_fd = 3;

static int reestablish_qp(struct pingpong_context *ctx,
			  struct perftest_parameters *user_param,
			  struct perftest_comm *user_comm,
			  struct pingpong_dest *my_dest,
			  struct pingpong_dest *rem_dest)
{
	int i;
	struct ibv_wc wc;
	for (i = 0; i < user_param->num_of_qps; i++) {
		if (ctx->qp[i] && ibv_destroy_qp(ctx->qp[i])) {
			fprintf(stderr, "reestablish: ibv_destroy_qp %d failed\n", i);
			return FAILURE;
		}
		ctx->qp[i] = NULL;
	}
	while (ibv_poll_cq(ctx->send_cq, 1, &wc) > 0) {}
	if (ctx->recv_cq)
		while (ibv_poll_cq(ctx->recv_cq, 1, &wc) > 0) {}
	if (ctx->scnt)
		memset(ctx->scnt, 0, sizeof(uint64_t) * user_param->num_of_qps);
	if (ctx->ccnt)
		memset(ctx->ccnt, 0, sizeof(uint64_t) * user_param->num_of_qps);
	user_comm->rdma_params->port = user_param->port;
	if (user_comm->rdma_params->sockfd >= 0) {
		close(user_comm->rdma_params->sockfd);
		user_comm->rdma_params->sockfd = -1;
	}
	{
		/* FMT-1267: agent client may dial before the server reaches listen()
		 * (ready is signalled earlier) -- retry the OOB connect on the client
		 * rather than failing on a transient ECONNREFUSED. */
		int _oob_tries = (user_param->machine == CLIENT) ? 200 : 1;
		int _oob_t, _oob_ok = 0;
		for (_oob_t = 0; _oob_t < _oob_tries; _oob_t++) {
			if (!establish_connection(user_comm)) { _oob_ok = 1; break; }
			usleep(25000);
		}
		if (!_oob_ok)
			return FAILURE;
	}
	if (check_mtu(ctx->context, user_param, user_comm))
		return FAILURE;
	for (i = 0; i < user_param->num_of_qps; i++) {
		if (create_qp_main(ctx, user_param, i)) {
			fprintf(stderr, "reestablish: create_qp_main %d failed\n", i);
			return FAILURE;
		}
		if (user_param->work_rdma_cm == OFF)
			modify_qp_to_init(ctx, user_param, i);
	}
	if (set_up_connection(ctx, user_param, my_dest))
		return FAILURE;
	for (i = 0; i < user_param->num_of_qps; i++)
		if (ctx_hand_shake(user_comm, &my_dest[i], &rem_dest[i]))
			return FAILURE;
	if (user_param->work_rdma_cm == OFF)
		if (ctx_connect(ctx, rem_dest, user_param, my_dest))
			return FAILURE;
	user_comm->rdma_params->side = REMOTE;
	for (i = 0; i < user_param->num_of_qps; i++)
		if (ctx_hand_shake(user_comm, &my_dest[i], &rem_dest[i]))
			return FAILURE;
	if (ctx_hand_shake(user_comm, &my_dest[0], &rem_dest[0]))
		return FAILURE;
	return SUCCESS;
}

static int run_one_job(struct pingpong_context *ctx, struct perftest_parameters *user_param,
		       struct perftest_comm *user_comm, struct pingpong_dest *my_dest,
		       struct pingpong_dest *rem_dest, struct bw_report_data *my_bw_rep,
		       struct bw_report_data *rem_bw_rep)
{
	if (reestablish_qp(ctx, user_param, user_comm, my_dest, rem_dest))
		return FAILURE;
	if (user_param->machine == CLIENT || user_param->duplex)
		ctx_set_send_wqes(ctx, user_param, rem_dest);
	if (user_param->duplex)
		if (ctx_hand_shake(user_comm, &my_dest[0], &rem_dest[0]))
			return FAILURE;
	if (user_param->machine == CLIENT || user_param->duplex) {
		if (run_iter_bw(ctx, user_param))
			return FAILURE;
		print_report_bw(user_param, my_bw_rep);
	}
	/* non-duplex WRITE: client measured, server was a passive target; both now
	 * sync (client signals done / server waits) and exchange the report. */
	if (!user_param->duplex)
		if (ctx_hand_shake(user_comm, &my_dest[0], &rem_dest[0]))
			return FAILURE;
	xchg_bw_reports(user_comm, my_bw_rep, rem_bw_rep, atof(user_param->rem_version));
	if (user_comm->rdma_params->sockfd >= 0) {
		close(user_comm->rdma_params->sockfd);
		user_comm->rdma_params->sockfd = -1;
	}
	return SUCCESS;
}

/* agent_repl (FMT-1267): role-pure REPL. Warm device/PD/MR/CQ/CUDA + OOB socket
 * are already up; per stdin job, run_one_job recreates the QP (warm MR reused)
 * and runs the fixed role, emitting a result on fd 3 (client = measurement,
 * server = bare ack). "shutdown" -> bye. */
static void agent_repl(struct pingpong_context *ctx, struct perftest_parameters *user_param,
		       struct perftest_comm *user_comm, struct pingpong_dest *my_dest,
		       struct pingpong_dest *rem_dest, struct bw_report_data *my_bw_rep,
		       struct bw_report_data *rem_bw_rep)
{
	char line[4096];
	const char *role = (user_param->machine == CLIENT) ? "client" : "server";
	while (fgets(line, sizeof(line), stdin)) {
		char *q, *sq;
		long jid;
		if (strstr(line, "\"type\":\"shutdown\""))
			break;
		if (!strstr(line, "\"type\":\"job\""))
			continue;
		q = strstr(line, "\"id\":");
		jid = q ? atol(q + 5) : 0;
		sq = strstr(line, "\"size\":");
		if (sq)
			user_param->size = (unsigned long)atol(sq + 7);
		{ char *pq = strstr(line, "\"port\":"); if (pq) user_param->port = atoi(pq + 7); }
		if (user_param->machine == SERVER)
			dprintf(g_agent_fd, "{\"v\":1,\"type\":\"ready\",\"id\":%ld}\n", jid);
		if (run_one_job(ctx, user_param, user_comm, my_dest, rem_dest, my_bw_rep, rem_bw_rep)) {
			dprintf(g_agent_fd, "{\"v\":1,\"type\":\"result\",\"id\":%ld,\"role\":\"%s\",\"status\":\"failed\",\"fault\":{\"reason\":\"qp_setup_failed\"}}\n", jid, role);
			break;
		}
		if (user_param->machine == CLIENT)
			dprintf(g_agent_fd, "{\"v\":1,\"type\":\"result\",\"id\":%ld,\"role\":\"client\",\"status\":\"ok\",\"verb\":\"write_bw\",\"size\":%lu,\"measurement\":{\"bw_peak_gbps\":%.2f,\"bw_avg_gbps\":%.2f,\"bw_min_gbps\":%.2f,\"msg_rate_mpps\":%.6f}}\n",
				jid, (unsigned long)my_bw_rep->size, my_bw_rep->bw_peak, my_bw_rep->bw_avg, my_bw_rep->bw_min, my_bw_rep->msgRate_avg);
		else
			dprintf(g_agent_fd, "{\"v\":1,\"type\":\"result\",\"id\":%ld,\"role\":\"server\",\"status\":\"ok\"}\n", jid);
	}
	dprintf(g_agent_fd, "{\"v\":1,\"type\":\"bye\"}\n");
}

int main(int argc, char *argv[])
{
	int				ret_parser, i = 0, rc;
	struct ibv_device		*ib_dev = NULL;
	struct pingpong_context		ctx;
	struct pingpong_dest		*my_dest,*rem_dest;
	struct perftest_parameters	user_param;
	struct perftest_comm		user_comm;
	struct bw_report_data		my_bw_rep, rem_bw_rep;
	int rdma_cm_flow_destroyed = 0;

	/* FMT-1267: in agent mode, line-buffer stdout and unbuffer stderr so a
	 * hung (still-alive) agent's diagnostics reach the captured logfile for
	 * triage instead of sitting in a block buffer until exit. */
	if (getenv("PERFTEST_AGENT")) {
		setvbuf(stdout, NULL, _IOLBF, 0);
		setvbuf(stderr, NULL, _IONBF, 0);
	}

	/* init default values to user's parameters */
	memset(&user_param,0,sizeof(struct perftest_parameters));
	memset(&user_comm,0,sizeof(struct perftest_comm));
	memset(&ctx,0,sizeof(struct pingpong_context));

	user_param.verb    = WRITE;
	user_param.tst     = BW;
	strncpy(user_param.version, VERSION, sizeof(user_param.version));

	/* Configure the parameters values according to user arguments or default values. */
	ret_parser = parser(&user_param,argv,argc);
	if (ret_parser) {
		if (ret_parser != VERSION_EXIT && ret_parser != HELP_EXIT)
			fprintf(stderr," Parser function exited with Error\n");
		goto return_error;
	}

	if((user_param.connection_type == DC || user_param.use_xrc) && user_param.duplex) {
		user_param.num_of_qps *= 2;
	}

	/* Finding the IB device selected (or default if none is selected). */
	ib_dev = ctx_find_dev(&user_param.ib_devname);
	if (!ib_dev) {
		fprintf(stderr," Unable to find the Infiniband/RoCE device\n");
		goto return_error;
	}

	/* Set the affinity to the CPU cores or NUMA node */
	if (set_process_affinity(ib_dev->ibdev_path, &user_param)) {
		goto return_error;
	}

	/* Getting the relevant context from the device */
	ctx.context = ctx_open_device(ib_dev, &user_param);
	if (!ctx.context) {
		fprintf(stderr, " Couldn't get context for the device\n");
		goto free_devname;
	}

	/* Verify user parameters that require the device context,
	 * the function will print the relevent error info. */
	if (verify_params_with_device_context(ctx.context, &user_param))
	{
		fprintf(stderr, " Couldn't get context for the device\n");
		goto free_devname;
	}

	/* See if link type is valid and supported. */
	if (check_link(ctx.context,&user_param)) {
		fprintf(stderr, " Couldn't get context for the device\n");
		goto free_devname;
	}

	/* copy the relevant user parameters to the comm struct + creating rdma_cm resources. */
	if (create_comm_struct(&user_comm,&user_param)) {
		fprintf(stderr," Unable to create RDMA_CM resources\n");
		goto free_devname;
	}

	if (user_param.output == FULL_VERBOSITY && user_param.machine == SERVER) {
		printf("\n************************************\n");
		printf("* Waiting for client to connect... *\n");
		printf("************************************\n");
	}

	/* Initialize the connection and print the local data. */
	if (!getenv("PERFTEST_AGENT")) {
	if (establish_connection(&user_comm)) {
		fprintf(stderr," Unable to init the socket connection\n");
		dealloc_comm_struct(&user_comm,&user_param);
		goto free_devname;
	}
	sleep(1);
	exchange_versions(&user_comm, &user_param);
	check_version_compatibility(&user_param);
	check_sys_data(&user_comm, &user_param);

	/* See if MTU is valid and supported. */
	if (check_mtu(ctx.context,&user_param, &user_comm)) {
		fprintf(stderr, " Couldn't get context for the device\n");
		dealloc_comm_struct(&user_comm,&user_param);
		goto free_devname;
	}
	}

	MAIN_ALLOC(my_dest , struct pingpong_dest , user_param.num_of_qps , free_rdma_params);
	memset(my_dest, 0, sizeof(struct pingpong_dest)*user_param.num_of_qps);
	MAIN_ALLOC(rem_dest , struct pingpong_dest , user_param.num_of_qps , free_my_dest);
	memset(rem_dest, 0, sizeof(struct pingpong_dest)*user_param.num_of_qps);

	/* Allocating arrays needed for the test. */
	if(alloc_ctx(&ctx,&user_param)){
		fprintf(stderr, "Couldn't allocate context\n");
		goto free_mem;
	}

	/* Negotiate parameters. */
	if (!getenv("PERFTEST_AGENT")) {
	if (negotiate_params(&ctx, &user_comm, &user_param)) {
		fprintf(stderr, " Failed to negotiate parameters\n");
		dealloc_ctx(&ctx, &user_param);
		goto free_mem;
	}
	}

	/* Create RDMA CM resources and connect through CM. */
	if (user_param.work_rdma_cm == ON) {
		rc = create_rdma_cm_connection(&ctx, &user_param, &user_comm,
			my_dest, rem_dest);
		if (rc) {
			fprintf(stderr,
				"Failed to create RDMA CM connection with resources.\n");
			dealloc_ctx(&ctx, &user_param);
			goto free_mem;
		}
	} else {
		/* create all the basic IB resources (data buffer, PD, MR, CQ and events channel) */
		if (ctx_init(&ctx, &user_param)) {
			fprintf(stderr, " Couldn't create IB resources\n");
			dealloc_ctx(&ctx, &user_param);
			goto free_mem;
		}
	}

	if (getenv("PERFTEST_AGENT")) {
		g_agent_fd = getenv("AGENT_CONTROL_OUT_FD") ? atoi(getenv("AGENT_CONTROL_OUT_FD")) : 3;
		dprintf(g_agent_fd, "{\"v\":1,\"type\":\"hello\",\"device\":\"%s\",\"verb\":\"write_bw\",\"gdr_capable\":true,\"pid\":%d}\n",
			user_param.ib_devname ? user_param.ib_devname : "", (int)getpid());
		user_comm.rdma_params->sockfd = -1;
		agent_repl(&ctx, &user_param, &user_comm, my_dest, rem_dest, &my_bw_rep, &rem_bw_rep);
		goto destroy_context;
	}

	/* Initialize data validation for receiver side */
	if (user_param.data_validation &&
		(user_param.machine == SERVER || user_param.duplex) &&
		data_validation_init(&ctx, &user_param)) {
		goto destroy_context;
	}

	/* Set up the Connection. */
	if (set_up_connection(&ctx,&user_param,my_dest)) {
		fprintf(stderr," Unable to set up socket connection\n");
		goto destroy_context;
	}

	/* Print basic test information. */
	ctx_print_test_info(&user_param);

	for (i=0; i < user_param.num_of_qps; i++) {

		if (ctx_hand_shake(&user_comm,&my_dest[i],&rem_dest[i])) {
			fprintf(stderr," Failed to exchange data between server and clients\n");
			goto destroy_context;
		}
	}

	if (user_param.work_rdma_cm == OFF) {
		if (ctx_check_gid_compatibility(&my_dest[0], &rem_dest[0])) {
			fprintf(stderr,"\n Found Incompatibility issue with GID types.\n");
			fprintf(stderr," Please Try to use a different IP version.\n\n");
			goto destroy_context;
		}
	}

	if (user_param.work_rdma_cm == OFF) {
		if (ctx_connect(&ctx,rem_dest,&user_param,my_dest)) {
			fprintf(stderr," Unable to Connect the HCA's through the link\n");
			goto destroy_context;
		}
	}

	if (user_param.connection_type == DC)
	{
		/* Set up connection one more time to send qpn properly for DC */
		if (set_up_connection(&ctx, &user_param, my_dest))
		{
			fprintf(stderr," Unable to set up socket connection\n");
			goto destroy_context;
		}
	}

	/* Print this machine QP information */
	for (i=0; i < user_param.num_of_qps; i++)
		ctx_print_pingpong_data(&my_dest[i],&user_comm);

	user_comm.rdma_params->side = REMOTE;

	for (i=0; i < user_param.num_of_qps; i++) {
		if (ctx_hand_shake(&user_comm,&my_dest[i],&rem_dest[i])) {
			fprintf(stderr," Failed to exchange data between server and clients\n");
			goto destroy_context;
		}

		ctx_print_pingpong_data(&rem_dest[i],&user_comm);
	}

	if (user_param.use_event) {
		if (ibv_req_notify_cq(ctx.send_cq, 0)) {
			fprintf(stderr, " Couldn't request CQ notification\n");
			goto destroy_context;
		}
		if (ibv_req_notify_cq(ctx.recv_cq, 0)) {
			fprintf(stderr, " Couldn't request CQ notification\n");
			goto destroy_context;
		}
	}

	/* An additional handshake is required after moving qp to RTR. */
	if (ctx_hand_shake(&user_comm,&my_dest[0],&rem_dest[0])) {
		fprintf(stderr," Failed to exchange data between server and clients\n");
		goto destroy_context;
	}

	if (user_param.output == FULL_VERBOSITY) {
		if (user_param.report_per_port) {
			printf(RESULT_LINE_PER_PORT);
			printf((user_param.report_fmt == MBS ? RESULT_FMT_PER_PORT : RESULT_FMT_G_PER_PORT));
		}
		else {
			printf(RESULT_LINE);
			printf((user_param.report_fmt == MBS ? RESULT_FMT : RESULT_FMT_G));
		}

		printf((user_param.cpu_util_data.enable ? RESULT_EXT_CPU_UTIL : RESULT_EXT));
	}

	/* For half duplex write tests, server just waits for client to exit */
	if (user_param.machine == SERVER && user_param.verb == WRITE && !user_param.duplex) {

		if (user_param.data_validation &&
			data_validation_start(&ctx, &user_param, NULL))
			goto free_mem;

		if (ctx_hand_shake(&user_comm,&my_dest[0],&rem_dest[0])) {
			fprintf(stderr," Failed to exchange data between server and clients\n");
			goto destroy_context;
		}

		xchg_bw_reports(&user_comm, &my_bw_rep,&rem_bw_rep,atof(user_param.rem_version));

		if (user_param.test_method != RUN_INFINITELY) {
			print_full_bw_report(&user_param, &rem_bw_rep, NULL);
		} else {
			printf(" Client closed connection\n");
		}

		if (ctx_close_connection(&user_comm,&my_dest[0],&rem_dest[0])) {
			fprintf(stderr,"Failed to close connection between server and client\n");
			goto destroy_context;
		}

		if (user_param.output == FULL_VERBOSITY) {
			if (user_param.report_per_port)
				printf(RESULT_LINE_PER_PORT);
			else
				printf(RESULT_LINE);
		}

		if (user_param.data_validation)
			data_validation_stop_and_report(&ctx, &user_param, NULL);

		if (user_param.work_rdma_cm == ON) {
			if (user_param.data_validation)
				data_validation_destroy(&ctx);
			if (destroy_ctx(&ctx,&user_param)) {
				fprintf(stderr, "Failed to destroy resources\n");
				goto destroy_cm_context;
			}
			user_comm.rdma_params->work_rdma_cm = OFF;
			free(my_dest);
			free(rem_dest);
			free(user_param.ib_devname);
			if(destroy_ctx(user_comm.rdma_ctx, user_comm.rdma_params)) {
				free(user_comm.rdma_params);
				free(user_comm.rdma_ctx);
				return FAILURE;
			}
			free(user_comm.rdma_params);
			free(user_comm.rdma_ctx);
			return SUCCESS;
		}

		if (user_param.data_validation)
			data_validation_destroy(&ctx);
		free(my_dest);
		free(rem_dest);
		free(user_param.ib_devname);
		if(destroy_ctx(&ctx, &user_param)) {
			free(user_comm.rdma_params);
			return FAILURE;
		}
		free(user_comm.rdma_params);
		return SUCCESS;
	}

	/* In unidir WRITE with data_validation, client waits for server to start validation kernel */
	if (user_param.data_validation) {
		sleep(2);
	}
	if (user_param.test_method == RUN_ALL) {

		for (i = 1; i < 24 ; ++i) {

			user_param.size = (uint64_t)1 << i;

			if (user_param.machine == CLIENT || user_param.duplex)
				ctx_set_send_wqes(&ctx,&user_param,rem_dest);

			if (user_param.verb == WRITE_IMM && !user_param.use_unsolicited_write &&
			    (user_param.machine == SERVER || user_param.duplex)) {
				if (ctx_set_recv_wqes(&ctx,&user_param)) {
					fprintf(stderr," Failed to post receive recv_wqes\n");
					goto destroy_context;
				}
			}

			if (user_param.perform_warm_up) {

				if (user_param.verb == WRITE_IMM) {
					fprintf(stderr, "Warm up not supported for WRITE_IMM verb.\n");
					fprintf(stderr, "Skipping\n");
				} else if(perform_warm_up(&ctx, &user_param)) {
					fprintf(stderr, "Problems with warm up\n");
					goto destroy_context;
				}
			}

			if(user_param.duplex || user_param.verb == WRITE_IMM) {
				if (ctx_hand_shake(&user_comm,&my_dest[0],&rem_dest[0])) {
					fprintf(stderr,"Failed to sync between server and client between different msg sizes\n");
					goto destroy_context;
				}
			}

			if (user_param.duplex && user_param.verb == WRITE_IMM) {

				if(run_iter_bi(&ctx,&user_param)){
					fprintf(stderr," Failed to complete run_iter_bi function successfully\n");
					goto destroy_context;
				}

			} else if (user_param.machine == CLIENT || user_param.verb != WRITE_IMM) {

				if ((user_param.data_validation ? run_iter_bw_dv : run_iter_bw)(&ctx,&user_param)) {
					fprintf(stderr," Failed to complete run_iter_bw function successfully\n");
					goto destroy_context;
				}

			} else if (user_param.machine == SERVER) {

				if(run_iter_bw_server(&ctx,&user_param)) {
					fprintf(stderr," Failed to complete run_iter_bw_server function successfully\n");
					goto destroy_context;
				}
			}

			if (user_param.verb == WRITE_IMM || (user_param.duplex && (atof(user_param.version) >= 4.6))) {
				if (ctx_hand_shake(&user_comm,&my_dest[0],&rem_dest[0])) {
					fprintf(stderr,"Failed to sync between server and client between different msg sizes\n");
					goto destroy_context;
				}
			}

			print_report_bw(&user_param,&my_bw_rep);

			if (user_param.duplex && (user_param.verb != WRITE_IMM || user_param.test_type != DURATION)) {
				xchg_bw_reports(&user_comm, &my_bw_rep,&rem_bw_rep,atof(user_param.rem_version));
				print_full_bw_report(&user_param, &my_bw_rep, &rem_bw_rep);
			}
		}

	} else if (user_param.test_method == RUN_REGULAR) {

		if (user_param.machine == CLIENT || user_param.duplex)
			ctx_set_send_wqes(&ctx,&user_param,rem_dest);

		if (user_param.verb == WRITE_IMM && (user_param.machine == SERVER || user_param.duplex)) {
			if (ctx_set_recv_wqes(&ctx,&user_param)) {
				fprintf(stderr," Failed to post receive recv_wqes\n");
				goto destroy_context;
			}
		}

		if (user_param.verb != SEND && user_param.verb != WRITE_IMM) {

			if (user_param.perform_warm_up) {
				if(perform_warm_up(&ctx, &user_param)) {
					fprintf(stderr, "Problems with warm up\n");
					goto destroy_context;
				}
			}
		}

		/* Start validation for duplex mode */
		if (user_param.data_validation && user_param.duplex &&
			data_validation_start(&ctx, &user_param,
				user_param.machine == SERVER ? "SERVER" : "CLIENT"))
			goto free_mem;

		if(user_param.duplex || user_param.verb == WRITE_IMM) {
			if (ctx_hand_shake(&user_comm,&my_dest[0],&rem_dest[0])) {
				fprintf(stderr,"Failed to sync between server and client between different msg sizes\n");
				goto destroy_context;
			}
		}

		if (user_param.duplex && user_param.verb == WRITE_IMM) {

			if(run_iter_bi(&ctx,&user_param)){
				fprintf(stderr," Failed to complete run_iter_bi function successfully\n");
				goto destroy_context;
			}

		} else if (user_param.machine == CLIENT || user_param.verb != WRITE_IMM) {

			if ((user_param.data_validation ? run_iter_bw_dv : run_iter_bw)(&ctx,&user_param)) {
				fprintf(stderr," Failed to complete run_iter_bw function successfully\n");
				goto destroy_context;
			}

		} else if (user_param.machine == SERVER) {

			if(run_iter_bw_server(&ctx,&user_param)) {
				fprintf(stderr," Failed to complete run_iter_bw_server function successfully\n");
				goto destroy_context;
			}
		}

		/* Sync before stopping validation for duplex mode */
		if (user_param.data_validation && user_param.duplex) {
			if (ctx_hand_shake(&user_comm, &my_dest[0], &rem_dest[0])) {
				fprintf(stderr, "Failed to sync before stopping validation\n");
				goto free_mem;
			}
			usleep(100000);  /* 100ms delay for validator to drain */
		}

		print_report_bw(&user_param,&my_bw_rep);

		if (getenv("PERFTEST_AGENT")) {
			dprintf(g_agent_fd, "{\"v\":1,\"type\":\"result\",\"id\":0,\"role\":\"%s\",\"status\":\"ok\",\"verb\":\"write_bw\",\"size\":%lu,\"measurement\":{\"bw_peak_gbps\":%.2f,\"bw_avg_gbps\":%.2f,\"bw_min_gbps\":%.2f,\"msg_rate_mpps\":%.6f}}\n",
				user_param.machine == CLIENT ? "client" : "server",
				(unsigned long)my_bw_rep.size, my_bw_rep.bw_peak, my_bw_rep.bw_avg, my_bw_rep.bw_min, my_bw_rep.msgRate_avg);
		}

		/* run_job stdin REPL (FMT-1267): in PERFTEST_AGENT mode, read JSON job
		 * lines from stdin and run each on a fresh QP reusing warm MR/PD/CQ/CUDA
		 * (run_one_job), emitting a result on fd 3; "shutdown" -> bye. Outside
		 * agent mode, keep the PERFTEST_SPIKE_RERUNS self-test loop. */
		if (getenv("PERFTEST_AGENT")) {
			char line[4096];
			while (fgets(line, sizeof(line), stdin)) {
				char *q;
				long jid;
				if (strstr(line, "\"type\":\"shutdown\""))
					break;
				if (!strstr(line, "\"type\":\"job\""))
					continue;
				q = strstr(line, "\"id\":");
				jid = q ? atol(q + 5) : 0;
				{ char *sq = strstr(line, "\"size\":"); if (sq) user_param.size = (unsigned long)atol(sq + 7); }
				if (run_one_job(&ctx, &user_param, &user_comm, my_dest, rem_dest, &my_bw_rep, &rem_bw_rep)) {
					dprintf(g_agent_fd, "{\"v\":1,\"type\":\"result\",\"id\":%ld,\"role\":\"%s\",\"status\":\"failed\",\"fault\":{\"reason\":\"qp_setup_failed\"}}\n",
						jid, user_param.machine == CLIENT ? "client" : "server");
					goto destroy_context;
				}
				dprintf(g_agent_fd, "{\"v\":1,\"type\":\"result\",\"id\":%ld,\"role\":\"%s\",\"status\":\"ok\",\"verb\":\"write_bw\",\"size\":%lu,\"measurement\":{\"bw_peak_gbps\":%.2f,\"bw_avg_gbps\":%.2f,\"bw_min_gbps\":%.2f,\"msg_rate_mpps\":%.6f}}\n",
					jid, user_param.machine == CLIENT ? "client" : "server",
					(unsigned long)my_bw_rep.size, my_bw_rep.bw_peak, my_bw_rep.bw_avg, my_bw_rep.bw_min, my_bw_rep.msgRate_avg);
			}
			dprintf(g_agent_fd, "{\"v\":1,\"type\":\"bye\"}\n");
		} else {
			const char *rj_env = getenv("PERFTEST_SPIKE_RERUNS");
			int rj_reruns = rj_env ? atoi(rj_env) : 1;
			int rj_i;
			for (rj_i = 1; rj_i < rj_reruns; rj_i++)
				if (run_one_job(&ctx, &user_param, &user_comm, my_dest, rem_dest, &my_bw_rep, &rem_bw_rep))
					goto destroy_context;
		}

		if (user_param.duplex && (user_param.verb != WRITE_IMM || user_param.test_type != DURATION)) {
			xchg_bw_reports(&user_comm, &my_bw_rep,&rem_bw_rep,atof(user_param.rem_version));
			print_full_bw_report(&user_param, &my_bw_rep, &rem_bw_rep);
		}

		if (user_param.report_both && user_param.duplex) {
			printf(RESULT_LINE);
			printf("\n Local results: \n");
			printf(RESULT_LINE);
			printf((user_param.report_fmt == MBS ? RESULT_FMT : RESULT_FMT_G));
			printf((user_param.cpu_util_data.enable ? RESULT_EXT_CPU_UTIL : RESULT_EXT));
			print_full_bw_report(&user_param, &my_bw_rep, NULL);
			printf(RESULT_LINE);

			printf("\n Remote results: \n");
			printf(RESULT_LINE);
			printf((user_param.report_fmt == MBS ? RESULT_FMT : RESULT_FMT_G));
			printf((user_param.cpu_util_data.enable ? RESULT_EXT_CPU_UTIL : RESULT_EXT));
			print_full_bw_report(&user_param, &rem_bw_rep, NULL);
		}
	} else if (user_param.test_method == RUN_INFINITELY) {

		if (user_param.machine == CLIENT || user_param.duplex)
			ctx_set_send_wqes(&ctx,&user_param,rem_dest);

		else if (user_param.machine == SERVER && user_param.verb == WRITE_IMM) {
			if (ctx_set_recv_wqes(&ctx,&user_param)) {
				fprintf(stderr," Failed to post receive recv_wqes\n");
				goto destroy_context;
			}
		}

		if (user_param.verb == WRITE_IMM) {
			if (ctx_hand_shake(&user_comm,&my_dest[0],&rem_dest[0])) {
				fprintf(stderr,"Failed to exchange data between server and clients\n");
				goto destroy_context;
			}
		}

		if (user_param.machine == CLIENT || user_param.verb == WRITE) {
			if(run_iter_bw_infinitely(&ctx,&user_param)) {
				fprintf(stderr," Error occurred while running infinitely! aborting ...\n");
				goto destroy_context;
			}
		} else if (user_param.machine == SERVER && user_param.verb == WRITE_IMM) {
			if(run_iter_bw_infinitely_server(&ctx,&user_param)) {
				fprintf(stderr," Error occurred while running infinitely on server! aborting ...\n");
				goto destroy_context;
			}
		}
	}

	if (user_param.output == FULL_VERBOSITY) {
		if (user_param.report_per_port)
			printf(RESULT_LINE_PER_PORT);
		else
			printf(RESULT_LINE);
	}

	/* Stop validation for duplex mode (after result line) */
	if (user_param.data_validation && user_param.duplex) {
		data_validation_stop_and_report(&ctx, &user_param,
				user_param.machine == SERVER ? "SERVER" : "CLIENT");
	}

	/* For half duplex write tests, server just waits for client to exit */
	if (user_param.machine == CLIENT && user_param.verb == WRITE && !user_param.duplex) {
		if (ctx_hand_shake(&user_comm,&my_dest[0],&rem_dest[0])) {
			fprintf(stderr," Failed to exchange data between server and clients\n");
			goto destroy_context;
		}

		xchg_bw_reports(&user_comm, &my_bw_rep,&rem_bw_rep,atof(user_param.rem_version));
	}

	/* Closing connection. */
	if (ctx_close_connection(&user_comm,&my_dest[0],&rem_dest[0])) {
		fprintf(stderr,"Failed to close connection between server and client\n");
		goto destroy_context;
	}

	if (!user_param.is_bw_limit_passed && (user_param.is_limit_bw == ON ) ) {
		fprintf(stderr,"Error: BW result is below bw limit\n");
		goto destroy_context;
	}

	if (!user_param.is_msgrate_limit_passed && (user_param.is_limit_bw == ON )) {
		fprintf(stderr,"Error: Msg rate  is below msg_rate limit\n");
		goto destroy_context;
	}
	if (user_param.work_rdma_cm == ON) {
		if (user_param.data_validation)
			data_validation_destroy(&ctx);
		if (destroy_ctx(&ctx,&user_param)) {
			fprintf(stderr, "Failed to destroy resources\n");
			goto destroy_cm_context;
		}

		user_comm.rdma_params->work_rdma_cm = OFF;
		free(rem_dest);
		free(my_dest);
		free(user_param.ib_devname);
		if(destroy_ctx(user_comm.rdma_ctx, user_comm.rdma_params)) {
			free(user_comm.rdma_params);
			free(user_comm.rdma_ctx);
			return FAILURE;
		}
		free(user_comm.rdma_params);
		free(user_comm.rdma_ctx);
		return SUCCESS;
	}

	if (user_param.data_validation)
		data_validation_destroy(&ctx);
	free(rem_dest);
	free(my_dest);
	free(user_param.ib_devname);
	if(destroy_ctx(&ctx, &user_param)){
		free(user_comm.rdma_params);
		return FAILURE;
	}
	free(user_comm.rdma_params);
	return SUCCESS;

destroy_context:
	if (user_param.data_validation)
		data_validation_destroy(&ctx);
	if (destroy_ctx(&ctx,&user_param))
		fprintf(stderr, "Failed to destroy resources\n");
destroy_cm_context:
	if (user_param.work_rdma_cm == ON) {
		rdma_cm_flow_destroyed = 1;
		user_comm.rdma_params->work_rdma_cm = OFF;
		destroy_ctx(user_comm.rdma_ctx,user_comm.rdma_params);
	}
free_mem:
	free(rem_dest);
free_my_dest:
	free(my_dest);
free_rdma_params:
	if (user_param.use_rdma_cm == ON && rdma_cm_flow_destroyed == 0)
		dealloc_comm_struct(&user_comm, &user_param);

	else {
		if(user_param.use_rdma_cm == ON)
			free(user_comm.rdma_ctx);
		free(user_comm.rdma_params);
	}
free_devname:
	free(user_param.ib_devname);
return_error:
	//coverity[leaked_storage]
	return FAILURE;
}
