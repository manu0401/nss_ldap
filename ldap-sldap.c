/* Copyright (C) 1997-2006 Luke Howard.
   This file is part of the nss_ldap library.
   Contributed by Luke Howard, <lukeh@padl.com>, 2006.

   The nss_ldap library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The nss_ldap library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the nss_ldap library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.

   $Id$
 */


static char rcsId[] =
  "$Id$";

#include "config.h"

#ifdef HAVE_PORT_BEFORE_H
#include <port_before.h>
#endif

#if defined(HAVE_THREAD_H) && !defined(_AIX)
#include <thread.h>
#elif defined(HAVE_PTHREAD_H)
#include <pthread.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>

#ifdef HAVE_LBER_H
#include <lber.h>
#endif
#ifdef HAVE_LDAP_H
#include <ldap.h>
#endif

#include <sys/types.h>
#include <sys/socket.h>
#ifdef HAVE_NET_ROUTE_H
#include <net/route.h>
#endif
#include <net/if.h>
#include <netinet/in.h>

#include "ldap-nss.h"
#include "ldap-sldap.h"
#include "util.h"

#ifdef HAVE_PORT_AFTER_H
#include <port_after.h>
#endif

#ifdef HAVE_NSSWITCH_H

/*
 * This implements enough of the Solaris libsldap interface in order
 * for the automounter to work.
 */

static void **
__ns_ldap_makeStringParam(const char *string)
{
	void **p;

	p = (void **)malloc(2 * sizeof(void *));
	if (p == NULL) {
		return NULL;
	}
	p[0] = strdup(string);
	if (p[0] == NULL) {
		free(p);
		return NULL;
	}
	p[1] = NULL;

	return p;
}

char **
__ns_ldap_getMappedAttributes(const char *service, const char *attribute)
{
	const char *mapped;

	mapped = _nss_ldap_map_at(_nss_ldap_str2selector(service), attribute);
	if (mapped == NULL) {
		return NULL;
	}

	return (char **)__ns_ldap_makeStringParam(mapped);
}

char **
__ns_ldap_getMappedObjectClass(const char *service, const char *objectClass)
{
	const char *mapped;

	mapped = _nss_ldap_map_oc(objectClass);
	if (mapped == NULL) {
		return NULL;
	}

	return (char **)__ns_ldap_makeStringParam(mapped);
}

static ns_ldap_return_code
__ns_ldap_mapError(NSS_STATUS error)
{
	ns_ldap_return_code code;

	switch (error) {
	case NSS_SUCCESS:
		code = NS_LDAP_SUCCESS;
		break;
	case NSS_TRYAGAIN:
		code = NS_LDAP_MEMORY;
		break;
	case NSS_NOTFOUND:
		code = NS_LDAP_NOTFOUND;
		break;
	case NSS_UNAVAIL:
	default:
		code = NS_LDAP_OP_FAILED;
		break;
	}

	return code;
}

static NSS_STATUS
__ns_ldap_mapErrorDetail(ns_ldap_error_t **errorp)
{
	char *m = NULL;
	char *s = NULL;

	*errorp = (ns_ldap_error_t *)calloc(1, sizeof(ns_ldap_error_t));
	if (*errorp == NULL) {
		return NSS_TRYAGAIN;
	}

	(*errorp)->status = _nss_ldap_get_ld_errno(&m, &s);
	(*errorp)->message = (m != NULL) ? strdup(m) : NULL;

	return NSS_SUCCESS;
}

ns_ldap_return_code
__ns_ldap_freeError(ns_ldap_error_t **errorp)
{
	if (errorp == NULL) {
		return NS_LDAP_INVALID_PARAM;
	}
	if (*errorp != NULL) {
		if ((*errorp)->message != NULL) {
			free((*errorp)->message);
			(*errorp)->message = NULL;
		}
		free(*errorp);
		*errorp = NULL;
	}
	return NS_LDAP_SUCCESS;
}

ns_ldap_return_code
__ns_ldap_freeParam(void ***data)
{
	void **p;

	if (*data != NULL) {
		for (p = *data; *p != NULL; p++) {
			free(*p);
			*p = NULL;
		}
		free(*data);
		*data = NULL;
	}

	return NS_LDAP_SUCCESS;
}


ns_ldap_return_code
__ns_ldap_getParam(const ParamIndexType type, void ***data, ns_ldap_error_t **errorp)
{
	*errorp = NULL;

	switch (type) {
	case NS_LDAP_FILE_VERSION_P:
		*data = __ns_ldap_makeStringParam(NS_LDAP_VERSION);
		return NS_LDAP_SUCCESS;
	default:
	}
	return NS_LDAP_INVALID_PARAM;
}

ns_ldap_return_code
__ns_ldap_freeAttr(ns_ldap_attr_t **pattr)
{
	int i;
	ns_ldap_attr_t *attr = *pattr;

	if (attr != NULL) {
		if (attr->attrname != NULL) {
			free(attr->attrname);
		}
		if (attr->attrvalue != NULL) {
			for (i = 0; i < attr->value_count; i++) {
				free(attr->attrvalue[i]);
			}
			free(attr->attrvalue);
		}
	}

	return NS_LDAP_SUCCESS;
}

ns_ldap_return_code
__ns_ldap_freeEntry(ns_ldap_entry_t **pentry)
{
	int i;
	ns_ldap_entry_t *entry = *pentry;

	if (entry != NULL) {
		if (entry->attr_pair != NULL) {
			for (i = 0; i < entry->attr_count; i++) {
				__ns_ldap_freeAttr(&entry->attr_pair[i]);
			}
			free(entry->attr_pair);
		}
		free(entry);
		*pentry = NULL;
	}

	return NS_LDAP_SUCCESS;
}

ns_ldap_return_code
__ns_ldap_freeResult(ns_ldap_result_t **presult)
{
	ns_ldap_result_t *result;
	ns_ldap_entry_t *entry, *next = NULL;

	if (presult == NULL) {
		return NS_LDAP_INVALID_PARAM;
	}

	result = *presult;
	if (result == NULL) {
		return NS_LDAP_SUCCESS;
	}

	while (entry != NULL) {
		next = entry->next;
		__ns_ldap_freeEntry(&entry);
		entry = next;
	}

	free(result);
	*presult = NULL;

	return NS_LDAP_SUCCESS;
}

ns_ldap_return_code
__ns_ldap_parseAttr(ns_ldap_cookie_t *cookie, LDAPMessage *entry, const char *attribute, ns_ldap_attr_t **pattr)
{
	ns_ldap_attr_t *attr;
	const char *unmappedAttribute;

	attr = (ns_ldap_attr_t *)malloc(sizeof(*attr));
	if (attr == NULL) {
		return NS_LDAP_MEMORY;
	}

	unmappedAttribute = _nss_ldap_unmap_at(cookie->sel, attribute);
	if (unmappedAttribute == NULL) {
		return NS_LDAP_INVALID_PARAM;
	}

	attr->attrname = strdup(unmappedAttribute);
	if (attr->attrname == NULL) {
		__ns_ldap_freeAttr(&attr);
		return NS_LDAP_MEMORY;
	}

	attr->attrvalue = _nss_ldap_get_values(entry, attr->attrname);
	attr->value_count = (attr->attrvalue != NULL) ? ldap_count_values(attr->attrvalue) : 0;

	*pattr = attr;

	return NS_LDAP_SUCCESS;
}

NSS_STATUS
__ns_ldap_parseEntry(LDAPMessage *msg, ldap_state_t *state,
	void *result, char *buffer, size_t buflen)
{
	ns_ldap_cookie_t *cookie = (ns_ldap_cookie_t *)result;
	char *attribute;
	BerElement *ber = NULL;
	ns_ldap_return_code ret = NS_LDAP_SUCCESS;
	ns_ldap_entry_t *entry;
	int attr_count = 0;

	entry = (ns_ldap_entry_t *)malloc(sizeof(*entry));
	if (entry == NULL) {
		cookie->ret = NS_LDAP_MEMORY;
		return NSS_NOTFOUND;
	}

	entry->attr_count = 0;
	entry->attr_pair = NULL;
	entry->next = NULL;

	for (attribute = _nss_ldap_first_attribute (msg, &ber);
		attribute != NULL;
		attribute = _nss_ldap_next_attribute (msg, ber)) {
		attr_count++;
#ifdef HAVE_LDAP_MEMFREE
		ldap_memfree(attribute);
#endif
	}

	if (ber != NULL)
		ber_free (ber, 0);

	entry->attr_pair = (ns_ldap_attr_t **)calloc(attr_count, sizeof(ns_ldap_attr_t *));
	if (entry->attr_pair == NULL) {
		__ns_ldap_freeEntry(&entry);
		cookie->ret = NS_LDAP_MEMORY;
		return NSS_NOTFOUND;
	}

	for (attribute = _nss_ldap_first_attribute (msg, &ber);
		attribute != NULL;
		attribute = _nss_ldap_next_attribute (msg, ber)) {
		ns_ldap_attr_t *attr;

		ret = __ns_ldap_parseAttr(cookie, msg, attribute, &attr);
#ifdef HAVE_LDAP_MEMFREE
		ldap_memfree(attribute);
#endif
		if (ret != NS_LDAP_SUCCESS) {
			continue;
		}
		entry->attr_pair[entry->attr_count++] = attr;
	}

	if (ber != NULL)
		ber_free (ber, 0);

	if (ret == NS_LDAP_SUCCESS) {
		entry->next = cookie->entry;
		cookie->entry = entry;

		cookie->result->entries_count++;
	} else {
		__ns_ldap_freeEntry(&entry);
	}

	if (cookie->result->entry == NULL) {
		cookie->result->entry = entry;
	}

	return __ns_ldap_mapError(ret);
}

static ns_ldap_return_code
__ns_ldap_initResult(ns_ldap_result_t **presult)
{
	ns_ldap_result_t *result;

	result = (ns_ldap_result_t *)malloc(sizeof(ns_ldap_result_t));
	if (result == NULL) {
		return NS_LDAP_MEMORY;
	}

	result->entries_count = 0;
	result->entry = NULL;

	*presult = result;

	return NS_LDAP_SUCCESS;
}

ns_ldap_return_code
__ns_ldap_list(
	const char *map,
	const char *filter,
	int (*init_filter_cb)(const ns_ldap_search_desc_t *desc, char **realfilter, const void *userdata),
	const char * const *attribute,
	const ns_cred_t *cred,
	const int flags,
	ns_ldap_result_t **result,
	ns_ldap_error_t **errorp,
	int (*callback)(const ns_ldap_entry_t *entry, const void *userdata),
	const void *userdata)
{
	ns_ldap_cookie_t cookie;
/*	ldap_args_t a; */
	ent_context_t *ctx = NULL;
	NSS_STATUS stat;
	int action = NS_LDAP_CB_NEXT;

	/*
	 * XXX for automounts (shall we assume all maps are automounts given
	 * the purpose of this? Sun looks for anything starting with auto but
	 * customer may have maps starting with different prefixes) we need
	 * to do the following:
	 *
	 *	determine set of search bases for automounts (as for Linux)
	 *	parse the filter into mapped attributes - filter is always
	 * 	setmntent()
	 *
	 */

	cookie.sel = _nss_ldap_str2selector(map);
	cookie.ret = -1;
	cookie.erange = 0;
	cookie.result = NULL;
	cookie.entry = NULL;

	cookie.ret = __ns_ldap_initResult(&cookie.result);
	if (cookie.ret != NS_LDAP_SUCCESS) {
		return cookie.ret;
	}

	_nss_ldap_enter();

	do {
		stat = _nss_ldap_getent_ex(NULL, &ctx,
			(void *)&cookie, NULL, 0,
			&cookie.erange, filter, cookie.sel,
			attribute, __ns_ldap_parseEntry);
		if (stat != NSS_SUCCESS) {
			break;
		}

		if (callback != NULL) {
			action = (*callback)(cookie.entry, userdata);
		}
	} while (action == NS_LDAP_CB_NEXT);

	_nss_ldap_leave();

	if (cookie.ret != NS_LDAP_SUCCESS) {
		__ns_ldap_freeResult(&cookie.result);
	} else {
		*result = cookie.result;
	}
	__ns_ldap_mapErrorDetail(errorp);

	return (cookie.ret < 0) ? __ns_ldap_mapError(stat) : cookie.ret;
}

ns_ldap_return_code
__ns_ldap_err2str(ns_ldap_return_code err, char **strmsg)
{
	switch (err) {
	case NS_LDAP_SUCCESS:
	case NS_LDAP_SUCCESS_WITH_INFO:
		*strmsg = "Success";
		break;
	case NS_LDAP_OP_FAILED:
		*strmsg = "Operation failed";
		break;
	case NS_LDAP_NOTFOUND:
		*strmsg = "Not found";
		break;
	case NS_LDAP_MEMORY:
		*strmsg = "Out of memory";
		break;
	case NS_LDAP_CONFIG:
		*strmsg = "Configuration error";
		break;
	case NS_LDAP_PARTIAL:
		*strmsg = "Partial results received";
		break;
	case NS_LDAP_INTERNAL:
		*strmsg = "Internal LDAP error";
		break;
	case NS_LDAP_INVALID_PARAM:
		*strmsg = "Invalid parameter";
		break;
	default:
		*strmsg = "Unknown error";
		return NS_LDAP_INVALID_PARAM;
		break;
	}

	return NS_LDAP_SUCCESS;
}

#endif /* HAVE_NSSWITCH_H */
