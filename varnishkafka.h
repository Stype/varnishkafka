/*
 * varnishkafka
 *
 * Copyright (c) 2013 Wikimedia Foundation
 * Copyright (c) 2013 Magnus Edenhill <vk@edenhill.se>
 *
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met: 
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer. 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution. 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE 
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <sys/queue.h>

#ifndef likely
#define likely(x)   __builtin_expect((x),1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect((x),0)
#endif




#define VSL_TAGS_MAX 255
/* A tag that is in all requests but only once. */
#define VSL_TAG__ONCE  SLT_ReqEnd


/**
 * Pointer to matched tag's content.
 */
struct match {
	const char *ptr;
	int         len;
};


/* Format configurations */
#define FMT_CONF_MAIN    0  /* Main format */
#define FMT_CONF_KEY     1  /* Kafka key format */
#define FMT_CONF_NUM     2


/**
 * Temporary scratch buffer
 */
struct tmpbuf {
	struct tmpbuf *next;
	size_t size;
	int    of;
	char   buf[0];  /* Must be at end of struct: allocated to '.size' */
};


/**
 * Currently parsed logline(s)
 */
struct logline {
	LIST_ENTRY(logline)  link;

	/* Log id */
	unsigned int  id;

	/* Per fmt_conf logline matches */
	struct match *match[FMT_CONF_NUM];

	/* Tags seen (for -m regexp) */
	uint64_t tags_seen;

	/* Sequence number */
	uint64_t seq;

	/* Last use of this logline */
	time_t   t_last;

	/* Rendered FMT_CONF_KEY for use in _MAIN output func */
	char    *key;
	size_t   key_len;

	/* Auxillery buffers (if scratch pad is not sufficient) */
	struct tmpbuf *tmpbuf;

	/* Scratch pad */
	int      sof;
	char     scratch[0];  /* Must be at end of struct.
			       * Allocated to conf.scratch_size bytes */
};


/**
 * Tag found in format.
 */
struct tag {
	struct tag *next;
	struct fmt *fmt;
	int    fid;    /* conf.fconf index */
	int    spec;
	int    tag;
	char  *var;
	int    varlen;
	int  (*parser) (const struct tag *tag, struct logline *lp,
			const char *ptr, int len);
	int    col;
	int    flags;
#define TAG_F_NOVARMATCH  0x1  /* Dont attempt to match tag->var to input */
};

/**
 * Formatting from format
 */
struct fmt {
	int   id;         /* formatter (i.e., (char)'r' in "%r") */
	int   idx;        /* fmt[] array index */
	const char *var;  /* variable name  (for %{..}x,i,o) */
	const char *def;  /* default string, typically "-" */
	int   deflen;     /* default string's length */
	const char *name; /* field name (for JSON, et.al) */
	int   namelen;    /* name length */
	enum {
		FMT_TYPE_STRING,
		FMT_TYPE_NUMBER,
	}     type;       /* output type (for JSON, et.al) */
	int   flags;
#define FMT_F_ESCAPE    0x1 /* Escape the value string */
};


typedef enum {
	VK_ENC_STRING,
	VK_ENC_JSON,
} fmt_enc_t;

struct fmt_conf {
	/* Array of tags in output order. */
	struct fmt *fmt;
	int         fmt_cnt;
	int         fmt_size;

	int         fid;  /* conf.fconf index */
	fmt_enc_t   encoding;
};


/**
 * varnishkafka config & state struct
 *
 * Try to keep commonly used fields at the top.
 */
struct conf {
	int         run;
	int         pret;   /* parse return value: use to exit parser. */
	int         m_flag;

	/* Sparsely populated with desired tags */
	struct tag **tag;

	/* Format configurations */
	struct fmt_conf fconf[FMT_CONF_NUM];
	int             fconf_cnt;

	uint64_t    sequence_number;

	size_t      scratch_size;    /* Size of scratch buffer */
	int         datacopy;
	fmt_enc_t   fmt_enc;
	int         total_fmt_cnt;
	int         loglines_hsize;  /* Log id hash size */
	int         loglines_hmax;   /* Max log ids per hash bucket */
	int         tag_size_max;    /* Maximum tag size to accept without
				      * truncating it. */

	int         stats_interval;  /* Statistics output interval */
	char       *stats_file;      /* Statistics output log file */
	FILE       *stats_fp;        /* Statistics file pointer    */
	time_t      t_last_stats;    /* Last stats output */

	int         need_logrotate;  /* If this is 1, log files will be reopened */

	/* Kafka config */
	int         partition;
	char       *topic;

	char       *logname;
	int         log_level;
	int         log_to;
#define VK_LOG_STDERR 0x1
#define VK_LOG_SYSLOG 0x2
	int         log_rate;        /* Maximum log rate per minute. */
	int         log_rate_period; /* Log rate limiting period */

	int         log_kafka_msg_error;  /* Log Kafka message delivery errors*/

	char       *format[FMT_CONF_NUM]; /* Configured format string(s) */
	int         daemonize;

	rd_kafka_conf_t       *rk_conf;
	rd_kafka_topic_conf_t *topic_conf;
};

extern struct conf conf;
struct VSM_data *vd;


int conf_file_read (const char *path);


void vk_log0 (const char *func, const char *file, int line,
	      const char *facility, int level, const char *fmt, ...)
	__attribute__((format (printf, 6, 7)));

#define vk_log(facility,level,fmt...) \
	vk_log0(__FUNCTION__,__FILE__,__LINE__, facility, level, fmt)

#define _DBG(fmt...) vk_log("DEBUG", LOG_DEBUG, fmt)

void vk_log_stats(const char *fmt, ...)
	__attribute__((format (printf, 1, 2)));

void out_kafka (struct fmt_conf *fconf, struct logline *lp,
		const char *buf, size_t len);
void out_stdout (struct fmt_conf *fconf, struct logline *lp,
		 const char *buf, size_t len);
void out_null (struct fmt_conf *fconf, struct logline *lp,
	       const char *buf, size_t len);
extern void (*outfunc) (struct fmt_conf *fconf, struct logline *lp,
			const char *buf, size_t len);
