#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "conf.h"
#include "rule.h"
#include "format.h"
#include "zc_defs.h"

/*******************************************************************************/
static void xlog_conf_debug(xlog_conf_t *a_conf);
static const char xlog_default_format[] = "&default	\"$d(%m-%d %T) $P [$p:$F:$L] $m$n\"";
/*******************************************************************************/
static int xlog_conf_parse_line(xlog_conf_t * a_conf, char *line, long line_len, int *init_chk_conf)
{
	int rc = 0;
	int nread = 0;
	char name[MAXLEN_CFG_LINE + 1];
	char value[MAXLEN_CFG_LINE + 1];
	xlog_format_t *a_format = NULL;
	xlog_rule_t *a_rule = NULL;

	if (line_len > MAXLEN_CFG_LINE || strlen(line) > MAXLEN_CFG_LINE) {
		zc_error("line_len[%ld] > MAXLEN_CFG_LINE[%ld], may cause overflow", line_len, MAXLEN_CFG_LINE);
		return -1;
	}

	zc_debug("line=[%s]", line);

	switch (line[0]) {
	case '@':
		memset(name, 0x00, sizeof(name));
		memset(value, 0x00, sizeof(value));
		nread = sscanf(line + 1, "%s%s", name, value);
		if (nread != 2) {
			zc_error("sscanf [%s] fail, name or value is null", line);
			return -1;
		}
		if (STRCMP(name, ==, "buf_size_min")) {
			a_conf->buf_size_min = zc_parse_byte_size(value);
			zc_debug("buf_size_min=[%ld]", a_conf->buf_size_min);
		} else if (STRCMP(name, ==, "buf_size_max")) {
			a_conf->buf_size_max = zc_parse_byte_size(value);
			zc_debug("buf_size_max=[%ld]", a_conf->buf_size_max);
		} else if (STRCMP(name, ==, "init_chk_conf")) {
			*init_chk_conf = atoi(value);
			a_conf->init_chk_conf = *init_chk_conf;
			zc_debug("init_chk_conf=[%d]", *init_chk_conf);
		} else if (STRCMP(name, ==, "rotate_lock_file")) {
			if (strlen(value) > sizeof(a_conf->rotate_lock_file) - 1) {
				zc_error("lock_file name[%s] is too long", value);
				return -1;
			}
			strcpy(a_conf->rotate_lock_file, value);
			zc_debug("lock_file=[%s]", a_conf->rotate_lock_file);
		} else {
			zc_error("in line[%s], name[%s] is wrong", line, name);
			return -1;
		}
		break;
	case '&':
		a_format = xlog_format_new(line, strlen(line));
		if (!a_format) {
			zc_error("xlog_format_init fail,a_format is free");
			return -1;
		}
		if (STRCMP(a_format->name, ==, "default")) {
			zc_debug("use conf file default format overwrite inner default format");
			rc = zc_arraylist_set(a_conf->formats, 0, a_format);
			if (rc) {
				zc_error("add list fail");
				xlog_format_del(a_format);
				return -1;
			}
		} else {
			rc = zc_arraylist_add(a_conf->formats, a_format);
			if (rc) {
				zc_error("add list fail");
				xlog_format_del(a_format);
				return -1;
			}
		}
		break;
	default:
		a_rule = xlog_rule_new(a_conf->formats, line, strlen(line));
		if (!a_rule) {
			zc_error("xlog_rule_init fail, a_rule is free");
			return -1;
		}
		rc = zc_arraylist_add(a_conf->rules, a_rule);
		if (rc) {
			zc_error("add list fail");
			xlog_rule_del(a_rule);
			return -1;
		}
		break;
	}

	return 0;
}

static int xlog_conf_read_config(xlog_conf_t * a_conf)
{
	int rc = 0;
	FILE *fp = NULL;
	int init_chk_conf = 0;

	char line[MAXLEN_CFG_LINE + 1];
	char *pline = NULL;
	char *p = NULL;
	int line_no = 0;
	size_t line_len = 0;
	int i = 0;

	struct stat a_stat;
	struct tm local_time;

	rc = lstat(a_conf->file, &a_stat);
	if (rc) {
		zc_error("lstat conf file[%s] fail, errno[%d]", a_conf->file, errno);
		return -1;
	}

	localtime_r(&(a_stat.st_mtime), &local_time);
	strftime(a_conf->mtime, sizeof(a_conf->mtime), "%F %X", &local_time);

	if ((fp = fopen(a_conf->file, "r")) == NULL) {
		zc_error("open configure file[%s] fail", a_conf->file);
		return -1;
	}
	/* Now process the file.
	 */
	pline = line;
	memset(&line, 0x00, sizeof(line));
	while (fgets((char *)pline, sizeof(line) - (pline - line), fp) != NULL) {
		++line_no;
		line_len = strlen(pline);
		if (pline[line_len - 1] == '\n') {
			pline[line_len - 1] = '\0';
		}

		/* check for end-of-section, comments, strip off trailing
		 * spaces and newline character.
		 */
		p = pline;
		while (*p && isspace((int)*p))
			++p;
		if (*p == '\0' || *p == '#')
			continue;

		/* we now need to copy the characters to the begin of line. As this overlaps,
		 * we can not use strcpy(). -- rgerhards, 2008-03-20
		 */
		for (i = 0; p[i] != '\0'; ++i) {
			pline[i] = p[i];
		}
		pline[i] = '\0';

		for (p = pline + strlen(pline) - 1; isspace((int)*p); --p)
			/*EMPTY*/;

		if (*p == '\\') {
			if ((p - line) > MAXLEN_CFG_LINE - 30) {
				/* Oops the buffer is full - what now? */
				pline = line;
			} else {
				for (p--; isspace((int)*p); --p)
					/*EMPTY*/;
				p++;
				*p = 0;
				pline = p;
				continue;
			}
		} else
			pline = line;

		*++p = '\0';

		/* we now have the complete line, and are positioned at the first non-whitespace
		 * character. So let's process it
		 */
		rc = xlog_conf_parse_line(a_conf, line, strlen(line), &init_chk_conf);
		if (rc) {
			/* we log a message, but otherwise ignore the error. After all, the next
			 * line can be correct.  -- rgerhards, 2007-08-02
			 */
			zc_error("parse configure file[%s] line[%ld] fail", a_conf->file, line_no);
			if (init_chk_conf || getenv("XLOG_ICC")) {
				rc = -1;
				goto xlog_conf_read_config_exit;
			} else {
				rc = 0;
				continue;
			}
		}
	}

      xlog_conf_read_config_exit:

	fclose(fp);
	return rc;
}


int xlog_conf_init(xlog_conf_t * a_conf, char *conf_file)
{
	int rc = 0;
	xlog_format_t *a_format;
	int nwrite = 0;

	zc_assert(a_conf, -1);

	if (conf_file) {
		nwrite = snprintf(a_conf->file, sizeof(a_conf->file), "%s", conf_file);
	} else if (getenv("XLOG_CONFPATH") != NULL) {
		nwrite = snprintf(a_conf->file, sizeof(a_conf->file), "%s", getenv("XLOG_CONFPATH"));
	} else {
		nwrite = snprintf(a_conf->file, sizeof(a_conf->file), "/etc/xlog.conf");
	}
	if (nwrite < 0 || nwrite >= sizeof(a_conf->file)) {
		zc_error("not enough space for path name, nwrite=[%d]", nwrite);
		return -1;
	}

	a_conf->buf_size_min = 1024;
	a_conf->buf_size_max = 0;

	a_format = xlog_format_new((char *)xlog_default_format, strlen(xlog_default_format));
	if (!a_format) {
		zc_error("xlog_format_new fail");
		return -1;
	}

	a_conf->formats = zc_arraylist_new((zc_arraylist_del_fn) xlog_format_del);
	if (!(a_conf->formats)) {
		zc_error("init format_list fail");
		rc = -1;
		goto xlog_conf_init_exit;
	}

	/* add the default format at the head of list */
	rc = zc_arraylist_add(a_conf->formats, a_format);
	if (rc) {
		zc_error("zc_arraylist_add fail");
		xlog_format_del(a_format);
		rc = -1;
		goto xlog_conf_init_exit;
	}

	a_conf->rules = zc_arraylist_new((zc_arraylist_del_fn) xlog_rule_del);
	if (!(a_conf->rules)) {
		zc_error("init rule_list fail");
		rc = -1;
		goto xlog_conf_init_exit;
	}


	rc = xlog_conf_read_config(a_conf);
	if (rc) {
		zc_error("xlog_conf_read_config fail");
		rc = -1;
		goto xlog_conf_init_exit;
	}

      xlog_conf_init_exit:
	if (rc) {
		xlog_conf_fini(a_conf);
	}

	xlog_conf_debug(a_conf);
	return rc;
}

void xlog_conf_fini(xlog_conf_t * a_conf)
{
	zc_assert(a_conf,);

	if (a_conf->formats) {
		zc_arraylist_del(a_conf->formats);
		a_conf->formats = NULL;
	}
	if (a_conf->rules) {
		zc_arraylist_del(a_conf->rules);
		a_conf->rules = NULL;
	}

	memset(a_conf, 0x00, sizeof(xlog_conf_t));
	return;
}

int xlog_conf_update(xlog_conf_t * a_conf, char *conf_file)
{
	int rc = 0;
	int nwrite = 0;
	xlog_format_t *a_format;

	zc_assert(a_conf, -1);

	if (conf_file) {
		memset(&a_conf->file, 0x00, sizeof(a_conf->file));
		nwrite = snprintf(a_conf->file, sizeof(a_conf->file), "%s", conf_file);
		if (nwrite < 0 || nwrite >= sizeof(a_conf->file)) {
			zc_error("not enough space for path name, nwrite=[%d]", nwrite);
			return -1;
		}
	} /* else use last conf file */

	if (a_conf->formats) {
		zc_arraylist_del(a_conf->formats);
		a_conf->formats = NULL;
	}
	if (a_conf->rules) {
		zc_arraylist_del(a_conf->rules);
		a_conf->rules = NULL;
	}

	a_conf->buf_size_min = 1024;
	a_conf->buf_size_max = 0;

	a_format = xlog_format_new((char *)xlog_default_format, strlen(xlog_default_format));
	if (!a_format) {
		zc_error("xlog_format_new fail");
		return -1;
	}

	a_conf->formats = zc_arraylist_new((zc_arraylist_del_fn) xlog_format_del);
	if (!(a_conf->formats)) {
		zc_error("init format_list fail");
		rc = -1;
		goto xlog_conf_update_exit;
	}

	/* add the default format at the head of list */
	rc = zc_arraylist_add(a_conf->formats, a_format);
	if (rc) {
		zc_error("zc_arraylist_add fail");
		xlog_format_del(a_format);
		rc = -1;
		goto xlog_conf_update_exit;
	}

	a_conf->rules = zc_arraylist_new((zc_arraylist_del_fn) xlog_rule_del);
	if (!(a_conf->rules)) {
		zc_error("init rule_list fail");
		rc = -1;
		goto xlog_conf_update_exit;
	}


	rc = xlog_conf_read_config(a_conf);
	if (rc) {
		zc_error("xlog_conf_read_config fail");
		rc = -1;
		goto xlog_conf_update_exit;
	}

      xlog_conf_update_exit:
	if (rc) {
		xlog_conf_fini(a_conf);
	}

	xlog_conf_debug(a_conf);
	return rc;
}

/*******************************************************************************/
static void xlog_conf_debug(xlog_conf_t *a_conf)
{
	zc_debug("---conf---");
	zc_debug("file:[%s],mtime:[%s]", a_conf->file, a_conf->mtime);
	zc_debug("init_chk_conf:[%d]", a_conf->init_chk_conf);
	zc_debug("buf_size_min:[%ld]", a_conf->buf_size_min);
	zc_debug("buf_size_max:[%ld]", a_conf->buf_size_max);
	zc_debug("rotate_lock_file:[%s]", a_conf->rotate_lock_file);
	zc_debug("formats:[%p]", a_conf->formats);
	zc_debug("rules:[%p]", a_conf->rules);
	return;
}

void xlog_conf_profile(xlog_conf_t *a_conf)
{
	int i;
	xlog_rule_t *a_rule;
	xlog_format_t *a_format;

	zc_error("---conf[%p]---", a_conf);
	zc_error("file:[%s],mtime:[%s]", a_conf->file, a_conf->mtime);
	zc_error("init_chk_conf:[%d]", a_conf->init_chk_conf);
	zc_error("buf_size_min:[%ld]", a_conf->buf_size_min);
	zc_error("buf_size_max:[%ld]", a_conf->buf_size_max);
	zc_error("rotate_lock_file:[%s]", a_conf->rotate_lock_file);

	zc_error("---rules[%p]---", a_conf->rules);
	for (i = 0; i < zc_arraylist_len(a_conf->rules); i++) {
		a_rule = (xlog_rule_t *) zc_arraylist_get(a_conf->rules, i);
		if (!a_rule) {
			zc_error("get null rule");
			continue;
		}
		xlog_rule_profile(a_rule);
	}

	zc_error("---formats[%p]---", a_conf->formats);
	for (i = 0; i < zc_arraylist_len(a_conf->formats); i++) {
		a_format = (xlog_format_t *) zc_arraylist_get(a_conf->formats, i);
		if (!a_format) {
			zc_error("get null format");
			continue;
		}
		xlog_format_profile(a_format);
	}
	return;
}

#if 0
int xlog_conf_output_event(xlog_conf_t * a_conf, xlog_event_t * a_event)
{
	int rc = 0;
	int rd = 0;
	int i;
	xlog_rule_t *a_rule = NULL;
	xlog_buf_t *a_buf = NULL;
	pthread_t tid;

	zc_assert(a_conf, -1);
	zc_assert(a_event, -1);

	rd = pthread_rwlock_rdlock(&(a_conf->lock));
	if (rd) {
		zc_error("pthread_rwlock_rdlock fail, rd[%d]", rd);
		return -1;
	}

	if (a_conf->init_success <= 0) {
		zc_error("befor log, must init fisrt!");
		rc = -1;
		goto xlog_conf_output_event_exit;
	}

	/* get or make a thread buf */
	tid = pthread_self();
	a_buf = zc_hashtable_get(a_conf->bufs, (void *)&tid);
	if (!a_buf) {

		rd = pthread_rwlock_unlock(&(a_conf->lock));
		if (rd) {
			zc_error("pthread_rwlock_unlock fail, rd[%d]", rd);
			return -1;
		}

		/* change to wrlock, make and insert new thread buf */
		rd = pthread_rwlock_wrlock(&(a_conf->lock));
		if (rd) {
			zc_error("pthread_rwlock_wrlock fail, rd[%d]", rd);
			return -1;
		}

		a_buf = zc_hashtable_get(a_conf->bufs, (void *)&tid);
		if (!a_buf) {
			pthread_t *ptid;

			zc_debug("first use buf in tid[%ld]", (long)tid);

			ptid = calloc(1, sizeof(*ptid));
			if (!ptid) {
				zc_error("calloc fail, errno[%d]", errno);
				rc = -1;
				goto xlog_conf_output_event_exit;
			}
			*ptid = tid;

			rc = xlog_buf_init(&a_buf, a_conf->buf_size_min, a_conf->buf_size_max);
			if (rc) {
				zc_error("xlog_buf_init fail");
				rc = -1;
				goto xlog_conf_output_event_exit;
			}

			rc = zc_hashtable_put(a_conf->bufs, (void *)ptid, (void *)a_buf);
			if (rc) {
				zc_error("zc_hashtable_put fail");
				rc = -1;
				goto xlog_conf_output_event_exit;
			}
		}
	}

	xlog_event_set_category(a_event, a_conf->category);

	/* go through all match rules to output */
	for (i = 0; i < zc_arraylist_length(a_conf->match_rules); i++) {
		a_rule = (xlog_rule_t *) zc_arraylist_get_idx(a_conf->match_rules, i);
		if (!a_rule) {
			zc_error("get null rule");
			rc = -1;
			goto xlog_conf_output_event_exit;
		}

		rc = xlog_rule_output_event(a_rule, a_buf, a_event);
		if (rc) {
			zc_error("hzb_log_rule_output_event fail");
			rc = -1;
			goto xlog_conf_output_event_exit;
		}
	}

      xlog_conf_output_event_exit:
	rd = pthread_rwlock_unlock(&(a_conf->lock));
	if (rd) {
		zc_error("pthread_rwlock_unlock fail, rd[%d]", rd);
		return -1;
	}

	return rc;
}
#endif

/*******************************************************************************/
