/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int
__create_file(WT_SESSION_IMPL *session,
    const char *uri, int exclusive, const char *config)
{
	WT_DECL_ITEM(val);
	WT_DECL_RET;
	int is_metadata;
	const char *filecfg[4], *fileconf, *filename;

	fileconf = NULL;

	is_metadata = strcmp(uri, WT_METADATA_URI) == 0;

	filename = uri;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		WT_RET_MSG(session, EINVAL, "Expected a 'file:' URI: %s", uri);

	/* Check if the file already exists. */
	if (!is_metadata && (ret =
	    __wt_metadata_read(session, uri, &fileconf)) != WT_NOTFOUND) {
		if (exclusive)
			WT_TRET(EEXIST);
		goto err;
	}

	/* Create the file. */
	WT_ERR(__wt_block_manager_create(session, filename));
	if (WT_META_TRACKING(session))
		WT_ERR(__wt_meta_track_fileop(session, NULL, uri));

	/*
	 * If creating an ordinary file, append the current version numbers to
	 * the passed-in configuration and insert the resulting configuration
	 * into the metadata.
	 */
	if (!is_metadata) {
		WT_ERR(__wt_scr_alloc(session, 0, &val));
		WT_ERR(__wt_buf_fmt(session, val, "version=(major=%d,minor=%d)",
		    WT_BTREE_MAJOR_VERSION, WT_BTREE_MINOR_VERSION));
		filecfg[0] = WT_CONFIG_BASE(session, file_meta);
		filecfg[1] = config;
		filecfg[2] = val->data;
		filecfg[3] = NULL;
		WT_ERR(__wt_config_collapse(session, filecfg, &fileconf));
		if ((ret = __wt_metadata_insert(session, uri, fileconf)) != 0) {
			if (ret == WT_DUPLICATE_KEY)
				ret = EEXIST;
			goto err;
		}
	}

	/*
	 * Open the file to check that it was setup correctly.   We don't need
	 * to pass the configuration, we just wrote the collapsed configuration
	 * into the metadata file, and it's going to be read/used by underlying
	 * functions.
	 *
	 * Keep the handle exclusive until it is released at the end of the
	 * call, otherwise we could race with a drop.
	 */
	WT_ERR(__wt_conn_btree_get(
	    session, uri, NULL, NULL, WT_DHANDLE_EXCLUSIVE));
	if (WT_META_TRACKING(session))
		WT_ERR(__wt_meta_track_handle_lock(session, 1));
	else
		WT_ERR(__wt_session_release_btree(session));

err:	__wt_scr_free(&val);
	__wt_free(session, fileconf);
	return (ret);
}

int
__wt_schema_colgroup_source(WT_SESSION_IMPL *session,
    WT_TABLE *table, const char *cgname, const char *config, WT_ITEM *buf)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	const char *prefix, *suffix, *tablename;

	tablename = table->name + strlen("table:");
	prefix = "file:";
	suffix = ".wt";
	if ((ret = __wt_config_getones(session, config, "type", &cval)) == 0 &&
	    WT_STRING_MATCH("lsm", cval.str, cval.len)) {
		prefix = "lsm:";
		suffix = "";
	}
	WT_RET_NOTFOUND_OK(ret);

	if (cgname == NULL)
		WT_RET(__wt_buf_fmt(session, buf, "%s%s%s",
		    prefix, tablename, suffix));
	else
		WT_RET(__wt_buf_fmt(session, buf, "%s%s_%s%s",
		    prefix, tablename, cgname, suffix));

	return (0);
}

static int
__create_colgroup(WT_SESSION_IMPL *session,
    const char *name, int exclusive, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	WT_ITEM confbuf, fmt, namebuf;
	WT_TABLE *table;
	size_t tlen;
	const char *cfg[4] =
	    { WT_CONFIG_BASE(session, colgroup_meta), config, NULL, NULL };
	const char *sourcecfg[] = { config, NULL, NULL };
	const char **cfgp;
	const char *cgconf, *cgname, *sourceconf, *oldconf;
	const char *source, *tablename;

	cgconf = sourceconf = oldconf = NULL;
	WT_CLEAR(fmt);
	WT_CLEAR(confbuf);
	WT_CLEAR(namebuf);

	tablename = name;
	if (!WT_PREFIX_SKIP(tablename, "colgroup:"))
		return (EINVAL);
	cgname = strchr(tablename, ':');
	if (cgname != NULL) {
		tlen = (size_t)(cgname - tablename);
		++cgname;
	} else
		tlen = strlen(tablename);

	if ((ret =
	    __wt_schema_get_table(session, tablename, tlen, 1, &table)) != 0)
		WT_RET_MSG(session, (ret == WT_NOTFOUND) ? ENOENT : ret,
		    "Can't create '%s' for non-existent table '%.*s'",
		    name, (int)tlen, tablename);

	/* Make sure the column group is referenced from the table. */
	if (cgname != NULL && (ret =
	    __wt_config_subgets(session, &table->cgconf, cgname, &cval)) != 0)
		WT_ERR_MSG(session, EINVAL,
		    "Column group '%s' not found in table '%.*s'",
		    cgname, (int)tlen, tablename);

	/* Find the first NULL entry in the cfg stack. */
	for (cfgp = &cfg[1]; *cfgp; cfgp++)
		;

	/* Add the source to the colgroup config before collapsing. */
	if (__wt_config_getones(
	    session, config, "source", &cval) == 0 && cval.len != 0) {
		WT_ERR(__wt_buf_fmt(
		    session, &namebuf, "%.*s", (int)cval.len, cval.str));
		source = namebuf.data;
	} else {
		WT_ERR(__wt_schema_colgroup_source(
		    session, table, cgname, config, &namebuf));
		source = namebuf.data;
		WT_ERR(__wt_buf_fmt(
		    session, &confbuf, "source=\"%s\"", source));
		*cfgp++ = confbuf.data;
	}

	/* Calculate the key/value formats: these go into the source config. */
	WT_ERR(__wt_buf_fmt(session, &fmt, "key_format=%s", table->key_format));
	if (cgname == NULL)
		WT_ERR(__wt_buf_catfmt
		    (session, &fmt, ",value_format=%s", table->value_format));
	else {
		if (__wt_config_getones(session, config, "columns", &cval) != 0)
			WT_ERR_MSG(session, EINVAL,
			    "No 'columns' configuration for '%s'", name);
		WT_ERR(__wt_buf_catfmt(session, &fmt, ",value_format="));
		WT_ERR(__wt_struct_reformat(session,
		    table, cval.str, cval.len, NULL, 1, &fmt));
	}
	sourcecfg[1] = fmt.data;
	WT_ERR(__wt_config_concat(session, sourcecfg, &sourceconf));

	WT_ERR(__wt_schema_create(session, source, sourceconf));

	WT_ERR(__wt_config_collapse(session, cfg, &cgconf));
	if ((ret = __wt_metadata_insert(session, name, cgconf)) != 0) {
		/*
		 * If the entry already exists in the metadata, we're done.
		 * This is an error for exclusive creates but okay otherwise.
		 */
		if (ret == WT_DUPLICATE_KEY)
			ret = exclusive ? EEXIST : 0;
		goto err;
	}

	WT_ERR(__wt_schema_open_colgroups(session, table));

err:	__wt_free(session, cgconf);
	__wt_free(session, sourceconf);
	__wt_free(session, oldconf);
	__wt_buf_free(session, &confbuf);
	__wt_buf_free(session, &fmt);
	__wt_buf_free(session, &namebuf);

	__wt_schema_release_table(session, table);
	return (ret);
}

int
__wt_schema_index_source(WT_SESSION_IMPL *session,
    WT_TABLE *table, const char *idxname, const char *config, WT_ITEM *buf)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	const char *prefix, *suffix, *tablename;

	tablename = table->name + strlen("table:");
	prefix = "file:";
	suffix = ".wti";
	if ((ret = __wt_config_getones(session, config, "type", &cval)) == 0 &&
	    WT_STRING_MATCH("lsm", cval.str, cval.len)) {
		prefix = "lsm:";
		suffix = "_idx";
	}
	WT_RET_NOTFOUND_OK(ret);

	tablename = table->name + strlen("table:");
	WT_RET(__wt_buf_fmt(session, buf, "%s%s_%s%s",
	    prefix, tablename, idxname, suffix));

	return (0);
}

static int
__create_index(WT_SESSION_IMPL *session,
    const char *name, int exclusive, const char *config)
{
	WT_CONFIG pkcols;
	WT_CONFIG_ITEM ckey, cval, icols;
	WT_DECL_RET;
	WT_ITEM confbuf, extra_cols, fmt, namebuf;
	WT_TABLE *table;
	const char *cfg[4] =
	    { WT_CONFIG_BASE(session, index_meta), NULL, NULL, NULL };
	const char *sourcecfg[] = { config, NULL, NULL };
	const char *sourceconf, *source, *idxconf, *idxname;
	const char *tablename;
	size_t tlen;
	u_int i;

	idxconf = sourceconf = NULL;
	WT_CLEAR(confbuf);
	WT_CLEAR(fmt);
	WT_CLEAR(extra_cols);
	WT_CLEAR(namebuf);

	tablename = name;
	if (!WT_PREFIX_SKIP(tablename, "index:"))
		return (EINVAL);
	idxname = strchr(tablename, ':');
	if (idxname == NULL)
		WT_RET_MSG(session, EINVAL, "Invalid index name, "
		    "should be <table name>:<index name>: %s", name);

	tlen = (size_t)(idxname++ - tablename);
	if ((ret =
	    __wt_schema_get_table(session, tablename, tlen, 1, &table)) != 0)
		WT_RET_MSG(session, ret,
		    "Can't create an index for a non-existent table: %.*s",
		    (int)tlen, tablename);

	if (__wt_config_getones(session, config, "source", &cval) == 0) {
		WT_ERR(__wt_buf_fmt(session, &namebuf,
		    "%.*s", (int)cval.len, cval.str));
		source = namebuf.data;
	} else {
		WT_ERR(__wt_schema_index_source(
		    session, table, idxname, config, &namebuf));
		source = namebuf.data;

		/* Add the source name to the index config before collapsing. */
		WT_ERR(__wt_buf_catfmt(session, &confbuf,
		    ",source=\"%s\"", source));
	}

	/* Calculate the key/value formats. */
	if (__wt_config_getones(session, config, "columns", &icols) != 0)
		WT_ERR_MSG(session, EINVAL,
		    "No 'columns' configuration for '%s'", name);

	/*
	 * The key format for an index is somewhat subtle: the application
	 * specifies a set of columns that it will use for the key, but the
	 * engine usually adds some hidden columns in order to derive the
	 * primary key.  These hidden columns are part of the source's
	 * key_format, which we are calculating now, but not part of an index
	 * cursor's key_format.
	 */
	WT_ERR(__wt_config_subinit(session, &pkcols, &table->colconf));
	for (i = 0; i < table->nkey_columns &&
	    (ret = __wt_config_next(&pkcols, &ckey, &cval)) == 0;
	    i++) {
		/*
		 * If the primary key column is already in the secondary key,
		 * don't add it again.
		 */
		if (__wt_config_subgetraw(session, &icols, &ckey, &cval) == 0)
			continue;
		WT_ERR(__wt_buf_catfmt(
		    session, &extra_cols, "%.*s,", (int)ckey.len, ckey.str));
	}
	if (ret != 0 && ret != WT_NOTFOUND)
		goto err;

	/*
	 * Index values are normally empty: all columns are packed into the
	 * index key.  The exception is LSM, which (currently) reserves empty
	 * values as tombstones.  Use a single padding byte in that case.
	 */
	if (WT_PREFIX_MATCH(source, "lsm:"))
		WT_ERR(__wt_buf_fmt(session, &fmt, "value_format=x,"));
	else
		WT_ERR(__wt_buf_fmt(session, &fmt, "value_format=,"));
	WT_ERR(__wt_buf_fmt(session, &fmt, "value_format=,key_format="));
	WT_ERR(__wt_struct_reformat(session, table,
	    icols.str, icols.len, (const char *)extra_cols.data, 0, &fmt));

	/* Check for a record number index key, which makes no sense. */
	WT_ERR(__wt_config_getones(session, fmt.data, "key_format", &cval));
	if (cval.len == 1 && cval.str[0] == 'r')
		WT_ERR_MSG(session, EINVAL,
		    "column-store index may not use the record number as its "
		    "index key");

	sourcecfg[1] = fmt.data;
	WT_ERR(__wt_config_concat(session, sourcecfg, &sourceconf));

	WT_ERR(__wt_schema_create(session, source, sourceconf));

	cfg[1] = sourceconf;
	cfg[2] = confbuf.data;
	WT_ERR(__wt_config_collapse(session, cfg, &idxconf));
	if ((ret = __wt_metadata_insert(session, name, idxconf)) != 0) {
		/*
		 * If the entry already exists in the metadata, we're done.
		 * This is an error for exclusive creates but okay otherwise.
		 */
		if (ret == WT_DUPLICATE_KEY)
			ret = exclusive ? EEXIST : 0;
		goto err;
	}

err:	__wt_free(session, idxconf);
	__wt_free(session, sourceconf);
	__wt_buf_free(session, &confbuf);
	__wt_buf_free(session, &extra_cols);
	__wt_buf_free(session, &fmt);
	__wt_buf_free(session, &namebuf);

	__wt_schema_release_table(session, table);
	return (ret);
}

static int
__create_table(WT_SESSION_IMPL *session,
    const char *name, int exclusive, const char *config)
{
	WT_CONFIG conf;
	WT_CONFIG_ITEM cgkey, cgval, cval;
	WT_DECL_RET;
	WT_TABLE *table;
	size_t cgsize;
	int ncolgroups;
	char *cgname;
	const char *cfg[4] =
	    { WT_CONFIG_BASE(session, table_meta), config, NULL, NULL };
	const char *tableconf, *tablename;

	cgname = NULL;
	table = NULL;
	tableconf = NULL;

	tablename = name;
	if (!WT_PREFIX_SKIP(tablename, "table:"))
		return (EINVAL);

	if ((ret = __wt_schema_get_table(session,
	    tablename, strlen(tablename), 0, &table)) == 0) {
		__wt_schema_release_table(session, table);
		return (exclusive ? EEXIST : 0);
	}
	WT_RET_NOTFOUND_OK(ret);

	WT_RET(__wt_config_gets(session, cfg, "colgroups", &cval));
	WT_RET(__wt_config_subinit(session, &conf, &cval));
	for (ncolgroups = 0;
	    (ret = __wt_config_next(&conf, &cgkey, &cgval)) == 0;
	    ncolgroups++)
		;
	WT_RET_NOTFOUND_OK(ret);

	WT_RET(__wt_config_collapse(session, cfg, &tableconf));
	if ((ret = __wt_metadata_insert(session, name, tableconf)) != 0) {
		/*
		 * If the entry already exists in the metadata, we're done.
		 * This is an error for exclusive creates but okay otherwise.
		 */
		if (ret == WT_DUPLICATE_KEY)
			ret = exclusive ? EEXIST : 0;
		goto err;
	}

	/* Attempt to open the table now to catch any errors. */
	WT_ERR(__wt_schema_get_table(
	    session, tablename, strlen(tablename), 1, &table));

	if (ncolgroups == 0) {
		cgsize = strlen("colgroup:") + strlen(tablename) + 1;
		WT_ERR(__wt_calloc_def(session, cgsize, &cgname));
		snprintf(cgname, cgsize, "colgroup:%s", tablename);
		WT_ERR(__create_colgroup(session, cgname, exclusive, config));
	}

	if (0) {
err:		if (table != NULL) {
			__wt_schema_remove_table(session, table);
			table = NULL;
		}
	}
	if (table != NULL)
		__wt_schema_release_table(session, table);
	__wt_free(session, cgname);
	__wt_free(session, tableconf);
	return (ret);
}

static int
__create_data_source(WT_SESSION_IMPL *session,
    const char *uri, int exclusive, const char *config, WT_DATA_SOURCE *dsrc)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	const char *cfg[4], *fileconf;

	fileconf = NULL;

	/* Check if the data-source already exists. */
	if ((ret =
	    __wt_metadata_read(session, uri, &fileconf)) != WT_NOTFOUND) {
		__wt_free(session, fileconf);
		if (exclusive)
			WT_TRET(EEXIST);
		return (ret);
	}

	/*
	 * User-specified collators aren't supported for data-source objects.
	 */
	if (__wt_config_getones(
	    session, config, "collator", &cval) != WT_NOTFOUND)
		WT_RET_MSG(session, EINVAL,
		    "WT_DATA_SOURCE objects do not support WT_COLLATOR "
		    "ordering");

	/*
	 * Set a default key/value format, and insert the configuration into
	 * the metadata.
	 *
	 * XXX
	 * Use the session_create information, even though it includes a ton of
	 * things we don't care about (like checksum configuration).  We should
	 * be stripping that information out.
	 */
	cfg[0] = WT_CONFIG_BASE(session, session_create);
	cfg[1] = "key_format=u,value_format=u";
	cfg[2] = config;
	cfg[3] = NULL;
	WT_RET(__wt_config_collapse(session, cfg, &fileconf));
	if ((ret = __wt_metadata_insert(session, uri, fileconf)) == 0) {
		cfg[0] = fileconf;
		cfg[1] = NULL;
		WT_ERR(dsrc->create(
		    dsrc, &session->iface, uri, (WT_CONFIG_ARG *)cfg));
	} else if (ret == WT_DUPLICATE_KEY)
		ret = EEXIST;

	if (0) {
err:		WT_TRET(__wt_metadata_remove(session, uri));
	}

	__wt_free(session, fileconf);
	return (ret);
}

int
__wt_schema_create(
    WT_SESSION_IMPL *session, const char *uri, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_DATA_SOURCE *dsrc;
	WT_DECL_RET;
	int exclusive;

	exclusive = (
	    __wt_config_getones(session, config, "exclusive", &cval) == 0 &&
	    cval.val != 0);

	/*
	 * We track create operations: if we fail in the middle of creating a
	 * complex object, we want to back it all out.
	 */
	WT_RET(__wt_meta_track_on(session));

	if (WT_PREFIX_MATCH(uri, "colgroup:"))
		ret = __create_colgroup(session, uri, exclusive, config);
	else if (WT_PREFIX_MATCH(uri, "file:"))
		ret = __create_file(session, uri, exclusive, config);
	else if (WT_PREFIX_MATCH(uri, "lsm:"))
		ret = __wt_lsm_tree_create(session, uri, exclusive, config);
	else if (WT_PREFIX_MATCH(uri, "index:"))
		ret = __create_index(session, uri, exclusive, config);
	else if (WT_PREFIX_MATCH(uri, "table:"))
		ret = __create_table(session, uri, exclusive, config);
	else if ((ret = __wt_schema_get_source(session, uri, &dsrc)) == 0)
		ret = dsrc->create == NULL ?
		    __wt_object_unsupported(session, uri) :
		    __create_data_source(session, uri, exclusive, config, dsrc);

	session->dhandle = NULL;
	WT_TRET(__wt_meta_track_off(session, ret != 0));

	return (ret);
}
