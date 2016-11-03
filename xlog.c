/**
 * Copyright (C) 2001-2003 FhG Fokus
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>

#include "sr_module.h"
#include "dprint.h"
#include "error.h"
#include "mem/mem.h"
#include "xlog.h"

#include "pvar.h"
#include "trace_api.h"

#define XLOG_TRACE_API_MODULE "proto_hep"

xlog_register_trace_type_f xlog_register_trace_type=NULL;
xlog_check_is_traced_f xlog_check_is_traced=NULL;
xlog_get_next_destination_f xlog_get_next_destination=NULL;


char *log_buf = NULL;

int xlog_buf_size = 4096;
int xlog_force_color = 0;
int xlog_default_level = L_ERR;

trace_proto_t xlog_trace_api;

/* id with which xlog will be identified by siptrace module
 * and will identify an xlog tracing packet */
int xlog_proto_id;

/* xlog string identifier */
static const char* xlog_id_s="xlog";

static int buf_init(void)
{
	LM_DBG("initializing...\n");
	log_buf = (char*)pkg_malloc((xlog_buf_size+1)*sizeof(char));
	if(log_buf==NULL)
	{
		LM_ERR("no pkg memory left\n");
		return -1;
	}
	return 0;
}

int init_xlog(void)
{
	if (log_buf == NULL) {
		if (buf_init()) {
			LM_ERR("Cannot print message!\n");
			return -1;
		}
	}

	memset(&xlog_trace_api, 0, sizeof(trace_proto_t));
	if (trace_prot_bind(XLOG_TRACE_API_MODULE, &xlog_trace_api) < 0) {
		LM_DBG("no trace module loaded!\n");
	}

	if (xlog_register_trace_type)
		xlog_proto_id = xlog_register_trace_type((char *)xlog_id_s);

	return 0;
}

static inline int trace_xlog(struct sip_msg* msg, char* buf, int len)
{
	str x_msg = {buf, len};

	int siptrace_id_hash=0;
	union sockaddr_union to_su, from_su;

	const int proto = IPPROTO_TCP;

	trace_dest send_dest, old_dest=NULL;
	trace_message trace_msg;

	if (msg == NULL || buf == NULL) {
		LM_ERR("bad input!\n");
		return -1;
	}

	/* api not loaded; no need to continue */
	if (xlog_trace_api.create_trace_message == NULL)
		return 0;

	/* xlog not traced; exit... */
	if (xlog_check_is_traced && (siptrace_id_hash=xlog_check_is_traced(xlog_proto_id)) == 0)
		return 0;

	if (!xlog_get_next_destination)
		return 0;

	/*
	 * Source and destination will be set to localhost(127.0.0.1) port 0
	 */
	from_su.sin.sin_addr.s_addr = TRACE_INADDR_LOOPBACK;
	from_su.sin.sin_port = 0;
	from_su.sin.sin_family = AF_INET;

	to_su.sin.sin_addr.s_addr = TRACE_INADDR_LOOPBACK;
	to_su.sin.sin_port = 0;
	to_su.sin.sin_family = AF_INET;


	while((send_dest=xlog_get_next_destination(old_dest, siptrace_id_hash))) {
		trace_msg = xlog_trace_api.create_trace_message(&from_su, &to_su,
				proto, &x_msg, xlog_proto_id, send_dest);
		if (trace_msg == NULL) {
			LM_ERR("failed to create trace message!\n");
			return -1;
		}

		if (xlog_trace_api.add_trace_data(trace_msg, msg->callid->body.s,
			msg->callid->body.len, TRACE_TYPE_STR, 0x0011/* correlation id*/, 0) < 0) {
			LM_ERR("failed to add correlation id to the packet!\n");
			return -1;
		}

		if (xlog_trace_api.send_message(trace_msg, send_dest, NULL) < 0) {
			LM_ERR("failed to send trace message!\n");
			return -1;
		}

		xlog_trace_api.free_message(trace_msg);

		old_dest=send_dest;
	}

	return 0;
}

int xl_print_log(struct sip_msg* msg, pv_elem_p list, int *len)
{
	if (pv_printf(msg, list, log_buf, len) < 0) {
		LM_ERR("failed to resolve xlog variables!\n");
		return -1;
	}

	if (trace_xlog(msg, log_buf, *len) < 0) {
		LM_ERR("failed to trace xlog message!\n");
		return -1;
	}

	return 0;
}


int xlog_2(struct sip_msg* msg, char* lev, char* frm)
{
	int log_len;
	long level;
	xl_level_p xlp;
	pv_value_t value;

	xlp = (xl_level_p)lev;
	if(xlp->type==1)
	{
		if(pv_get_spec_value(msg, &xlp->v.sp, &value)!=0
			|| value.flags&PV_VAL_NULL || !(value.flags&PV_VAL_INT))
		{
			LM_ERR("invalid log level value [%d]\n", value.flags);
			return -1;
		}
		level = (long)value.ri;
	} else {
		level = xlp->v.level;
	}

	if(!is_printable((int)level))
		return 1;

	log_len = xlog_buf_size;

	if(xl_print_log(msg, (pv_elem_t*)frm, &log_len)<0)
		return -1;

	/* log_buf[log_len] = '\0'; */
	LM_GEN1((int)level, "%.*s", log_len, log_buf);

	return 1;
}


int xlog_1(struct sip_msg* msg, char* frm, char* str2)
{
	int log_len;

	if(!is_printable(L_ERR))
		return 1;

	log_len = xlog_buf_size;

	if(xl_print_log(msg, (pv_elem_t*)frm, &log_len)<0)
		return -1;

	/* log_buf[log_len] = '\0'; */
	LM_GEN1(xlog_default_level, "%.*s", log_len, log_buf);

	return 1;
}

/**
 */
int xdbg(struct sip_msg* msg, char* frm, char* str2)
{
	int log_len;

	if(!is_printable(L_DBG))
		return 1;

	log_len = xlog_buf_size;

	if(xl_print_log(msg, (pv_elem_t*)frm, &log_len)<0)
		return -1;

	/* log_buf[log_len] = '\0'; */
	LM_GEN1(L_DBG, "%.*s", log_len, log_buf);

	return 1;
}

int pv_parse_color_name(pv_spec_p sp, str *in)
{

	if(in==NULL || in->s==NULL || sp==NULL)
		return -1;

	if(in->len != 2)
	{
		LM_ERR("color name must have two chars\n");
		return -1;
	}

	/* foreground */
	switch(in->s[0])
	{
		case 'x':
		case 's': case 'r': case 'g':
		case 'y': case 'b': case 'p':
		case 'c': case 'w': case 'S':
		case 'R': case 'G': case 'Y':
		case 'B': case 'P': case 'C':
		case 'W':
		break;
		default:
			goto error;
	}

	/* background */
	switch(in->s[1])
	{
		case 'x':
		case 's': case 'r': case 'g':
		case 'y': case 'b': case 'p':
		case 'c': case 'w':
		break;
		default:
			goto error;
	}

	sp->pvp.pvn.type = PV_NAME_INTSTR;
	sp->pvp.pvn.u.isname.type = AVP_NAME_STR;
	sp->pvp.pvn.u.isname.name.s = *in;

	sp->getf = pv_get_color;

	/* force the color PV type */
	sp->type = PVT_COLOR;
	return 0;
error:
	LM_ERR("invalid color name\n");
	return -1;
}

#define COL_BUF 10

#define append_sstring(p, end, s) \
        do{\
                if ((p)+(sizeof(s)-1)<=(end)){\
                        memcpy((p), s, sizeof(s)-1); \
                        (p)+=sizeof(s)-1; \
                }else{ \
                        /* overflow */ \
                        LM_ERR("append_sstring overflow\n"); \
                        goto error;\
                } \
        } while(0)


int pv_get_color(struct sip_msg *msg, pv_param_t *param,
		pv_value_t *res)
{
	static char color[COL_BUF];
	char* p;
	char* end;
	str s;

	if(log_stderr==0 && xlog_force_color==0)
	{
		s.s = "";
		s.len = 0;
		return pv_get_strval(msg, param, res, &s);
	}

	p = color;
	end = p + COL_BUF;

	/* excape sequenz */
	append_sstring(p, end, "\033[");

	if(param->pvn.u.isname.name.s.s[0]!='_')
	{
		if (islower((int)param->pvn.u.isname.name.s.s[0]))
		{
			/* normal font */
			append_sstring(p, end, "0;");
		} else {
			/* bold font */
			append_sstring(p, end, "1;");
			param->pvn.u.isname.name.s.s[0] += 32;
		}
	}

	/* foreground */
	switch(param->pvn.u.isname.name.s.s[0])
	{
		case 'x':
			append_sstring(p, end, "39;");
		break;
		case 's':
			append_sstring(p, end, "30;");
		break;
		case 'r':
			append_sstring(p, end, "31;");
		break;
		case 'g':
			append_sstring(p, end, "32;");
		break;
		case 'y':
			append_sstring(p, end, "33;");
		break;
		case 'b':
			append_sstring(p, end, "34;");
		break;
		case 'p':
			append_sstring(p, end, "35;");
		break;
		case 'c':
			append_sstring(p, end, "36;");
		break;
		case 'w':
			append_sstring(p, end, "37;");
		break;
		default:
			LM_ERR("invalid foreground\n");
			return pv_get_null(msg, param, res);
	}

	/* background */
	switch(param->pvn.u.isname.name.s.s[1])
	{
		case 'x':
			append_sstring(p, end, "49");
		break;
		case 's':
			append_sstring(p, end, "40");
		break;
		case 'r':
			append_sstring(p, end, "41");
		break;
		case 'g':
			append_sstring(p, end, "42");
		break;
		case 'y':
			append_sstring(p, end, "43");
		break;
		case 'b':
			append_sstring(p, end, "44");
		break;
		case 'p':
			append_sstring(p, end, "45");
		break;
		case 'c':
			append_sstring(p, end, "46");
		break;
		case 'w':
			append_sstring(p, end, "47");
		break;
		default:
			LM_ERR("invalid background\n");
			return pv_get_null(msg, param, res);
	}

	/* end */
	append_sstring(p, end, "m");

	s.s = color;
	s.len = p-color;
	return pv_get_strval(msg, param, res, &s);

error:
	return -1;
}

