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


#define _XOPEN_SOURCE 500    /* for strptime() */
#define _BSD_SOURCE          /* for daemon() */
#define _GNU_SOURCE          /* for strndupa() */
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <sys/queue.h>
#include <syslog.h>
#include <netdb.h>

#include <varnish/varnishapi.h>
#include <librdkafka/rdkafka.h>

#include "varnishkafka.h"
#include "base64.h"


/* Kafka handle */
static rd_kafka_t *rk;
/* Kafka topic */
static rd_kafka_topic_t *rkt;

/* Varnish shared memory handle*/
struct VSM_data *vd;

const char *conf_file_path = VARNISHKAFKA_CONF_PATH;


/**
 * Currently parsed logline(s)
 */
static struct logline **loglines;
static int              logline_cnt;



/**
 * All constant strings in the format are placed in 'const_string' which
 * hopefully will be small enough to fit a single cache line.
 */
static char  *const_string      = NULL;
static size_t const_string_size = 0;
static size_t const_string_len  = 0;

/**
 * Adds a constant string to the constant string area.
 * If the string is already found in the area, return it instead.
 */
static char *const_string_add (const char *in, int inlen) {
	char *ret;
	const char *instr = strndupa(in, inlen);

	if (!const_string || !(ret = strstr(const_string, instr))) {
		if (const_string_len + inlen + 1 >= const_string_size) {
			/* Reallocate buffer to fit new string (and more) */
			const_string_size = (const_string_size + inlen + 64)*2;
			const_string = realloc(const_string, const_string_size);
		}

		/* Append new string */
		ret = const_string + const_string_len;
		memcpy(ret, in, inlen);
		ret[inlen] = '\0';
		const_string_len += inlen;
	}

	return ret;
}


/**
 * Print parsed format string: formatters
 */
static __attribute__((unused)) void fmt_dump (void) {
	int i;

	_DBG("%i/%i formats:", conf.fmt_cnt, conf.fmt_size);
	for (i = 0 ; i < conf.fmt_cnt ; i++) {
		_DBG(" #%-3i  fmt %i (%c)  var \"%s\", def \"%.*s\"",
		     i,
		     conf.fmt[i].id,
		     isprint(conf.fmt[i].id) ? (char)conf.fmt[i].id : 0,
		     conf.fmt[i].var ? : "",
		     conf.fmt[i].deflen, conf.fmt[i].def);
	}
}

/**
 * Print parser format string: tags
 */
static __attribute__((unused)) void tag_dump (void) {
	int i;

	_DBG("Tags:");
	for (i = 0 ; i < VSL_TAGS_MAX ; i++) {
		struct tag *tag;

		for (tag = conf.tag[i] ; tag ; tag = tag->next) {
			_DBG(" #%-3i  spec 0x%x, tag %s (%i), var \"%s\", "
			     "parser %p, col %i, fmt #%i %i (%c)",
			     i,
			     tag->spec,
			     VSL_tags[tag->tag], tag->tag,
			     tag->var, tag->parser,
			     tag->col,
			     tag->fmt->idx,
			     tag->fmt->id,
			     isprint(tag->fmt->id) ?
			     (char)tag->fmt->id : 0);
		}
	}
}



/**
 * Adds a parsed formatter to the list of formatters
 */
static int format_add (int fmtr,
		       const char *var, ssize_t varlen,
		       const char *def, ssize_t deflen,
		       char *errstr, size_t errstr_size) {
	struct fmt *fmt;

	if (conf.fmt_cnt >= conf.fmt_size) {
		conf.fmt_size = (conf.fmt_size ? : 32) * 2;
		conf.fmt = realloc(conf.fmt, conf.fmt_size * sizeof(*conf.fmt));
	}

	fmt = &conf.fmt[conf.fmt_cnt];

	fmt->id = fmtr;
	fmt->idx = conf.fmt_cnt;

	if (var) {
		if (varlen == -1)
			varlen = strlen(var);

		fmt->var = malloc(varlen+1);
		memcpy((char *)fmt->var, var, varlen);
		((char *)fmt->var)[varlen] = '\0';
	} else
		fmt->var = NULL;

	if (!def)
		def = "-";

	if (deflen == -1)
		deflen = strlen(def);
	fmt->deflen = deflen;
	fmt->def = const_string_add(def, deflen);

	conf.fmt_cnt++;

	return fmt->idx;
}



/**
 * Adds a parsed tag to the list of tags
 */
static int tag_add (struct fmt *fmt,
		    int spec, int tagid,
		    const char *var, ssize_t varlen,
		    int col,
		    int (*parser) (const struct tag *tag, struct logline *lp,
				   const char *ptr, int len),
		    char *errstr, size_t errstr_size) {
	struct tag *tag;

	tag = calloc(1, sizeof(*tag));

	assert(tagid < VSL_TAGS_MAX);

	if (conf.tag[tagid])
		tag->next = conf.tag[tagid];

	conf.tag[tagid] = tag;

	tag->spec   = spec;
	tag->tag    = tagid;
	tag->col   = col;
	tag->fmt    = fmt;
	tag->parser = parser;

	if (var) {
		if (varlen == -1)
			varlen = strlen(var);

		tag->var = malloc(varlen+1);
		tag->varlen = varlen;
		memcpy(tag->var, var, varlen);
		tag->var[varlen] = '\0';
	} else
		tag->var = NULL;

	return 0;
}
		     

/**
 * Assign 'PTR' of size 'LEN' as a match for 'TAG' in logline 'LP'.
 *
 * 'PTR' must be a pointer to persistent memory:
 *   - either VSL shared memory (original VSL tag payload)
 *   - or to a buffer allocated in the 'LP' scratch buffer.
 */
#define MATCH_ASSIGN(TAG,LP,PTR,LEN) do {			\
		(LP)->match[(TAG)->fmt->idx].ptr = (PTR);	\
		(LP)->match[(TAG)->fmt->idx].len = (LEN);	\
	} while (0)


/**
 * Allocate persistent memory space ('len' bytes) in 
 * logline 'lp's scratch buffer.
 */
static inline char *scratch_alloc (const struct tag *tag, struct logline *lp,
				   int len) {
	char *ptr;

	if (lp->sof + len > sizeof(lp->scratch)) {
		vk_log("WARNING", LOG_WARNING,
		       "scratch pad is too small (%zd bytes), "
		       "need %i bytes or more",
		       sizeof(lp->scratch), lp->sof + len);
		return NULL;
	}

	ptr = lp->scratch + lp->sof;
	lp->sof += len;
	return ptr;
}

/**
 * Helper that allocates 'len' bytes in the scratch buffer and
 * writes the contents of 'src' there.
 */
static inline int scratch_write (const struct tag *tag, struct logline *lp,
				 const char *src, int len) {
	char *dst;

	if (unlikely((dst = scratch_alloc(tag, lp, len)) == NULL))
		return -1;

	memcpy(dst, src, len);

	MATCH_ASSIGN(tag, lp, dst, len);

	return len;
}

/**
 * Helper that allocates enough space in the scratch buffer to fit
 * the string produced by 'fmt...'.
 */
static inline int scratch_printf (const struct tag *tag, struct logline *lp,
				  const char *fmt, ...) {
	va_list ap, ap2;
	int r;
	char *dst;

	va_copy(ap2, ap);
	va_start(ap2, fmt);
	r = vsnprintf(NULL, 0, fmt, ap2);
	va_end(ap2);

	if (!(dst = scratch_alloc(tag, lp, r+1)))
		return -1;

	va_start(ap, fmt);
	vsnprintf(dst, r+1, fmt, ap);
	va_end(ap);

	MATCH_ASSIGN(tag, lp, dst, r);

	return r;
}

static inline int scratch_printf (const struct tag *tag, struct logline *lp,
				  const char *fmt, ...)
	__attribute__((format(printf,3,4)));



static char *strnchr (const char *s, int len, int c) {
	const char *end = s + len;
	while (s < end) {
		if (*s == c)
			return (char *)s;
		s++;
	}

	return NULL;
}
	

/**
 * Splits 'ptr' (with length 'len') by delimiter 'delim' and assigns
 * the Nth ('col') column to '*dst' and '*dstlen'.
 * Does not modify the input data ('ptr'), only points to it.
 *
 * Returns 1 if the column was found, else 0.
 *
 * NOTE: Columns start at 1.
 */
static int column_get (int col, char delim, const char *ptr, int len,
		       const char **dst, int *dstlen) {
	const char *s = ptr;
	const char *b = s;
	const char *end = s + len;
	int i = 0;

	while (s < end) {
		if (*s != delim) {
			s++;
			continue;
		}

		if (s != b && col == ++i) {
			*dst = b;
			*dstlen = (int)(s - b);
			return 1;
		}

		b = ++s;
	}

	if (s != b && col == ++i) {
		*dst = b;
		*dstlen = (int)(s - b);
		return 1;
	}

	return 0;
}



/**
 *
 * Misc parsers for formatters
 *
 */
static int parse_BackendOpen (const struct tag *tag, struct logline *lp,
			      const char *ptr, int len) {
	const char *s;
	int slen;
	const int deflen = strlen("default");

	if (unlikely(!column_get(1, ' ', ptr, len, &s, &slen)))
		return 0;

	if (slen == deflen && !strncmp(s, "default", slen))
		column_get(2, ' ', ptr, len, &s, &slen);

	MATCH_ASSIGN(tag, lp, s, slen);

	return 0;
}	

static int parse_U (const struct tag *tag, struct logline *lp,
		    const char *ptr, int len) {
	const char *qs;
	int slen = len;

	if ((qs = strnchr(ptr, len, '?')))
		slen = (int)(qs - ptr);

	MATCH_ASSIGN(tag, lp, ptr, slen);
	return slen;
}

static int parse_q (const struct tag *tag, struct logline *lp,
		    const char *ptr, int len) {
	const char *qs;
	int slen = len;

	if (!(qs = strnchr(ptr, len, '?')))
		return 0;

	slen = len - (int)(qs - ptr);

	MATCH_ASSIGN(tag, lp, qs, slen);
	return slen;
}

static int parse_t (const struct tag *tag, struct logline *lp,
		    const char *ptr, int len) {
	struct tm tm;
	char *dst;
	const char *timefmt = "[%d/%b/%Y:%T %z]";
	const int timelen = sizeof("[20/AbrMnth/2013:00:00:00 +0200]");
	int tlen;

	if (tag->tag == SLT_TxHeader) {
		if (unlikely(!strptime(strndupa(ptr, len),
				       "%a, %d %b %Y %T", &tm)))
			return 0;

	} else {
		time_t t = strtoul(ptr, NULL, 10);
		localtime_r(&t, &tm);
	}

	if (unlikely(!(dst = scratch_alloc(tag, lp, timelen))))
		return -1;

	tlen = strftime(dst, timelen, timefmt, &tm);

	MATCH_ASSIGN(tag, lp, dst, tlen);

	return tlen;
}

static int parse_auth_user (const struct tag *tag, struct logline *lp,
			    const char *ptr, int len) {
	int rlen = len - 6/*"basic "*/;
	int ulen;
	char *tmp;
	char *q;

	if (unlikely(rlen <= 0 || strncasecmp(ptr, "basic ", 6) || (rlen % 2)))
		return 0;

	/* Calculate base64 decoded length */
	if (unlikely(!(ulen = (rlen * 4) / 3)))
		return 0;

	/* Protect our stack */
	if (unlikely(ulen > 1000))
		return 0;

	tmp = alloca(ulen+1);

	if ((ulen = VB64_decode2(tmp, ulen, ptr+6, rlen)) <= 0)
		return 0;

	/* Strip password */
	if ((q = strnchr(tmp, ulen, ':')))
		*q = '\0';

	return scratch_write(tag, lp, tmp, strlen(tmp));
}


static int parse_hitmiss (const struct tag *tag, struct logline *lp,
			  const char *ptr, int len) {
	if (len == 3 && !strncmp(ptr, "hit", 3)) {
		MATCH_ASSIGN(tag, lp, ptr, len);
		return len;
	} else if (len == 4 &&
		 (!strncmp(ptr, "miss", 4) ||
		  !strncmp(ptr, "pass", 4))) {
		MATCH_ASSIGN(tag, lp, "miss", 4);
		return 4;
	}

	return 0;
}

static int parse_handling (const struct tag *tag, struct logline *lp,
			   const char *ptr, int len) {
	if ((len == 3 && !strncmp(ptr, "hit", 3)) ||
	    (len == 4 && (!strncmp(ptr, "miss", 4) ||
			  !strncmp(ptr, "pass", 4)))) {
		MATCH_ASSIGN(tag, lp, ptr, len);
		return len;
	}
	return 0;
}

static int parse_seq (const struct tag *tag, struct logline *lp,
		       const char *ptr, int len) {
	return scratch_printf(tag, lp, "%"PRIu64, conf.sequence_number);
}



/**
 * 'arr' is an array of tuples (const char *from, const char *to) with
 * replacements. 'arr' must be terminated with a NULL (from).
 *
 * Returns a new allocated string with strings replaced according to 'arr'.
 *
 * NOTE: 'arr' must be sorted by descending 'from' length.
 */
static char *string_replace_arr (const char *in, const char **arr) {
	char  *out;
	size_t inlen = strlen(in);
	size_t outsize = (inlen + 64) * 2;
	size_t of = 0;
	const char *s, *sp;

	out = malloc(outsize);

	s = sp = in;
	while (*s) {
		const char **a;

		for (a = arr ; *a ; a += 2) {
			const char *from = arr[0];
			const char *to   = arr[1];
			size_t fromlen   = strlen(from);
			size_t tolen;
			ssize_t diff;

			if (strncmp(s, from, fromlen))
				continue;

			tolen = strlen(to);
			diff  = tolen - fromlen;

			if (s > sp) {
				memcpy(out+of, sp, (int)(s-sp));
				of += (int)(s-sp);
			}
			sp = s += fromlen;

			if (of + diff >= outsize) {
				/* Not enough space in output buffer,
				 * reallocate and make some headroom to
				 * avoid future reallocs. */
				outsize = (of + diff + 64) * 2;
				out = realloc(out, outsize);
				assert(out);
			}
			
			memcpy(out+of, to, tolen);
			of += tolen;
			s--;
			break;
		}
		s++;
	}

	if (s > sp) {
		memcpy(out+of, sp, (int)(s-sp));
		of += (int)(s-sp);
	}

	out[of] = '\0';

	return out;
}


/**
 * Parse the format string and build a parsing array.
 */
static int format_parse (const char *format_orig,
			 char *errstr, size_t errstr_size) {
	/**
	 * Maps a formatter %X to a VSL tag and column id, or parser, or both
	 */
	struct {
		/* A formatter may be backed by multiple tags.
		 * The first matching tag observed in the log will be used. */
		struct {
			/* VSL_S_CLIENT or VSL_S_BACKEND, or both */
			int spec;
			/* The SLT_.. tag id */
			int tag;
			/* For "Name: Value" tags (such as SLT_RxHeader),
			 * this is the "Name" part. */
			const char *var;
			/* Special handling for non-name-value vars such as
			 * %{Varnish:handling}x. fmtvar is "Varnish:handling" */
			const char *fmtvar; 
			/* Column to extract:
			 * 0 for entire string, else 1, 2, .. */
			int col;
			/* Parser to manually extract and/or convert a
			 * tag's content. */
			int (*parser) (const struct tag *tag,
				       struct logline *lp,
				       const char *ptr, int len);
		} f[3+1]; /* increase size when necessary (max used size + 1) */
		
		/* Default string if no matching tag was found or all
		 * parsers failed, defaults to "-". */
		const char *def;

	} map[256] = {
		/* Indexed by formatter character as
		 * specified by varnishncsa(1) */
		['b'] = { {
				{ VSL_S_CLIENT, SLT_Length },
				{ VSL_S_BACKEND, SLT_RxHeader,
				  var: "content-length" }
			} },
		['H'] = { {
				{ VSL_S_CLIENT, SLT_RxProtocol },
				{ VSL_S_BACKEND, SLT_TxProtocol },
			}, def: "HTTP/1.0" },
		['h'] = { {
				{ VSL_S_CLIENT, SLT_ReqStart, col: 1 },
				{ VSL_S_BACKEND, SLT_BackendOpen,
				  parser: parse_BackendOpen }
			} },
		['i'] = { { 
				{ VSL_S_CLIENT, SLT_RxHeader },
			} },
		['l'] = { {
				{ VSL_S_CLIENT|VSL_S_BACKEND },
			}, def: conf.logname },
		['m'] = { {
				{ VSL_S_CLIENT, SLT_RxRequest },
				{ VSL_S_BACKEND, SLT_TxRequest },
			} },
		['q'] = { {
				{ VSL_S_CLIENT, SLT_RxURL, parser: parse_q },
				{ VSL_S_BACKEND, SLT_TxURL, parser: parse_q },
			},  def: "" },
		['o'] = { { 
				{ VSL_S_CLIENT, SLT_TxHeader },
			} },
		['s'] = { {
				{ VSL_S_CLIENT, SLT_TxStatus },
				{ VSL_S_BACKEND, SLT_RxStatus },
			} },
		['t'] = { {
				{ VSL_S_CLIENT, SLT_ReqEnd,
				  parser: parse_t, col: 3 },
				{ VSL_S_BACKEND, SLT_RxHeader,
				  var: "date", parser: parse_t },
			} },
		['U'] = { {
				{ VSL_S_CLIENT, SLT_RxURL, parser: parse_U },
				{ VSL_S_BACKEND, SLT_TxURL, parser: parse_U },
			} },
		['u'] = { {
				{ VSL_S_CLIENT, SLT_RxHeader,
				  var: "authorization",
				  parser: parse_auth_user },
				{ VSL_S_BACKEND, SLT_TxHeader,
				  var: "authorization",
				  parser: parse_auth_user },
			} },
		['x'] = { { 
				{ VSL_S_CLIENT, SLT_ReqEnd,
				  fmtvar: "Varnish:time_firstbyte", col: 5 },
				{ VSL_S_CLIENT, SLT_VCL_call,
				  fmtvar: "Varnish:hitmiss",
				  parser: parse_hitmiss },
				{ VSL_S_CLIENT, SLT_VCL_call,
				  fmtvar: "Varnish:handling",
				  parser: parse_handling },

			} },
		['n'] = { {
				{ VSL_S_CLIENT|VSL_S_BACKEND, VSL_TAG__ONCE,
				  parser: parse_seq },
			} },
	};
	/* Replace legacy formatters */
	static const char *replace[] = {
		/* "legacy-formatter", "new-formatter(s)" */
		"%r", "%m http://%{Host?localhost}i%U%q %H",
		NULL, NULL
	};
	const char *s, *t;
	const char *format;
	int cnt = 0;

	/* Perform legacy replacements. */
	format = string_replace_arr(format_orig, replace);

	conf.tag = calloc(VSL_TAGS_MAX, sizeof(*conf.tag));


	/* Parse the format string */
	s = t = format;
	while (*s) {
		const char *begin;
		const char *var = NULL;
		int varlen = 0;
		const char *def = NULL;
		int deflen = -1;
		int fmtid;
		int i;

		if (*s != '%') {
			s++;
			continue;
		}

		/* ".....%... "
		 *  ^---^  add this part as verbatim string */
		if (s > t)
			if (format_add(0,
				       NULL, 0,
				       t, (int)(s - t),
				       errstr, errstr_size) == -1)
				return -1;

		begin = s;
		s++;

		/* Parse '{VAR}X': '*s' will be set to X, and 'var' to VAR.
		 * varnishkafka also adds the following features:
		 *  VAR?DEF,   where DEF is a default value, in this mode
		 *             VAR can be empty, and {?DEF} may be applied to
		 *             any formatter.
		 *             I.e.: %{Content-type?text/html}o
		 *                   %{?no-user}u
		 */
		if (*s == '{') {
			const char *a = s+1;
			const char *b = strchr(a, '}');
			const char *q;

			if (!b) {
				snprintf(errstr, errstr_size,
					 "Expecting '}' after \"%.*s...\"",
					 30, begin);
				return -1;
			}

			if (a == b) {
				snprintf(errstr, errstr_size,
					 "Empty {} identifier at \"%.*s...\"",
					 30, begin);
				return -1;
			}

			if (!*(b+1)) {
				snprintf(errstr, errstr_size,
					 "No formatter following "
					 "identifier at \"%.*s...\"",
					 30, begin);
				return -1;
			}

			var = a;

			if ((q = strnchr(a, (int)(b-a), '?'))) {
				/* "VAR?DEF" */
				def = q+1;
				deflen = (int)(b - def);
				varlen = (int)(q - a);
				if (varlen == 0)
					var = NULL;
			} else
				varlen = (int)(b-a);			

			s = b+1;
		}

		if (!map[(int)*s].f[0].spec) {
			snprintf(errstr, errstr_size,
				 "Unknown formatter '%c' at \"%.*s...\"",
				 *s, 30, begin);
			return -1;
		}

		if (!def)
			def = map[(int)*s].def;

		/* Add formatter to ordered list of formatters */
		if ((fmtid = format_add(*s, var, varlen, def, deflen,
					errstr, errstr_size)) == -1)
			return -1;

		cnt++;

		/* Now add the matched tags specification to the
		 * list of parse tags */
		for (i = 0 ; map[(int)*s].f[i].spec ; i++) {
			if (map[(int)*s].f[i].tag == 0)
				continue;

			/* mapping has fmtvar specified, make sure it 
			 * matches the format's variable. */
			if (map[(int)*s].f[i].fmtvar) {
				if (!var ||
				    strlen(map[(int)*s].f[i].fmtvar) != varlen||
				    strncmp(map[(int)*s].f[i].fmtvar, var,
					    varlen))
					continue;
				/* fmtvar's resets the format var */
				var = NULL;
				varlen = 0;
			}

			if (tag_add(&conf.fmt[fmtid],
				    map[(int)*s].f[i].spec,
				    map[(int)*s].f[i].tag,
				    var ? var : map[(int)*s].f[i].var,
				    var ? varlen : -1,
				    map[(int)*s].f[i].col,
				    map[(int)*s].f[i].parser,
				    errstr, errstr_size) == -1)
				return -1;
		}


		t = ++s;
	}

	/* "..%x....."
	 *      ^---^  add this part as verbatim string */
	if (s > t)
		if (format_add(0, NULL, 0,
			       t, (int)(s - t),
			       errstr, errstr_size) == -1)
			return -1;

	/* Dump parsed format string. */
	if (conf.log_level >= 7) {
		fmt_dump();
		tag_dump();
	}

	if (conf.fmt_cnt == 0) {
		snprintf(errstr, errstr_size, "Format string is empty");
		return -1;
	} else if (cnt == 0) {
		snprintf(errstr, errstr_size, "No %%.. formatters in format");
		return -1;
	}

	return conf.fmt_cnt;
}








/**
 * Kafka outputter
 */
void out_kafka (const char *buf, size_t len) {
	if (rd_kafka_produce(rkt, conf.partition, RD_KAFKA_MSG_F_COPY,
			     (void *)buf, len, NULL, 0, NULL) == -1) {
		vk_log("PRODUCE", LOG_WARNING,
		       "Failed to produce kafka message: %s",
		       strerror(errno));
	}

	rd_kafka_poll(rk, 0);
}


/**
 * Stdout outputter
 */
void out_stdout (const char *buf, size_t len) {
	printf("%.*s\n", (int)len, buf);
}


/**
 * Currently selected outputter.
 */
void (*outfunc) (const char *buf, size_t len) = out_kafka;



/**
 * Kafka error callback
 */
static void kafka_error_cb (rd_kafka_t *rk, int err,
			    const char *reason, void *opaque) {
	vk_log("KAFKAERR", LOG_ERR, "Kafka error (%i): %s", err, reason);
}


/**
 * Kafka message delivery report callback.
 * Called for each delivered (or failed delivery) message.
 * NOTE: If the dr callback is not to be used it can be turned off to
 *       improve performance.
 */
static void kafka_dr_cb (rd_kafka_t *rk,
			 void *payload, size_t len,
			 int error_code,
			 void *opaque, void *msg_opaque) {
	_DBG("Kafka delivery report: error=%i, size=%zd", error_code, len);
}



/**
 * Render an accumulated logline to string and pass it to the output function.
 */
static void render_match (struct logline *lp, uint64_t seq) {
	char buf[4096];
	int  i;
	int  of = 0;

	/* Render each formatter in order. */
	for (i = 0 ; i < conf.fmt_cnt ; i++) {
		const void *ptr;
		int len = lp->match[i].len;

		/* Either use accumulated value, or the default value. */
		if (len) {
			ptr = lp->match[i].ptr;
		} else {
			ptr = conf.fmt[i].def;
			len = conf.fmt[i].deflen;
		}

		if (of + len >= sizeof(buf))
			break;

		memcpy(buf+of, ptr, len);
		of += len;
	}
	
	buf[of] = '\0';

	/* Pass rendered log line to outputter function */
	outfunc(buf, of);
}


/**
 * Resets the given logline and makes it ready for accumulating a new request.
 */
static void logline_reset (struct logline *lp) {
	struct match *match = lp->match;
	/* Clear logline, except for scratch pad since it will be overwritten */
	memset(lp, 0, sizeof(*lp) - sizeof(lp->scratch));
	memset(match, 0, conf.fmt_cnt * sizeof(*lp->match));
	lp->match = match;

}


/**
 * Returns a logline.
 */
static inline struct logline *logline_get (unsigned int id) {
	struct logline *lp;

	if (unlikely(id >= logline_cnt)) {
		int newcnt = id + 64;

		_DBG("Reallocate logline array "
		     "from %i to %i entries (new id %i)",
		     logline_cnt, newcnt, id);
		     
		loglines = realloc(loglines, sizeof(*loglines) * newcnt);
		if (!loglines) {
			vk_log("MEM", LOG_CRIT,
			       "Unable to allocate new logline "
			       "array of size %lu bytes (%i entries): %s",
			       sizeof(*loglines) * newcnt, newcnt,
			       strerror(errno));
			conf.run = 0;
			conf.pret = -1;
			return NULL;
		}

		memset(loglines + logline_cnt, 0,
		       sizeof(*loglines) * (newcnt - logline_cnt));
		
		logline_cnt = newcnt;
	}

	if (unlikely(!(lp = loglines[id]))) {
		/* Allocate a new logline if necessary. */
		lp = loglines[id] = calloc(1, sizeof(*lp) + 
					   (conf.fmt_cnt * sizeof(*lp->match)));
		lp->match = (struct match *)(lp+1);
	}

	return lp;
}





/**
 * Given a single tag 'tagid' with its data 'ptr' and 'len';
 * try to match it to the registered format tags.
 *
 * Returns 1 if the line is done and can be rendered, else 0.
 */
static int tag_match (struct logline *lp, int spec, enum VSL_tag_e tagid,
		      const char *ptr, int len) {
	const struct tag *tag;

	/* Iterate through all handlers for this tag. */
	for (tag = conf.tag[tagid] ; tag ; tag = tag->next) {
		const char *ptr2;
		int len2;

		/* Value already assigned */
		if (lp->match[tag->fmt->idx].ptr)
			continue;

		/* Match spec (client or backend) */
		if (!(tag->spec & spec))
			continue;

		if (tag->var) {
			const char *t;
			
			/* Variable match ("Varname: value") */
			if (!(t = strnchr(ptr, len, ':')))
				continue;
			
			if (tag->varlen != (int)(t-ptr) ||
			    strncasecmp(ptr, tag->var, tag->varlen))
				continue;

			ptr2 = t+2; /* ": " */
			len2 = len - (int)(ptr2-ptr);
		} else {
			ptr2 = ptr;
			len2 = len;
		}

		/* Get specified column if specified. */
		if (tag->col)
			if (!column_get(tag->col, ' ', ptr2, len2,
					&ptr2, &len2) == -1)
				continue;

		if (tag->parser) {
			/* Pass value to parser which will assign it. */
			tag->parser(tag, lp, ptr2, len2);
			
		} else {
			/* Fallback to verbatim field. */
			MATCH_ASSIGN(tag, lp, ptr2, len2);
		}
	}

	/* Request end: render the match string. */
	if (tagid == SLT_ReqEnd)
		return 1;
	else
		return 0;
}


/**
 * VSL_Dispatch() callback called for each tag read from the VSL.
 */
static int parse_tag (void *priv, enum VSL_tag_e tag, unsigned id,
		      unsigned len, unsigned spec, const char *ptr,
		      uint64_t bitmap) {
	struct logline *lp;
	int    is_complete = 0;

	if (unlikely(!spec))
		return conf.pret;

	if (0)
		_DBG("[%u] #%-3i %-12s %c %.*s",
		     id, tag, VSL_tags[tag],
		     spec & VSL_S_CLIENT ? 'c' : 'b',
		     len, ptr);

	if (unlikely(!(lp = logline_get(id))))
		return -1;

	/* Update bitfield of seen tags */
	lp->tags_seen |= bitmap;

	/* Accumulate matched tag content */
	if (likely(!(is_complete = tag_match(lp, spec, tag, ptr, len))))
		return conf.pret;

	/* Match tag regexp, if any */
	if (conf.m_flag && !VSL_Matched(vd, lp->tags_seen)) {
		logline_reset(lp);
		return conf.pret;
	}
	
	/* Log line is complete: render & output */
	render_match(lp, ++conf.sequence_number);

	/* clean up */
	logline_reset(lp);

	return conf.pret;
}


/**
 * varnishkafka logger
 */
void vk_log0 (const char *func, const char *file, int line,
	      const char *facility, int level, const char *fmt, ...) {
	va_list ap;
	char buf[512];

	if (level > conf.log_level || !conf.log_to)
		return;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (conf.log_to & VK_LOG_SYSLOG)
		syslog(level, "%s: %s", facility, buf);

	if (conf.log_to & VK_LOG_STDERR)
		fprintf(stderr, "%%%i %s: %s\n", level, facility, buf);
}


/**
 * Termination signal handler.
 * May be called multiple times (multiple SIGTERM/SIGINT) since a
 * blocking VSL_Dispatch() call cannot be aborted:
 *  - first signal: flag to exit VSL_Dispatch() when the next tag is read.
 *                  if succesful the kafka producer can send remaining messages
 *  - second signal: exit directly, queued kafka messages will be lost.
 *
 */
static void sig_term (int sig) {
	vk_log("TERM", LOG_NOTICE,
	       "Received signal %i: terminating", sig);
	conf.pret = -1;
	if (--conf.run <= -1) {
		vk_log("TERM", LOG_WARNING, "Forced termination");
		exit(0);
	}
}


static void usage (const char *argv0) {
	fprintf(stderr,
		"varnishkafka version %s\n"
		"Varnish log listener with Apache Kafka producer support\n"
		"\n"
		"Usage: %s [VSL_ARGS] [-S <config-file>]\n"
		"\n"
		" VSL_ARGS are standard Varnish VSL arguments:\n"
		"  %s\n"
		"\n"
		" The VSL_ARGS can also be set through the configuration file\n"
		" with \"varnish.arg.<..> = <..>\"\n"
		"\n"
		" Default configuration file path: %s\n"
		"\n",
		VARNISHKAFKA_VERSION,
		argv0,
		VSL_USAGE,
		VARNISHKAFKA_CONF_PATH);
	exit(1);
}


int main (int argc, char **argv) {
	char errstr[512];
	char hostname[1024];
	struct hostent *lh;
	char c;
	int r;

	/*
	 * Default configuration
	 */
	conf.log_level = 6;
	conf.log_to    = VK_LOG_STDERR;
	conf.daemonize = 1;

	rd_kafka_defaultconf_set(&conf.rk_conf);
	conf.rk_conf.clientid              = "varnishkafka";
	conf.rk_conf.error_cb              = kafka_error_cb;
	conf.rk_conf.producer.dr_cb        = kafka_dr_cb;
	conf.rk_conf.producer.max_messages = 1000000;

	rd_kafka_topic_defaultconf_set(&conf.topic_conf);
	conf.topic_conf.required_acks      = 1;

	conf.format = "%l %n %t %{Varnish:time_firstbyte}x %h "
		"%{Varnish:handling}x/%s %b %m http://%{Host}i%U%q - - "
		"%{Referer}i %{X-Forwarded-For}i %{User-agent}i";

	/* Construct logname (%l) from local hostname */
	gethostname(hostname, sizeof(hostname)-1);
	hostname[sizeof(hostname)-1] = '\0';
	lh = gethostbyname(hostname);
	conf.logname = strdup(lh->h_name);


	/* Create varnish shared memory handle */
	vd = VSM_New();
	VSL_Setup(vd);

	/* Parse command line arguments */
	while ((c = getopt(argc, argv, VSL_ARGS "hS:")) != -1) {
		switch (c) {
		case 'h':
			usage(argv[0]);
			break;
		case 'S':
			conf_file_path = optarg;
			break;
		case 'm':
			conf.m_flag = 1;
			/* FALLTHRU */
		default:
			if ((r = VSL_Arg(vd, c, optarg)) == 0)
				usage(argv[0]);
			else if (r == -1)
				exit(1); /* VSL_Arg prints error message */
			break;
		}
	}

	/* Read config file */
	if (conf_file_read(conf_file_path) == -1)
		exit(1);

	if (!conf.topic)
		usage(argv[0]);

	/* Always include client communication (-c) */
	VSL_Arg(vd, 'c', NULL);

	/* Set up syslog */
	if (conf.log_to & VK_LOG_SYSLOG)
		openlog("varnishkafka", LOG_PID|LOG_NDELAY, LOG_DAEMON);

	/* Termination signal handlers */
	signal(SIGINT, sig_term);
	signal(SIGTERM, sig_term);

	/* Ignore network disconnect signals, handled by rdkafka */
	signal(SIGPIPE, SIG_IGN);

	/* Initialize base64 decoder */
	VB64_init();

	/* Space is the most common format separator so add it first
	 * the the const string, followed by the typical default value "-". */
	const_string_add(" -", 2);

	/* Parse the format string */
	if (format_parse(conf.format, errstr, sizeof(errstr)) == -1) {
		vk_log("FMTPARSE", LOG_ERR,
		       "Failed to parse format string: %s\n%s",
		       conf.format, errstr);
		exit(1);
	}

	/* Open the log file */
	if (VSL_Open(vd, 1) != 0) {
		vk_log("VSLOPEN", LOG_ERR, "Failed to open Varnish VSL: %s\n",
		       strerror(errno));
		exit(1);
	}

	/* Daemonize if desired */
	if (conf.daemonize) {
		if (daemon(0, 0) == -1) {
			vk_log("KAFKANEW", LOG_ERR, "Failed to daemonize: %s",
			       strerror(errno));
			exit(1);
		}
		conf.log_to &= ~VK_LOG_STDERR;
	}

	/* Kafka outputter */
	if (outfunc == out_kafka) {
		/* Create Kafka handle */
		if (!(rk = rd_kafka_new(RD_KAFKA_PRODUCER, &conf.rk_conf,
					errstr, sizeof(errstr)))) {
			vk_log("KAFKANEW", LOG_ERR,
			       "Failed to create kafka handle: %s", errstr);
			exit(1);
		}

		rd_kafka_set_log_level(rk, conf.log_level);

		/* Create Kafka topic handle */
		if (!(rkt = rd_kafka_topic_new(rk, conf.topic,
					       &conf.topic_conf))) {
			vk_log("KAFKANEW", LOG_ERR,
			       "Invalid topic or configuration: %s: %s",
			       conf.topic, strerror(errno));
			exit(1);
		}
	}

	/* Main dispatcher loop depending on outputter */
	conf.run = 1;
	conf.pret = 0;

	if (outfunc == out_kafka) {
		/* Kafka outputter */

		while (conf.run && VSL_Dispatch(vd, parse_tag, NULL) >= 0)
			rd_kafka_poll(rk, 0);

		/* Run until all kafka messages have been delivered
		 * or we are stopped again */
		conf.run = 1;

		while (conf.run && rd_kafka_outq_len(rk) > 0)
			rd_kafka_poll(rk, 100);

		rd_kafka_destroy(rk);

	} else {
		/* Stdout outputter */

		while (conf.run && VSL_Dispatch(vd, parse_tag, NULL) >= 0)
			;

	}

	VSM_Close(vd);
	exit(0);
}