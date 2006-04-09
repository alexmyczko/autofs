/*
   Copyright 2005 Red Hat, Inc.
   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:


    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Red Hat, Inc., nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 *  cyrus-sasl.c
 *
 *  Description:
 *
 *  This file implements SASL authentication to an LDAP server for the
 *  following mechanisms:
 *    GSSAPI, EXTERNAL, ANONYMOUS, PLAIN, DIGEST-MD5, KERBEROS_V5
 *  The mechanism to use is specified in an external file,
 *  LDAP_AUTH_CONF_FILE.  See the samples directory in the autofs
 *  distribution for an example configuration file.
 *
 *  This file is written with the intent that it will work with both the
 *  openldap and the netscape ldap client libraries.
 *
 *  Author: Nalin <nalin@redhat.com>
 *  Modified by Jeff <jmoyer@redhat.com> to adapt it to autofs.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ldap.h>
#include <sasl/sasl.h>

#include "automount.h"
#include "lookup_ldap.h"

#ifndef LDAP_OPT_RESULT_CODE
#ifdef  LDAP_OPT_ERROR_NUMBER
#define LDAP_OPT_RESULT_CODE LDAP_OPT_ERROR_NUMBER
#else
#error "Could not determine the proper value for LDAP_OPT_RESULT_CODE."
#endif
#endif

static int sasl_log_func(void *, int, const char *);
static int getpass_func(sasl_conn_t *, void *, int, sasl_secret_t **);
static int getuser_func(void *, int, const char **, unsigned *);

static sasl_callback_t callbacks[] = {
	{ SASL_CB_LOG, &sasl_log_func, NULL },
	{ SASL_CB_USER, &getuser_func, NULL },
	{ SASL_CB_AUTHNAME, &getuser_func, NULL },
	{ SASL_CB_PASS, &getpass_func, NULL },
	{ SASL_CB_LIST_END, NULL, NULL },
};

static char *sasl_auth_id, *sasl_auth_secret;
sasl_secret_t *sasl_secret;

static int
sasl_log_func(void *context, int level, const char *message)
{
	switch (level) {
	case SASL_LOG_ERR:
	case SASL_LOG_FAIL:
		error("%s", message);
		break;
	case SASL_LOG_WARN:
		warn("%s", message);
		break;
	case SASL_LOG_NOTE:
		info("%s", message);
		break;
	case SASL_LOG_DEBUG:
	case SASL_LOG_TRACE:
	case SASL_LOG_PASS:
		debug("%s", message);
		break;
	default:
		break;
	}

	return SASL_OK;
}

static int
getuser_func(void *context, int id, const char **result, unsigned *len)
{
	debug("called with context %p, id %d.", context, id);

	switch (id) {
	case SASL_CB_USER:
	case SASL_CB_AUTHNAME:
		*result = sasl_auth_id;
		if (len)
			*len = strlen(sasl_auth_id);
		break;
	default:
		error("unknown id in request: %d", id);
		return SASL_FAIL;
	}

	return SASL_OK;
}

/*
 *  This function creates a sasl_secret_t from the credentials
 *  specefied in sasl_init.  sasl_client_auth can return SASL_OK or
 *  SASL_NOMEM.  We simply propagate this return value to the caller.
 */
static int
getpass_func(sasl_conn_t *conn, void *context, int id, sasl_secret_t **psecret)
{
	int len = strlen(sasl_auth_secret);

	debug("context %p, id %d", context, id);

	*psecret = (sasl_secret_t *) malloc(sizeof(sasl_secret_t) + len);
	if (!*psecret)
		return SASL_NOMEM;

	(*psecret)->len = strlen(sasl_auth_secret);
	strncpy((char *)(*psecret)->data, sasl_auth_secret, len);

	return SASL_OK;
}

/*
 *  retrieves the supportedSASLmechanisms from the LDAP server.
 *
 *  Return Value: the result of ldap_get_values on success, NULL on failure.
 *                The caller is responsible for calling ldap_value_free on
 *                the returned data.
 */
char **
get_server_SASL_mechanisms(LDAP *ld)
{
	int ret;
	const char *saslattrlist[] = {"supportedSASLmechanisms", NULL};
	LDAPMessage *results = NULL, *entry;
	char **mechanisms;

	ret = ldap_search_ext_s(ld, "", LDAP_SCOPE_BASE, "(objectclass=*)",
				(char **)saslattrlist, 0,
				NULL, NULL,
				NULL, LDAP_NO_LIMIT, &results);
	if (ret != LDAP_SUCCESS) {
		debug("%s", ldap_err2string(ret));
		return NULL;
	}

	entry = ldap_first_entry(ld, results);
	if (entry == NULL) {
		/* No root DSE. (!) */
		ldap_msgfree(results);
		debug("a lookup of \"supportedSASLmechanisms\" returned "
		      "no results.");
		return NULL;
	}

	mechanisms = ldap_get_values(ld, entry, "supportedSASLmechanisms");
	ldap_msgfree(results);
	if (mechanisms == NULL) {
		/* Well, that was a waste of time. */
		info("No SASL authentication mechanisms are supported"
		     " by the LDAP server.\n");
		return NULL;
	}

	return mechanisms;
}

/*
 *  Returns 0 upon successful connect, -1 on failure.
 */
int
do_sasl_bind(LDAP *ld, sasl_conn_t *conn, const char **clientout,
	     unsigned int *clientoutlen, const char *auth_mech, int sasl_result)
{
	int ret, msgid, bind_result;
	struct berval client_cred, *server_cred, temp_cred;
	LDAPMessage *results;
	int have_data, expected_data;

	do {
		/* Take whatever client data we have and send it to the
		 * server. */
		client_cred.bv_val = (char *)*clientout;
		client_cred.bv_len = *clientoutlen;
		ret = ldap_sasl_bind(ld, NULL, auth_mech,
				     (client_cred.bv_len > 0) ?
				     &client_cred : NULL,
				     NULL, NULL, &msgid);
		if (ret != LDAP_SUCCESS) {
			crit("Error sending sasl_bind request to "
			     "the server: %s", ldap_err2string(ret));
			return -1;
		}

		/* Wait for a result message for this bind request. */
		results = NULL;
		ret = ldap_result(ld, msgid, LDAP_MSG_ALL, NULL, &results);
		if (ret != LDAP_RES_BIND) {
			crit("Error while waiting for response to "
			     "sasl_bind request: %s", ldap_err2string(ret));
			return -1;
		}

		/* Retrieve the result code for the bind request and
		 * any data which the server sent. */
		server_cred = NULL;
		ret = ldap_parse_sasl_bind_result(ld, results,
						  &server_cred, 0);
		ldap_msgfree(results);

		/* Okay, here's where things get tricky.  Both
		 * Mozilla's LDAP SDK and OpenLDAP store the result
		 * code which was returned by the server in the
		 * handle's ERROR_NUMBER option.  Mozilla returns
		 * LDAP_SUCCESS if the data was parsed correctly, even
		 * if the result was an error, while OpenLDAP returns
		 * the result code.  I'm leaning toward Mozilla being
		 * more correct.
		 * In either case, we stuff the result into bind_result.
		 */
		if (ret == LDAP_SUCCESS) {
			/* Mozilla? */
			ret = ldap_get_option(ld, LDAP_OPT_RESULT_CODE,
					      &bind_result);
			if (ret != LDAP_SUCCESS) {
				crit("Error retrieving response to sasl_bind "
				     "request: %s", ldap_err2string(ret));
				ret = -1;
				break;
			}
		} else {
			/* OpenLDAP? */
			switch (ret) {
			case LDAP_SASL_BIND_IN_PROGRESS:
				bind_result = ret;
				break;
			default:
				warn("Error parsing response to sasl_bind "
				     "request: %s.", ldap_err2string(ret));
				break;
			}
		}

		/*
		 * The LDAP server is supposed to send a NULL value for
		 * server_cred if there was no data.  However, *some*
		 * server implementations get this wrong, and instead send
		 * an empty string.  We check for both.
		 */
		have_data = server_cred != NULL && server_cred->bv_len > 0;

		/*
		 * If the result of the sasl_client_start is SASL_CONTINUE,
		 * then the server should have sent us more data.
		 */
		expected_data = sasl_result == SASL_CONTINUE;

		if (have_data && !expected_data) {
			warn("The LDAP server sent data in response to our "
			     "bind request, but indicated that the bind was "
			     "complete. LDAP SASL bind with mechansim %s "
			     "failed.", auth_mech);
			//debug(""); /* dump out the data we got */
			ret = -1;
			break;
		}
		if (expected_data && !have_data) {
			warn("The LDAP server indicated that the LDAP SASL "
			     "bind was incomplete, but did not provide the "
			     "required data to proceed. LDAP SASL bind with "
			     "mechanism %s failed.", auth_mech);
			ret = -1;
			break;
		}

		/* If we need another round trip, process whatever we
		 * received and prepare data to be transmitted back. */
		if ((sasl_result == SASL_CONTINUE) &&
		    ((bind_result == LDAP_SUCCESS) ||
		     (bind_result == LDAP_SASL_BIND_IN_PROGRESS))) {
			if (server_cred != NULL) {
				temp_cred = *server_cred;
			} else {
				temp_cred.bv_len = 0;
				temp_cred.bv_val = NULL;
			}
			sasl_result = sasl_client_step(conn,
						       temp_cred.bv_val,
						       temp_cred.bv_len,
						       NULL,
						       clientout,
						       clientoutlen);
			/* If we have data to send, then the server
			 * had better be expecting it.  (It's valid
			 * to send the server no data with a request.)
			 */
			if ((*clientoutlen > 0) &&
			    (bind_result != LDAP_SASL_BIND_IN_PROGRESS)) {
				printf("We have data for the server, "
				       "but it thinks we are done!");
				/* XXX should print out debug data here */
				ret = -1;
			}
		}

		if (server_cred && server_cred->bv_len > 0)
			ber_bvfree(server_cred);

	} while ((bind_result == LDAP_SASL_BIND_IN_PROGRESS) ||
		 (sasl_result == SASL_CONTINUE));

	if (server_cred && server_cred->bv_len > 0)
		ber_bvfree(server_cred);

	return ret;
}


sasl_conn_t *
sasl_bind_mech(LDAP *ldap, const char *mech)
{
	sasl_conn_t *conn;
	char *tmp, *host = NULL;
	const char *clientout;
	unsigned int clientoutlen;
	const char *chosen_mech;
	int result;

	result = ldap_get_option(ldap, LDAP_OPT_HOST_NAME, &host);
	if (result != LDAP_SUCCESS) {
		debug("failed to get hostname for connection");
		return NULL;
	}
	if ((tmp = strchr(host, ':')))
		*tmp = '\0';

	/* Create a new authentication context for the service. */
	result = sasl_client_new("ldap", host, NULL, NULL, NULL, 0, &conn);
	if (result != SASL_OK) {
		ldap_memfree(host);
		return NULL;
	}

	chosen_mech = NULL;
	result = sasl_client_start(conn, mech, NULL,
				&clientout, &clientoutlen, &chosen_mech);

	/* OK and CONTINUE are the only non-fatal return codes here. */
	if ((result != SASL_OK) && (result != SASL_CONTINUE)) {
		debug("%s", sasl_errdetail(conn));
		ldap_memfree(host);
		if (conn)
			sasl_dispose(&conn);
		return NULL;
	}

	result = do_sasl_bind(ldap, conn,
			 &clientout, &clientoutlen, chosen_mech, result);
	if (result == 0) {
		ldap_memfree(host);
		return conn;
	}

	ldap_memfree(host);

	/* sasl bind failed */
	sasl_dispose(&conn);

	return NULL;
}

/*
 *  Returns 0 if a suitable authentication mechanism is available.  Returns
 *  -1 on error or if no mechanism is supported by both client and server.
 */
int
sasl_choose_mech(struct lookup_context *ctxt, char **mechanism)
{
	LDAP *ldap;
	sasl_conn_t *conn;
	int authenticated;
	int i;
	char **mechanisms;

	/* TODO: what's this here for ? */
	*mechanism = NULL;

	ldap = ldap_connection_init(ctxt);
	if (!ldap)
		return -1;

	mechanisms = get_server_SASL_mechanisms(ldap);
	if (!mechanisms) {
		ldap_unbind_connection(ldap, ctxt);
		return -1;
	}

	/* Try each supported mechanism in turn. */
	authenticated = 0;
	for (i = 0; mechanisms[i] != NULL; i++) {
		/*
		 *  This routine is called if there is no configured
		 *  mechanism.  As such, we can skip over any auth
		 *  mechanisms that require user credentials.  These include
		 *  PLAIN and DIGEST-MD5.
		 */
		if (authtype_requires_creds(mechanisms[i]))
			continue;

		conn = sasl_bind_mech(ldap, mechanisms[i]);
		if (conn) {
			sasl_dispose(&conn);
			authenticated = 1;
			break;
		}
	}

	ldap_value_free(mechanisms);
	return (authenticated > 0) ? 0 : -1;
}

int
sasl_init(char *id, char *secret)
{
	/* Start up Cyrus SASL--only needs to be done once. */
	if (sasl_client_init(callbacks) != SASL_OK)
		return -1;

	sasl_auth_id = id;
	sasl_auth_secret = secret;

	return 0;
}
